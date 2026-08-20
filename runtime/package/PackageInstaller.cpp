#include "PackageInstaller.h"

#include "Archive.h"
#include "ArchiveTestHooks.h"
#include "CanonicalArchivePath.h"
#include "ContentDigest.h"
#include "PackageStore.h"
#include "PackageInstallerTestHooks.h"
#include "QmlSourcePolicy.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QVersionNumber>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
#ifdef Q_OS_WIN
QString stagingMemberKey(
    const QString &root,
    const QString &path,
    const bool directory)
{
    return (directory ? QStringLiteral("d:") : QStringLiteral("f:"))
        + QDir::fromNativeSeparators(QDir(root).relativeFilePath(path))
              .toCaseFolded();
}

void addStagingParentDirectories(
    const QString &prefix,
    const QString &relativeFile,
    QSet<QString> &members)
{
    QString parent = QFileInfo(relativeFile).path();
    while (!parent.isEmpty() && parent != QLatin1String(".")) {
        members.insert(QStringLiteral("d:")
                       + (prefix + QLatin1Char('/') + parent).toCaseFolded());
        parent = QFileInfo(parent).path();
    }
}

bool deleteOwnedStagingTree(
    const QString &root,
    qbrowser_archive_detail::WindowsStableDirectoryTree &tree,
    const QSet<QString> &expectedMembers)
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().beforeFailureCleanup) {
        qbrowser_archive_testing::archiveTestHooks().beforeFailureCleanup(root);
    }
#endif
    QStringList files;
    qsizetype scannedMembers = 0;
    QDirIterator iterator(
        root,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            return false;
        }
        const QString key = stagingMemberKey(root, path, info.isDir());
        if (scannedMembers >= expectedMembers.size()
            || !expectedMembers.contains(key)) {
            return false;
        }
        ++scannedMembers;
        if (info.isDir()) {
            if (!tree.addExistingDirectory(path)) {
                return false;
            }
        } else if (info.isFile()) {
            files.push_back(path);
        } else {
            return false;
        }
    }
    if (!tree.isStable()) {
        return false;
    }
    for (const QString &path : files) {
        qbrowser_archive_detail::WindowsStableFile owned;
        if (!owned.openForDelete(path, tree) || !owned.deleteOwned()) {
            return false;
        }
    }
    return tree.deleteHeldTree();
}

class WindowsStagingCleanup final
{
public:
    WindowsStagingCleanup(
        QString root,
        qbrowser_archive_detail::WindowsStableDirectoryTree &tree)
        : m_root(std::move(root))
        , m_tree(tree)
    {
        m_expectedMembers.insert(QStringLiteral("f:candidate.qapkg"));
    }

    ~WindowsStagingCleanup()
    {
        (void)deleteOwnedStagingTree(m_root, m_tree, m_expectedMembers);
    }

    WindowsStagingCleanup(const WindowsStagingCleanup &) = delete;
    WindowsStagingCleanup &operator=(const WindowsStagingCleanup &) = delete;

    [[nodiscard]] bool setAuthenticatedSnapshot(
        const QVector<ArchiveFile> &files,
        const ArchiveLimits &limits)
    {
        if (static_cast<quint64>(files.size()) > limits.maximumEntries) {
            return false;
        }
        QSet<QString> expected{QStringLiteral("f:candidate.qapkg"),
                               QStringLiteral("d:preflight"),
                               QStringLiteral("d:candidate")};
        for (const ArchiveFile &file : files) {
            if (!qbrowser_archive_detail::validateArchivePath(file.path, limits)
                     .has_value()) {
                return false;
            }
            const QString relative = QString::fromUtf8(file.path);
            for (const QString &prefix : {QStringLiteral("preflight"),
                                          QStringLiteral("candidate")}) {
                expected.insert(QStringLiteral("f:")
                                + (prefix + QLatin1Char('/') + relative)
                                      .toCaseFolded());
                addStagingParentDirectories(prefix, relative, expected);
            }
        }
        m_expectedMembers = std::move(expected);
        return true;
    }

private:
    QString m_root;
    qbrowser_archive_detail::WindowsStableDirectoryTree &m_tree;
    QSet<QString> m_expectedMembers;
};
#endif

InstallResult failure(const InstallPhase phase,
                      const InstallError error,
                      const QString &stableError)
{
    return {phase, error, stableError, {}, {}, {}};
}

const ArchiveFile *findFile(const QVector<ArchiveFile> &files, const QByteArray &path)
{
    const auto found = std::find_if(
        files.cbegin(), files.cend(), [&path](const ArchiveFile &file) {
            return file.path == path;
        });
    return found == files.cend() ? nullptr : &*found;
}

bool copyBounded(const QString &sourcePath,
                 const QString &destinationPath,
                 const quint64 maximumBytes)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly) || source.size() < 0
        || static_cast<quint64>(source.size()) > maximumBytes) {
        return false;
    }
    const QByteArray bytes = source.read(static_cast<qint64>(maximumBytes) + 1);
    if (bytes.size() != source.size()
        || static_cast<quint64>(bytes.size()) > maximumBytes) {
        return false;
    }
    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)
        || destination.write(bytes) != bytes.size()
        || !destination.commit()) {
        destination.cancelWriting();
        return false;
    }
    return true;
}

std::optional<QVersionNumber> strictRuntimeVersion(const QString &version)
{
    const qsizetype suffix = version.indexOf(QLatin1Char('-'));
    const QString core = suffix < 0 ? version : version.first(suffix);
    const QVersionNumber parsed = QVersionNumber::fromString(core);
    if (parsed.segmentCount() != 3 || parsed.toString() != core) {
        return std::nullopt;
    }
    return parsed;
}

bool runtimeIsCompatible(const QString &runtimeVersion,
                         const RuntimeCompatibility &compatibility)
{
    const std::optional<QVersionNumber> runtime = strictRuntimeVersion(runtimeVersion);
    const std::optional<QVersionNumber> minimum = strictRuntimeVersion(
        compatibility.minVersion);
    bool maximumMajorOk = false;
    const int maximumMajor = compatibility.maxVersion.first(
        compatibility.maxVersion.indexOf(QLatin1Char('.'))).toInt(&maximumMajorOk);
    return runtime.has_value() && minimum.has_value() && maximumMajorOk
        && QVersionNumber::compare(*runtime, *minimum) >= 0
        && runtime->majorVersion() <= maximumMajor;
}

bool importsAreAllowed(const QStringList &imports, const QSet<QString> &allowed)
{
    return std::ranges::all_of(imports, [&allowed](const QString &name) {
        return allowed.contains(name);
    });
}

bool sourcesPassPolicy(const QVector<ArchiveFile> &files)
{
    return std::ranges::all_of(files, [](const ArchiveFile &file) {
        if (!QmlSourcePolicy::isQmlSourcePath(file.path)) {
            return true;
        }
        return QmlSourcePolicy::violations(file.contents).isEmpty();
    });
}
}

PackageInstaller::PackageInstaller(PackageStore &store,
                                   QByteArray trustedPublicKeyPem,
                                   InstallPolicy policy)
    : m_store(store)
    , m_trustedPublicKeyPem(std::move(trustedPublicKeyPem))
    , m_policy(std::move(policy))
{
}

InstallResult PackageInstaller::verifyInstalled(
    const QString &appId,
    const QString &versionDirectory) const
{
    const QString root = m_store.versionPath(appId, versionDirectory);
    const QFileInfo rootInfo(root);
    if (root.isEmpty() || !rootInfo.isDir() || rootInfo.isSymLink()) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    QVector<ArchiveFile> files;
    quint64 totalBytes = 0;
    quint64 scannedMembers = 0;
    const quint64 maximumScannedMembers =
        m_policy.archiveLimits.maximumEntries
                > std::numeric_limits<quint64>::max() / 64
        ? std::numeric_limits<quint64>::max()
        : m_policy.archiveLimits.maximumEntries * 64;
    QDirIterator iterator(root,
                          QDir::AllEntries | QDir::Hidden | QDir::System
                              | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (scannedMembers >= maximumScannedMembers) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        ++scannedMembers;
        if (info.isSymLink()) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        if (info.isDir()) continue;
        if (!info.isFile()
            || static_cast<quint64>(files.size()) >= m_policy.archiveLimits.maximumEntries
            || info.size() < 0
            || static_cast<quint64>(info.size()) > m_policy.archiveLimits.maximumEntryBytes) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        const QByteArray relative = QDir(root).relativeFilePath(path).toUtf8();
        if (!qbrowser_archive_detail::validateArchivePath(
                 relative, m_policy.archiveLimits).has_value()
            || totalBytes > m_policy.archiveLimits.maximumTotalBytes
                   - static_cast<quint64>(info.size())) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        const QByteArray bytes = file.read(info.size() + 1);
        if (bytes.size() != info.size()) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        totalBytes += static_cast<quint64>(bytes.size());
        files.push_back({relative, bytes});
    }
    std::ranges::sort(files, [](const ArchiveFile &left, const ArchiveFile &right) {
        return qbrowser_archive_detail::archivePathBytewiseLess(left.path, right.path);
    });
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (signatureFile == nullptr || signatureFile->contents.size() != 64
        || !signedDigest.hasValue()
        || !SignatureVerifier::verifyPem(signedDigest.bytes(),
                                         m_trustedPublicKeyPem,
                                         signatureFile->contents).isVerified()
        || !content.isValid() || manifestFile == nullptr) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    const ManifestParseResult parsed = Manifest::parse(manifestFile->contents);
    if (!parsed.hasValue() || parsed.value().appId() != appId
        || !runtimeIsCompatible(m_policy.runtimeVersion, parsed.value().runtime())
        || !importsAreAllowed(parsed.value().imports(), m_policy.allowedImports)
        || !sourcesPassPolicy(files)
        || versionDirectory != parsed.value().version() + QLatin1Char('-')
               + QString::fromLatin1(content.digest().toHex())) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    const QFileInfo entry(root + QLatin1Char('/') + parsed.value().entryPoint());
    if (!entry.isFile() || entry.isSymLink()) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled(appId, versionDirectory);
    }
#endif
    return {InstallPhase::Complete, InstallError::None, {}, appId,
            parsed.value().version(), root, std::nullopt};
}

InstallResult PackageInstaller::install(const QString &packagePath) const
{
    const QString stagingParent = m_store.root() + QStringLiteral("/.staging");
    if (!QDir().mkpath(stagingParent)) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    const QFileInfo stagingInfo(stagingParent);
    if (!stagingInfo.isDir() || stagingInfo.isSymLink()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    QTemporaryDir staging(stagingParent + QStringLiteral("/install-XXXXXX"));
    if (!staging.isValid()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree stagingTree;
    if (!stagingTree.openRoot(staging.path()) || !stagingTree.isStable()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    staging.setAutoRemove(false);
    WindowsStagingCleanup stagingCleanup(staging.path(), stagingTree);
#endif
    const QString stagedPackage = staging.filePath(QStringLiteral("candidate.qapkg"));
    if (!copyBounded(packagePath,
                     stagedPackage,
                     m_policy.archiveLimits.maximumArchiveBytes)) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }

    const ArchiveSnapshotResult snapshot = Archive::snapshot(
        stagedPackage, m_policy.archiveLimits);
    if (!snapshot.hasValue()) {
        return failure(InstallPhase::Verify,
                       InstallError::ArchiveInvalid,
                       QStringLiteral("archive_invalid"));
    }
    const QVector<ArchiveFile> &files = snapshot.files();
#ifdef Q_OS_WIN
    if (!stagingCleanup.setAuthenticatedSnapshot(files, m_policy.archiveLimits)) {
        return failure(InstallPhase::Verify,
                       InstallError::ArchiveInvalid,
                       QStringLiteral("archive_invalid"));
    }
#endif
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    if (signatureFile == nullptr) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureMissing,
                       QStringLiteral("signature_missing"));
    }
    if (signatureFile->contents.size() != 64) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureInvalid,
                       QStringLiteral("signature_invalid"));
    }
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    if (!signedDigest.hasValue()
        || !SignatureVerifier::verifyPem(signedDigest.bytes(),
                                         m_trustedPublicKeyPem,
                                         signatureFile->contents)
                .isVerified()) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureInvalid,
                       QStringLiteral("signature_invalid"));
    }
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    if (!content.isValid()) {
        return failure(InstallPhase::Verify,
                       InstallError::ContentInvalid,
                       QStringLiteral("content_invalid"));
    }
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr) {
        return failure(InstallPhase::Verify,
                       InstallError::ManifestInvalid,
                       QStringLiteral("manifest_invalid"));
    }
    const ManifestParseResult parsed = Manifest::parse(manifestFile->contents);
    if (!parsed.hasValue()) {
        return failure(InstallPhase::Verify,
                       InstallError::ManifestInvalid,
                       QStringLiteral("manifest_invalid"));
    }
    const Manifest &manifest = parsed.value();
    if (!runtimeIsCompatible(m_policy.runtimeVersion, manifest.runtime())) {
        return failure(InstallPhase::Verify,
                       InstallError::RuntimeIncompatible,
                       QStringLiteral("runtime_incompatible"));
    }
    if (!importsAreAllowed(manifest.imports(), m_policy.allowedImports)) {
        return failure(InstallPhase::Verify,
                       InstallError::ImportDenied,
                       QStringLiteral("import_denied"));
    }
    if (!sourcesPassPolicy(files)) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("source_policy_rejected"));
    }

    const QString preflightRoot = staging.filePath(QStringLiteral("preflight"));
    if (!QDir().mkdir(preflightRoot)
        || !Archive::extractFiles(files, preflightRoot, m_policy.archiveLimits)
                .hasValue()) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }
    const QFileInfo entryPoint(
        preflightRoot + QLatin1Char('/') + manifest.entryPoint());
    if (!entryPoint.isFile() || entryPoint.isSymLink()
        || !m_policy.preflight
        || !m_policy.preflight(manifest, preflightRoot)) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }

    const QString candidateRoot = staging.filePath(QStringLiteral("candidate"));
    if (!QDir().mkdir(candidateRoot)
        || !Archive::extractFiles(files, candidateRoot, m_policy.archiveLimits)
                .hasValue()) {
        return failure(InstallPhase::Candidate,
                       InstallError::CandidateFailed,
                       QStringLiteral("candidate_failed"));
    }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidateCommit) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidateCommit(candidateRoot);
    }
#endif

    const PackageStoreResult candidate = m_store.commitVerifiedCandidate(
        manifest.appId(),
        manifest.version(),
        content.digest().toHex(),
        candidateRoot,
        signedDigest.bytes(),
        signatureFile->contents,
        files,
        m_policy.archiveLimits);
    if (!candidate.succeeded()) {
        return failure(InstallPhase::Candidate,
                       InstallError::CandidateFailed,
                       QStringLiteral("candidate_failed"));
    }
    const QString versionDirectory = QFileInfo(candidate.path).fileName();
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeActivate) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeActivate(manifest.appId(), versionDirectory);
    }
#endif
    const PackageStoreResult activated = m_store.activateVerified(
        manifest.appId(), versionDirectory);
    if (!activated.succeeded()) {
        InstallResult result = failure(InstallPhase::Activate,
                                       InstallError::ActivationFailed,
                                       QStringLiteral("activation_failed"));
        result.appId = manifest.appId();
        result.version = manifest.version();
        result.path = candidate.path;
        return result;
    }
    return {InstallPhase::Complete,
            InstallError::None,
            {},
            manifest.appId(),
            manifest.version(),
            candidate.path,
            activated.activationBinding};
}
