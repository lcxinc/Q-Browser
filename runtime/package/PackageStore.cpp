#include "PackageStore.h"
#include "Archive.h"
#include "ContentDigest.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

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
            && !tree.addExistingDirectory(
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

PackageStoreResult PackageStore::commitCandidate(
    const QString &appId,
    const QString &version,
    const QByteArray &digestHex,
    const QString &candidateRoot,
    const QByteArray &expectedSignedDigest) const
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
    if (!expectedSignedDigest.isEmpty()) {
        QVector<ArchiveFile> actualFiles;
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
            if (info.isDir()) {
                continue;
            }
            if (!info.isFile() || info.size() < 0
                || static_cast<quint64>(info.size())
                    > ArchiveLimits::DefaultMaximumEntryBytes
                || totalBytes > ArchiveLimits::DefaultMaximumTotalBytes
                || static_cast<quint64>(info.size())
                    > ArchiveLimits::DefaultMaximumTotalBytes - totalBytes) {
                return failure(PackageStoreError::CandidateCommitFailed,
                               QStringLiteral("candidate contents are invalid"));
            }
            QByteArray bytes;
#ifdef Q_OS_WIN
            qbrowser_archive_detail::WindowsStableFile locked;
            if (!locked.openReadMoveLocked(path, candidateTree)
                || !locked.readExact(static_cast<quint64>(info.size()),
                                     ArchiveLimits::DefaultMaximumEntryBytes,
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
            actualFiles.push_back(
                {QDir::fromNativeSeparators(
                     QDir(candidateRoot).relativeFilePath(path)).toUtf8(),
                 std::move(bytes)});
        }
        const ContentDigestResult actualSigned = ContentDigest::signedPackage(actualFiles);
        const ContentDigestValidation actualPayload = ContentDigest::validatePayload(actualFiles);
        if (!actualSigned.hasValue() || actualSigned.bytes() != expectedSignedDigest
            || !actualPayload.isValid()
            || actualPayload.digest().toHex() != digestHex
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
        if (!makeVersionFilesReadOnly(candidateRoot)) {
            return failure(PackageStoreError::CandidateCommitFailed,
                           QStringLiteral("version could not be made immutable"));
        }
    }
#ifdef Q_OS_WIN
    candidateLockedFiles.clear();
    if (!candidateTree.publishRootNoReplace(destination, storeTree)) {
#else
    if (!QDir().rename(QDir::cleanPath(candidateRoot), destination)) {
#endif
        return failure(PackageStoreError::CandidateCommitFailed,
                       QStringLiteral("candidate could not be committed"));
    }
    if (expectedSignedDigest.isEmpty() && !makeVersionFilesReadOnly(destination)) {
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

PackageStoreResult PackageStore::activate(
    const QString &appId,
    const QString &versionDirectory) const
{
    if (!stateTargetIsValid(appId, versionDirectory, false)) {
        return failure(PackageStoreError::VersionUnavailable,
                       QStringLiteral("version is unavailable"));
    }
    const ActivationStateResult loaded = activationState(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    if (loaded.state.current == versionDirectory) {
        return {};
    }
    ActivationState next = loaded.state;
    next.previous = next.current;
    next.current = versionDirectory;
    return writeState(appId, next);
}

PackageStoreResult PackageStore::markCurrentLastKnownGood(
    const QString &appId) const
{
    const ActivationStateResult loaded = activationState(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    if (loaded.state.current.isEmpty()) {
        return failure(PackageStoreError::VersionUnavailable,
                       QStringLiteral("no current version is active"));
    }
    if (loaded.state.lastKnownGood == loaded.state.current) {
        return {};
    }
    ActivationState next = loaded.state;
    next.lastKnownGood = next.current;
    return writeState(appId, next);
}

PackageStoreResult PackageStore::rollback(const QString &appId) const
{
    const ActivationStateResult loaded = activationState(appId);
    if (!loaded.hasValue()) {
        return failure(loaded.error, loaded.message);
    }
    QString target = loaded.state.previous;
    if (target.isEmpty()) {
        target = loaded.state.lastKnownGood;
    }
    if (target.isEmpty() || target == loaded.state.current
        || !stateTargetIsValid(appId, target, false)) {
        return failure(PackageStoreError::RollbackUnavailable,
                       QStringLiteral("rollback target is unavailable"));
    }
    ActivationState next = loaded.state;
    next.previous = next.current;
    next.current = target;
    return writeState(appId, next);
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
