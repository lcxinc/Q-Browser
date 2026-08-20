#include "PackageStore.h"
#include "Archive.h"
#include "CandidateMemberKeys.h"
#include "CanonicalArchivePath.h"
#include "ContentDigest.h"
#include "PackageInstallerTestHooks.h"
#include "PackageStoreTestHooks.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <optional>
#include <memory>

namespace
{
const QRegularExpression AppIdPattern(
    QStringLiteral(R"(\A[a-z0-9]+(?:[.-][a-z0-9]+)*\z)"));
const QRegularExpression VersionPattern(
    QStringLiteral(R"(\A(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\z)"));
const QRegularExpression DigestPattern(QStringLiteral(R"(\A[0-9a-f]{64}\z)"));

PackageStoreResult failure(
    const PackageStoreError error,
    const QString &message)
{
    return {error, {}, message};
}

ActivationStateResult stateFailure(
    const PackageStoreError error,
    const QString &message)
{
    return {error, {}, message};
}

bool validAppId(const QString &appId)
{
    return appId.size() <= 128 && AppIdPattern.match(appId).hasMatch();
}

bool validVersionDirectory(const QString &directory)
{
    constexpr qsizetype SeparatorAndDigestSize = 65;
    if (directory.size() <= SeparatorAndDigestSize) {
        return false;
    }
    const qsizetype separator = directory.size() - SeparatorAndDigestSize;
    return directory.at(separator) == QLatin1Char('-')
        && VersionPattern.match(directory.first(separator)).hasMatch()
        && DigestPattern.match(directory.sliced(separator + 1)).hasMatch();
}

std::unique_ptr<QLockFile> acquireActivationTransactionLock(
    const QString &applicationRoot)
{
    auto lock = std::make_unique<QLockFile>(
        applicationRoot + QStringLiteral("/.activation.lock"));
    lock->setStaleLockTime(30000);
#ifdef Q_BROWSER_PACKAGE_STORE_TESTING
    if (qbrowser_package_store_testing::packageStoreTestHooks()
            .beforeActivationLockAttempt) {
        qbrowser_package_store_testing::packageStoreTestHooks()
            .beforeActivationLockAttempt(
                applicationRoot,
                lock->staleLockTime(),
                5000);
    }
#endif
    if (!lock->tryLock(5000)) {
        return {};
    }
    return lock;
}

bool existingPlainDirectory(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isDir() && !info.isSymLink();
}

bool makeVersionFilesReadOnly(const QString &root)
{
    QDirIterator iterator(
        root,
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!QFile::setPermissions(
                path,
                QFileDevice::ReadOwner | QFileDevice::ReadGroup
                    | QFileDevice::ReadOther)) {
            return false;
        }
    }
    return true;
}

bool pathIsWithin(const QString &root, const QString &candidate)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanCandidate = QDir::cleanPath(candidate);
#ifdef Q_OS_WIN
    return cleanCandidate.compare(cleanRoot, Qt::CaseInsensitive) == 0
        || cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)
        || cleanCandidate.startsWith(cleanRoot + QLatin1Char('\\'), Qt::CaseInsensitive);
#else
    return cleanCandidate == cleanRoot
        || cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'));
#endif
}

QString candidateMemberKey(
    const QString &root,
    const QString &path,
    const bool directory)
{
    QString relative = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(path));
    relative = qbrowser_package_detail::canonicalCandidatePath(relative);
    return (directory ? QStringLiteral("d:") : QStringLiteral("f:")) + relative;
}

qsizetype candidateDepth(const QString &relative)
{
    return relative.split(QLatin1Char('/'), Qt::SkipEmptyParts).size();
}

std::optional<QSet<QString>> candidateMemberSet(
    const QString &root,
    const QSet<QString> &expectedMembers,
    const qsizetype maximumDepth,
    const ArchiveLimits &limits)
{
    QSet<QString> result;
    QSet<QString> canonicalPaths;
    QDirIterator iterator(
        root,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink() || (!info.isDir() && !info.isFile())) {
            return std::nullopt;
        }
        const QString relative = QDir::fromNativeSeparators(
            QDir(root).relativeFilePath(path));
        const QString key = candidateMemberKey(root, path, info.isDir());
        if (result.size() >= expectedMembers.size()
            || result.contains(key)
            || !qbrowser_package_detail::insertCanonicalCandidatePath(
                canonicalPaths, relative)
            || !expectedMembers.contains(key)
            || candidateDepth(relative) > maximumDepth
            || !qbrowser_archive_detail::validateArchivePath(
                    relative.toUtf8(), limits)
                    .has_value()) {
            return std::nullopt;
        }
        result.insert(key);
    }
    return result;
}

void addExpectedParentDirectories(
    const QString &relativeFile,
    QSet<QString> &expectedDirectories)
{
    QString parent = QFileInfo(relativeFile).path();
    while (!parent.isEmpty() && parent != QLatin1String(".")) {
        expectedDirectories.insert(
            qbrowser_package_detail::canonicalCandidatePath(parent));
        parent = QFileInfo(parent).path();
    }
}

#ifdef Q_OS_WIN
bool openStableAppTree(
    const QString &storeRoot,
    const QString &applicationRoot,
    qbrowser_archive_detail::WindowsStableDirectoryTree &tree)
{
    return tree.openRoot(storeRoot)
        && tree.addExistingDirectory(storeRoot + QStringLiteral("/apps"))
        && tree.addExistingDirectory(applicationRoot)
        && tree.addExistingDirectory(applicationRoot + QStringLiteral("/versions"))
        && tree.isStable();
}

bool pinStateTargets(
    const QString &applicationRoot,
    const ActivationState &state,
    qbrowser_archive_detail::WindowsStableDirectoryTree &tree)
{
    const QStringList targets{
        state.current, state.previous, state.lastKnownGood};
    for (const QString &target : targets) {
        if (!target.isEmpty()
            && !tree.addImmutableDirectory(
                applicationRoot + QStringLiteral("/versions/") + target)) {
            return false;
        }
    }
    return tree.isStable();
}
#endif
}

PackageStore::PackageStore(QString storeRoot)
    : m_root(QDir::cleanPath(QFileInfo(std::move(storeRoot)).absoluteFilePath()))
{
}

const QString &PackageStore::root() const noexcept
{
    return m_root;
}

QString PackageStore::appRoot(const QString &appId) const
{
    if (!validAppId(appId)) {
        return {};
    }
    return QDir::cleanPath(m_root + QStringLiteral("/apps/") + appId);
}

QString PackageStore::versionPath(
    const QString &appId,
    const QString &versionDirectory) const
{
    if (!validAppId(appId)
        || !validVersionDirectory(versionDirectory)) {
        return {};
    }
    return QDir::cleanPath(
        appRoot(appId) + QStringLiteral("/versions/") + versionDirectory);
}

bool PackageStore::ensureAppDirectories(const QString &appId) const
{
    if (!validAppId(appId) || m_root.isEmpty()) {
        return false;
    }
    const QString versions = appRoot(appId) + QStringLiteral("/versions");
    if (!QDir().mkpath(versions)) {
        return false;
    }
    const QStringList paths{
        m_root,
        QDir::cleanPath(m_root + QStringLiteral("/apps")),
        appRoot(appId),
        versions};
    for (const QString &path : paths) {
        if (!existingPlainDirectory(path)) {
            return false;
        }
    }
    return true;
}

PackageStoreResult PackageStore::commitVerifiedCandidate(
    const QString &appId,
    const QString &version,
    const QByteArray &digestHex,
    const QString &candidateRoot,
    const QByteArray &expectedSignedDigest,
    const QByteArray &expectedSignature,
    const QVector<ArchiveFile> &authenticatedFiles,
    const ArchiveLimits &limits) const
{
    return commitCandidateImpl(
        appId,
        version,
        digestHex,
        candidateRoot,
        expectedSignedDigest,
        expectedSignature,
        &authenticatedFiles,
        limits);
}

PackageStoreResult PackageStore::commitCandidateImpl(
    const QString &appId,
    const QString &version,
    const QByteArray &digestHex,
    const QString &candidateRoot,
    const QByteArray &expectedSignedDigest,
    const QByteArray &expectedSignature,
    const QVector<ArchiveFile> *authenticatedFiles,
    const ArchiveLimits &limits) const
{
    if (!validAppId(appId) || version.size() > 128
        || !VersionPattern.match(version).hasMatch()
        || !DigestPattern.match(QString::fromLatin1(digestHex)).hasMatch()) {
        return failure(PackageStoreError::InvalidArgument,
                       QStringLiteral("invalid package identity"));
    }
    const QFileInfo sourceInfo(candidateRoot);
    if (!sourceInfo.isAbsolute() || !existingPlainDirectory(candidateRoot)) {
        return failure(PackageStoreError::SourceUnavailable,
                       QStringLiteral("candidate directory is unavailable"));
    }
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree storeTree;
    if (!openStableAppTree(m_root, appRoot(appId), storeTree)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    qbrowser_archive_detail::WindowsStableDirectoryTree candidateTree;
    std::vector<qbrowser_archive_detail::WindowsStableFile> candidateLockedFiles;
    if (!candidateTree.openMovableRoot(QDir::cleanPath(candidateRoot))
        || !candidateTree.isStable()) {
        return failure(PackageStoreError::SourceUnavailable,
                       QStringLiteral("candidate directory is unavailable"));
    }
#endif
    const QString directory = version + QLatin1Char('-')
        + QString::fromLatin1(digestHex);
    const QString destination = versionPath(appId, directory);
    if (QFileInfo::exists(destination)) {
        return failure(PackageStoreError::VersionExists,
                       QStringLiteral("version directory already exists"));
    }
    const QString canonicalVersions = QFileInfo(
        appRoot(appId) + QStringLiteral("/versions")).canonicalFilePath();
    if (canonicalVersions.isEmpty()
        || !pathIsWithin(QFileInfo(appRoot(appId)).canonicalFilePath(),
                         canonicalVersions)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("version store escapes application root"));
    }
    QSet<QString> verifiedMembers;
    QStringList verifiedRelativeFiles;
    qsizetype maximumCandidateDepth = 0;
    if (authenticatedFiles != nullptr) {
        if (expectedSignedDigest.size() != 32 || expectedSignature.size() != 64
            || static_cast<quint64>(authenticatedFiles->size())
                > limits.maximumEntries) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("authenticated candidate is invalid"));
        }
        bool authenticatedSignatureMatched = false;
        QSet<QString> verifiedCanonicalFiles;
        QSet<QString> verifiedCanonicalDirectories;
        QHash<QString, QString> verifiedDirectorySpellings;
        for (const ArchiveFile &file : *authenticatedFiles) {
            const auto checked = qbrowser_archive_detail::validateArchivePath(
                file.path, limits);
            if (!checked.has_value()) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("authenticated candidate is invalid"));
            }
            const QString relative = QString::fromUtf8(file.path);
            const QString canonical =
                qbrowser_package_detail::canonicalCandidatePath(relative);
            if (verifiedCanonicalDirectories.contains(canonical)
                || !qbrowser_package_detail::insertCanonicalCandidatePath(
                    verifiedCanonicalFiles, relative)) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("authenticated members collide"));
            }
            verifiedMembers.insert(QStringLiteral("f:") + canonical);
            QString parentDirectory = QFileInfo(relative).path();
            while (!parentDirectory.isEmpty()
                   && parentDirectory != QLatin1String(".")) {
                const QString normalizedParent =
                    QDir::fromNativeSeparators(parentDirectory)
                        .normalized(QString::NormalizationForm_C);
                const QString canonicalParent =
                    qbrowser_package_detail::canonicalCandidatePath(
                        normalizedParent);
                const auto spelling = verifiedDirectorySpellings.constFind(
                    canonicalParent);
                if (verifiedCanonicalFiles.contains(canonicalParent)
                    || (spelling != verifiedDirectorySpellings.cend()
                        && *spelling != normalizedParent)) {
                    return failure(
                        PackageStoreError::CandidateCommitFailed,
                        QStringLiteral("authenticated members collide"));
                }
                verifiedCanonicalDirectories.insert(canonicalParent);
                verifiedDirectorySpellings.insert(
                    canonicalParent, normalizedParent);
                verifiedMembers.insert(QStringLiteral("d:") + canonicalParent);
                parentDirectory = QFileInfo(parentDirectory).path();
            }
            maximumCandidateDepth = std::max(
                maximumCandidateDepth, candidateDepth(relative));
            if (file.path == QByteArrayLiteral("metadata/signature.ed25519")) {
                authenticatedSignatureMatched = file.contents == expectedSignature;
            }
        }
        if (!authenticatedSignatureMatched) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("authenticated signature is invalid"));
        }
        QVector<ArchiveFile> actualFiles;
        QSet<QString> actualDirectories;
        QSet<QString> expectedDirectories;
        QSet<QString> actualMembers;
        QSet<QString> actualCanonicalPaths;
        bool actualSignatureMatched = false;
        quint64 totalBytes = 0;
        QDirIterator iterator(
            candidateRoot,
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (info.isSymLink()) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate identity changed"));
            }
            const QString relative = QDir::fromNativeSeparators(
                QDir(candidateRoot).relativeFilePath(path));
            const QString memberKey = candidateMemberKey(
                candidateRoot, path, info.isDir());
            if (actualMembers.size() >= verifiedMembers.size()
                || actualMembers.contains(memberKey)
                || !qbrowser_package_detail::insertCanonicalCandidatePath(
                    actualCanonicalPaths, relative)
                || !verifiedMembers.contains(memberKey)
                || candidateDepth(relative) > maximumCandidateDepth
                || !qbrowser_archive_detail::validateArchivePath(
                        relative.toUtf8(), limits)
                        .has_value()) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate members changed"));
            }
            actualMembers.insert(memberKey);
            if (info.isDir()) {
#ifdef Q_OS_WIN
                if (!candidateTree.addExistingDirectory(path)) {
                    return failure(PackageStoreError::CandidateCommitFailed,
                                   QStringLiteral("candidate identity changed"));
                }
#endif
                const QString relativeDirectory =
                    qbrowser_package_detail::canonicalCandidatePath(relative);
                if (actualDirectories.contains(relativeDirectory)) {
                    return failure(PackageStoreError::CandidateCommitFailed,
                                   QStringLiteral("candidate members collide"));
                }
                actualDirectories.insert(relativeDirectory);
                continue;
            }
            if (!info.isFile() || info.size() < 0
                || static_cast<quint64>(info.size())
                    > limits.maximumEntryBytes
                || totalBytes > limits.maximumTotalBytes
                || static_cast<quint64>(info.size())
                    > limits.maximumTotalBytes - totalBytes) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate contents are invalid"));
            }
            QByteArray bytes;
#ifdef Q_OS_WIN
            qbrowser_archive_detail::WindowsStableFile locked;
            if (!locked.openReadMoveLocked(path, candidateTree)
                || !locked.readExact(static_cast<quint64>(info.size()),
                                     limits.maximumEntryBytes,
                                     bytes)) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate identity changed"));
            }
            candidateLockedFiles.push_back(std::move(locked));
#else
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate identity changed"));
            }
            bytes = file.readAll();
            if (bytes.size() != info.size()) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate identity changed"));
            }
#endif
            totalBytes += static_cast<quint64>(bytes.size());
            const QString relativeFile = relative;
            addExpectedParentDirectories(relativeFile, expectedDirectories);
            verifiedRelativeFiles.push_back(relativeFile);
            if (relativeFile == QStringLiteral("metadata/signature.ed25519")) {
                actualSignatureMatched = bytes == expectedSignature;
            }
            actualFiles.push_back(
                {relativeFile.toUtf8(),
                 std::move(bytes)});
        }
        const ContentDigestResult actualSigned = ContentDigest::signedPackage(actualFiles);
        const ContentDigestValidation actualPayload = ContentDigest::validatePayload(actualFiles);
        if (!actualSigned.hasValue() || actualSigned.bytes() != expectedSignedDigest
            || !actualPayload.isValid()
            || actualPayload.digest().toHex() != digestHex
            || !actualSignatureMatched
            || actualDirectories != expectedDirectories
            || actualMembers != verifiedMembers
#ifdef Q_OS_WIN
            || !candidateTree.isStable()
            || !std::ranges::all_of(
                candidateLockedFiles,
                [&candidateTree](const auto &file) {
                    return file.isStableWithin(candidateTree);
                })
#endif
        ) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("candidate digest changed"));
        }
    }
#if defined(Q_BROWSER_PACKAGE_INSTALLER_TESTING) && !defined(Q_OS_WIN)
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidatePublish) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidatePublish(candidateRoot, destination);
    }
#endif
#ifdef Q_OS_WIN
    if (authenticatedFiles != nullptr) {
        const std::optional<QSet<QString>> currentMembers = candidateMemberSet(
            candidateRoot,
            verifiedMembers,
            maximumCandidateDepth,
            limits);
        if (!currentMembers.has_value()
            || *currentMembers != verifiedMembers
            || !candidateTree.isStable()
            || !std::ranges::all_of(
                candidateLockedFiles,
                [&candidateTree](const auto &file) {
                    return file.isStableWithin(candidateTree);
                })) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("candidate identity changed"));
        }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
        if (qbrowser_package_installer_testing::packageInstallerTestHooks()
                .afterCandidateScanBeforeSeal) {
            qbrowser_package_installer_testing::packageInstallerTestHooks()
                .afterCandidateScanBeforeSeal(candidateRoot);
        }
#endif
        const bool filesSealed = std::ranges::all_of(
            candidateLockedFiles,
            [](auto &file) { return file.sealMutationsForMove(); });
        const bool directoriesSealed = filesSealed
            && candidateTree.sealMutationsForMove();
        const bool filesReadOnly = directoriesSealed
            && std::ranges::all_of(
                candidateLockedFiles,
                [](auto &file) { return file.setReadOnly(true); });
        if (!filesSealed || !directoriesSealed || !filesReadOnly) {
            for (auto &file : candidateLockedFiles) {
                (void)file.setReadOnly(false);
                (void)file.restoreMutationSeal(file.path());
            }
            (void)candidateTree.restoreMutationSeals();
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("version could not be made immutable"));
        }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
        if (qbrowser_package_installer_testing::packageInstallerTestHooks()
                .beforeCandidatePublish) {
            qbrowser_package_installer_testing::packageInstallerTestHooks()
                .beforeCandidatePublish(candidateRoot, destination);
        }
#endif
        const std::optional<QSet<QString>> sealedMembers = candidateMemberSet(
            candidateRoot,
            verifiedMembers,
            maximumCandidateDepth,
            limits);
        if (!sealedMembers.has_value()
            || *sealedMembers != verifiedMembers
            || !candidateTree.isStable()
            || !std::ranges::all_of(
                candidateLockedFiles,
                [&candidateTree](const auto &file) {
                    return file.isStableWithin(candidateTree);
                })) {
            for (auto &file : candidateLockedFiles) {
                (void)file.setReadOnly(false);
                (void)file.restoreMutationSeal(file.path());
            }
            (void)candidateTree.restoreMutationSeals();
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("candidate identity changed"));
        }
        for (auto &file : candidateLockedFiles) {
            file.releaseSealedForMove();
        }
        candidateTree.releaseDescendantsForMove();
    }
    if (!candidateTree.publishRootNoReplace(destination, storeTree)) {
        for (size_t index = 0;
             index < candidateLockedFiles.size(); ++index) {
            const QString path = candidateRoot + QLatin1Char('/')
                + verifiedRelativeFiles.at(static_cast<qsizetype>(index));
            if (candidateLockedFiles.at(index).restoreMutationSeal(path)) {
                (void)QFile::setPermissions(
                    path,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            }
        }
        (void)candidateTree.restoreMutationSeals();
#else
    if (!QDir().rename(QDir::cleanPath(candidateRoot), destination)) {
#endif
        return failure(PackageStoreError::CandidateCommitFailed,
                       QStringLiteral("candidate could not be committed"));
    }
#ifdef Q_OS_WIN
    if (authenticatedFiles != nullptr) {
        const std::optional<QSet<QString>> publishedMembers = candidateMemberSet(
            destination,
            verifiedMembers,
            maximumCandidateDepth,
            limits);
        bool identitiesMatch = publishedMembers.has_value()
            && *publishedMembers == verifiedMembers
            && candidateTree.verifyMovedTree(destination);
        for (size_t index = 0;
             identitiesMatch && index < candidateLockedFiles.size(); ++index) {
            identitiesMatch = candidateLockedFiles.at(index).isSameIdentityAt(
                destination + QLatin1Char('/')
                + verifiedRelativeFiles.at(static_cast<qsizetype>(index)));
        }
        if (!identitiesMatch) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("candidate identity changed"));
        }
    }
#endif
#ifdef Q_OS_WIN
    if (authenticatedFiles == nullptr
        && !makeVersionFilesReadOnly(destination)) {
#else
    if (!makeVersionFilesReadOnly(destination)) {
#endif
        return failure(PackageStoreError::CandidateCommitFailed,
                       QStringLiteral("version could not be made immutable"));
    }
    return {PackageStoreError::None, destination, {}};
}

bool PackageStore::stateTargetIsValid(
    const QString &appId,
    const QString &target,
    const bool allowEmpty) const
{
    if (target.isEmpty()) {
        return allowEmpty;
    }
    if (!validVersionDirectory(target)) {
        return false;
    }
    const QString expected = versionPath(appId, target);
    const QFileInfo targetInfo(expected);
    const QString canonicalApp = QFileInfo(appRoot(appId)).canonicalFilePath();
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    return existingPlainDirectory(expected) && !canonicalApp.isEmpty()
        && !canonicalTarget.isEmpty()
        && pathIsWithin(canonicalApp, canonicalTarget);
}

ActivationStateResult PackageStore::activationState(const QString &appId) const
{
    if (!validAppId(appId)) {
        return stateFailure(PackageStoreError::InvalidArgument,
                            QStringLiteral("invalid application identifier"));
    }
    if (!QFileInfo::exists(appRoot(appId) + QStringLiteral("/activation.json"))) {
        return {};
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is busy"));
    }
    return activationStateUnlocked(appId);
}

ActivationStateResult PackageStore::activationStateUnlocked(
    const QString &appId) const
{
    if (!validAppId(appId)) {
        return stateFailure(PackageStoreError::InvalidArgument,
                            QStringLiteral("invalid application identifier"));
    }
    const QString statePath = appRoot(appId) + QStringLiteral("/activation.json");
    if (!QFileInfo::exists(statePath)) {
        return {};
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree storeTree;
    if (!openStableAppTree(m_root, appRoot(appId), storeTree)) {
        return stateFailure(PackageStoreError::UnsafeStore,
                            QStringLiteral("package store is unavailable"));
    }
#endif
    QByteArray stateBytes;
#ifdef Q_OS_WIN
    const qint64 expectedSize = QFileInfo(statePath).size();
    qbrowser_archive_detail::WindowsStableFile stateFile;
    if (expectedSize < 0 || expectedSize > 4096
        || !stateFile.openReadLocked(statePath, storeTree)
        || !stateFile.readExact(static_cast<quint64>(expectedSize), 4096, stateBytes)) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is unavailable"));
    }
#else
    QFile file(statePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is unavailable"));
    }
    stateBytes = file.readAll();
#endif
    const std::optional<ActivationState> state = ActivationState::fromJson(
        stateBytes);
    if (!state.has_value()
        || !stateTargetIsValid(appId, state->current, true)
        || !stateTargetIsValid(appId, state->previous, true)
        || !stateTargetIsValid(appId, state->lastKnownGood, true)) {
        return stateFailure(PackageStoreError::InvalidState,
                            QStringLiteral("activation state target is invalid"));
    }
#ifdef Q_OS_WIN
    if (!pinStateTargets(appRoot(appId), *state, storeTree)
        || !stateTargetIsValid(appId, state->current, true)
        || !stateTargetIsValid(appId, state->previous, true)
        || !stateTargetIsValid(appId, state->lastKnownGood, true)) {
        return stateFailure(PackageStoreError::InvalidState,
                            QStringLiteral("activation state target is invalid"));
    }
#endif
    return {PackageStoreError::None, *state, {}};
}

ActivationStateResult PackageStore::recordedActivationState(
    const QString &appId) const
{
    if (!validAppId(appId)) {
        return stateFailure(PackageStoreError::InvalidArgument,
                            QStringLiteral("invalid application identifier"));
    }
    if (!QFileInfo::exists(appRoot(appId) + QStringLiteral("/activation.json"))) {
        return {};
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is busy"));
    }
    return recordedActivationStateUnlocked(appId);
}

ActivationStateResult PackageStore::recordedActivationStateUnlocked(
    const QString &appId) const
{
    if (!validAppId(appId)) {
        return stateFailure(PackageStoreError::InvalidArgument,
                            QStringLiteral("invalid application identifier"));
    }
    const QString statePath = appRoot(appId) + QStringLiteral("/activation.json");
    if (!QFileInfo::exists(statePath)) return {};
    QByteArray stateBytes;
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree storeTree;
    const qint64 expectedSize = QFileInfo(statePath).size();
    qbrowser_archive_detail::WindowsStableFile stateFile;
    if (!openStableAppTree(m_root, appRoot(appId), storeTree)
        || expectedSize < 0 || expectedSize > 4096
        || !stateFile.openReadLocked(statePath, storeTree)
        || !stateFile.readExact(static_cast<quint64>(expectedSize), 4096,
                                stateBytes)) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is unavailable"));
    }
#else
    QFile file(statePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        return stateFailure(PackageStoreError::StateUnavailable,
                            QStringLiteral("activation state is unavailable"));
    }
    stateBytes = file.readAll();
#endif
    const std::optional<ActivationState> state = ActivationState::fromJson(stateBytes);
    const auto syntacticallyValid = [](const QString &target) {
        return target.isEmpty() || validVersionDirectory(target);
    };
    if (!state.has_value() || !syntacticallyValid(state->current)
        || !syntacticallyValid(state->previous)
        || !syntacticallyValid(state->lastKnownGood)) {
        return stateFailure(PackageStoreError::InvalidState,
                            QStringLiteral("activation state target is invalid"));
    }
    return {PackageStoreError::None, *state, {}};
}

PackageStoreResult PackageStore::writeState(
    const QString &appId,
    const ActivationState &state) const
{
    if (!ensureAppDirectories(appId)
        || !stateTargetIsValid(appId, state.current, true)
        || !stateTargetIsValid(appId, state.previous, true)
        || !stateTargetIsValid(appId, state.lastKnownGood, true)) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation state target is invalid"));
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree storeTree;
    if (!openStableAppTree(m_root, appRoot(appId), storeTree)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    if (!pinStateTargets(appRoot(appId), state, storeTree)
        || !stateTargetIsValid(appId, state.current, true)
        || !stateTargetIsValid(appId, state.previous, true)
        || !stateTargetIsValid(appId, state.lastKnownGood, true)) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation state target is invalid"));
    }
#endif
    const QByteArray bytes = state.toJson();
    QSaveFile file(appRoot(appId) + QStringLiteral("/activation.json"));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
        || !file.commit()) {
        file.cancelWriting();
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation state could not be committed"));
    }
    return {};
}

PackageStoreResult PackageStore::activateVerified(
    const QString &appId,
    const QString &versionDirectory) const
{
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return failure(PackageStoreError::StateUnavailable,
                       QStringLiteral("activation state is busy"));
    }
#ifdef Q_BROWSER_PACKAGE_STORE_TESTING
    if (qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired) {
        qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired(appId, QStringLiteral("activate"));
    }
#endif
    if (!stateTargetIsValid(appId, versionDirectory, false)) {
        return failure(PackageStoreError::VersionUnavailable,
                       QStringLiteral("version is unavailable"));
    }
    const ActivationStateResult loaded = activationStateUnlocked(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    if (loaded.state.current == versionDirectory) {
        const auto binding = bindingForState(loaded.state);
        if (!binding.has_value()) {
            return failure(PackageStoreError::InvalidState,
                           QStringLiteral("activation binding is invalid"));
        }
        return {PackageStoreError::None, versionPath(appId, versionDirectory),
                {}, binding};
    }
    if (loaded.state.generation >= 9'007'199'254'740'991LL) {
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation generation is exhausted"));
    }
    ActivationState next = loaded.state;
    next.previous = next.current;
    next.current = versionDirectory;
    ++next.generation;
    PackageStoreResult committed = writeState(appId, next);
    if (!committed.succeeded()) return committed;
    committed.path = versionPath(appId, versionDirectory);
    committed.activationBinding = bindingForState(next);
    if (!committed.activationBinding.has_value()) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation binding is invalid"));
    }
    return committed;
}

#ifdef Q_BROWSER_PACKAGE_STORE_TESTING
PackageStoreResult PackageStore::commitCandidateForTesting(
    const QString &appId,
    const QString &version,
    const QByteArray &digestHex,
    const QString &candidateRoot) const
{
    return commitCandidateImpl(
        appId,
        version,
        digestHex,
        candidateRoot,
        {},
        {},
        nullptr,
        {});
}

PackageStoreResult PackageStore::activateForTesting(
    const QString &appId,
    const QString &versionDirectory) const
{
    return activateVerified(appId, versionDirectory);
}

PackageStoreResult PackageStore::markCurrentLastKnownGoodForTesting(
    const QString &appId) const
{
    const ActivationStateResult state = activationState(appId);
    if (!state.hasValue()) return failure(state.error, state.message);
    const auto binding = bindingForState(state.state);
    if (!binding.has_value()) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation binding is invalid"));
    }
    return markCurrentLastKnownGood(appId, *binding);
}

PackageStoreResult PackageStore::markCurrentLastKnownGoodForTesting(
    const QString &appId,
    const ActivationBinding &expected) const
{
    return markCurrentLastKnownGood(appId, expected);
}

PackageStoreResult PackageStore::rollbackForTesting(const QString &appId) const
{
    const ActivationStateResult state = activationState(appId);
    if (!state.hasValue()) return failure(state.error, state.message);
    const auto binding = bindingForState(state.state);
    if (!binding.has_value()) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation binding is invalid"));
    }
    return rollback(appId, *binding);
}

PackageStoreResult PackageStore::rollbackForTesting(
    const QString &appId,
    const ActivationBinding &expected) const
{
    return rollback(appId, expected);
}
#endif

PackageStoreResult PackageStore::markCurrentLastKnownGood(
    const QString &appId,
    const ActivationBinding &expected) const
{
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return failure(PackageStoreError::StateUnavailable,
                       QStringLiteral("activation state is busy"));
    }
#ifdef Q_BROWSER_PACKAGE_STORE_TESTING
    if (qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired) {
        qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired(appId, QStringLiteral("mark_lkg"));
    }
#endif
    const ActivationStateResult loaded = activationStateUnlocked(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    if (loaded.state.current.isEmpty()) {
        return failure(PackageStoreError::VersionUnavailable,
                       QStringLiteral("no current version is active"));
    }
    const auto actual = bindingForState(loaded.state);
    if (!actual.has_value() || *actual != expected) {
        return failure(PackageStoreError::StateConflict,
                       QStringLiteral("activation state changed"));
    }
    if (loaded.state.lastKnownGood == loaded.state.current) {
        return {PackageStoreError::None, versionPath(appId, loaded.state.current),
                {}, actual};
    }
    if (loaded.state.generation >= 9'007'199'254'740'991LL) {
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation generation is exhausted"));
    }
    ActivationState next = loaded.state;
    next.lastKnownGood = next.current;
    ++next.generation;
    PackageStoreResult committed = writeState(appId, next);
    if (!committed.succeeded()) return committed;
    committed.path = versionPath(appId, next.current);
    committed.activationBinding = bindingForState(next);
    return committed;
}

PackageStoreResult PackageStore::recoverLastKnownGood(
    const QString &appId,
    const ActivationBinding &expected) const
{
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return failure(PackageStoreError::StateUnavailable,
                       QStringLiteral("activation state is busy"));
    }
    const ActivationStateResult loaded = recordedActivationStateUnlocked(appId);
    if (!loaded.hasValue()) return failure(loaded.error, loaded.message);
    const auto actual = bindingForState(loaded.state);
    if (!actual.has_value() || *actual != expected) {
        return failure(PackageStoreError::StateConflict,
                       QStringLiteral("activation state changed"));
    }
    if (loaded.state.lastKnownGood.isEmpty()
        || !stateTargetIsValid(appId, loaded.state.lastKnownGood, false)) {
        return failure(PackageStoreError::RollbackUnavailable,
                       QStringLiteral("last known good version is unavailable"));
    }
    if (loaded.state.current == loaded.state.lastKnownGood
        && stateTargetIsValid(appId, loaded.state.current, false)) {
        return {PackageStoreError::None, versionPath(appId, loaded.state.current),
                {}, actual};
    }
    if (loaded.state.generation >= 9'007'199'254'740'991LL) {
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation generation is exhausted"));
    }
    const ActivationState recovered{loaded.state.lastKnownGood,
                                    {},
                                    loaded.state.lastKnownGood,
                                    loaded.state.generation + 1};
    PackageStoreResult committed = writeState(appId, recovered);
    if (!committed.succeeded()) return committed;
    committed.path = versionPath(appId, recovered.current);
    committed.activationBinding = bindingForState(recovered);
    return committed;
}

PackageStoreResult PackageStore::rollback(
    const QString &appId,
    const ActivationBinding &expected) const
{
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return failure(PackageStoreError::StateUnavailable,
                       QStringLiteral("activation state is busy"));
    }
#ifdef Q_BROWSER_PACKAGE_STORE_TESTING
    if (qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired) {
        qbrowser_package_store_testing::packageStoreTestHooks()
            .afterActivationLockAcquired(appId, QStringLiteral("rollback"));
    }
#endif
    const ActivationStateResult loaded = activationStateUnlocked(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    const auto actual = bindingForState(loaded.state);
    if (!actual.has_value() || *actual != expected) {
        return failure(PackageStoreError::StateConflict,
                       QStringLiteral("activation state changed"));
    }
    QString target = loaded.state.lastKnownGood;
    if (target.isEmpty()) target = loaded.state.previous;
    if (target.isEmpty() || target == loaded.state.current
        || !stateTargetIsValid(appId, target, false)) {
        return failure(PackageStoreError::RollbackUnavailable,
                       QStringLiteral("rollback target is unavailable"));
    }
    if (loaded.state.generation >= 9'007'199'254'740'991LL) {
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation generation is exhausted"));
    }
    ActivationState next = loaded.state;
    next.previous = next.current;
    next.current = target;
    ++next.generation;
    PackageStoreResult committed = writeState(appId, next);
    if (!committed.succeeded()) return committed;
    committed.path = versionPath(appId, next.current);
    committed.activationBinding = bindingForState(next);
    return committed;
}

PackageStoreResult PackageStore::confirmCurrent(
    const QString &appId,
    const ActivationBinding &expected) const
{
    if (!ensureAppDirectories(appId)) {
        return failure(PackageStoreError::UnsafeStore,
                       QStringLiteral("package store is unavailable"));
    }
    const auto transactionLock = acquireActivationTransactionLock(appRoot(appId));
    if (!transactionLock) {
        return failure(PackageStoreError::StateUnavailable,
                       QStringLiteral("activation state is busy"));
    }
    const ActivationStateResult loaded = recordedActivationStateUnlocked(appId);
    if (!loaded.hasValue()) return failure(loaded.error, loaded.message);
    const auto actual = bindingForState(loaded.state);
    if (!actual.has_value() || *actual != expected) {
        return failure(PackageStoreError::StateConflict,
                       QStringLiteral("activation state changed"));
    }
    if (loaded.state.generation >= 9'007'199'254'740'991LL) {
        return failure(PackageStoreError::StateCommitFailed,
                       QStringLiteral("activation generation is exhausted"));
    }
    ActivationState confirmed = loaded.state;
    ++confirmed.generation;
    PackageStoreResult committed = writeState(appId, confirmed);
    if (!committed.succeeded()) return committed;
    committed.path = versionPath(appId, confirmed.current);
    committed.activationBinding = bindingForState(confirmed);
    if (!committed.activationBinding.has_value()) {
        return failure(PackageStoreError::InvalidState,
                       QStringLiteral("activation binding is invalid"));
    }
    return committed;
}

std::optional<ActivationBinding> PackageStore::bindingForState(
    const ActivationState &state) const
{
    if (state.generation < 0 || state.current.isEmpty()) return std::nullopt;
    const qsizetype separator = state.current.lastIndexOf(QLatin1Char('-'));
    if (separator <= 0 || state.current.size() - separator - 1 != 64) {
        return std::nullopt;
    }
    const QByteArray digest = state.current.sliced(separator + 1).toLatin1();
    for (const char value : digest) {
        if (!((value >= '0' && value <= '9')
              || (value >= 'a' && value <= 'f'))) {
            return std::nullopt;
        }
    }
    return ActivationBinding{state.current, digest, state.generation};
}

PackageStoreResult PackageStore::resolveCurrent(const QString &appId) const
{
    const ActivationStateResult loaded = activationState(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    if (loaded.state.current.isEmpty()) {
        return failure(PackageStoreError::VersionUnavailable,
                       QStringLiteral("no current version is active"));
    }
    return {PackageStoreError::None,
            versionPath(appId, loaded.state.current),
            {}};
}
