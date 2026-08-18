#include "PackageInstaller.h"

#include "Archive.h"
#include "ContentDigest.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QVersionNumber>

#include <algorithm>
#include <utility>

namespace
{
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
}

PackageInstaller::PackageInstaller(PackageStore &store,
                                   QByteArray trustedPublicKeyPem,
                                   InstallPolicy policy)
    : m_store(store)
    , m_trustedPublicKeyPem(std::move(trustedPublicKeyPem))
    , m_policy(std::move(policy))
{
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

    const QString payloadRoot = staging.filePath(QStringLiteral("payload"));
    if (!QDir().mkdir(payloadRoot)
        || !Archive::extractFiles(files, payloadRoot, m_policy.archiveLimits)
                .hasValue()) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }
    const QFileInfo entryPoint(
        payloadRoot + QLatin1Char('/') + manifest.entryPoint());
    if (!entryPoint.isFile() || entryPoint.isSymLink()
        || !m_policy.preflight
        || !m_policy.preflight(manifest, payloadRoot)) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }

    const PackageStoreResult candidate = m_store.commitCandidate(
        manifest.appId(), manifest.version(), content.digest().toHex(), payloadRoot);
    if (!candidate.succeeded()) {
        return failure(InstallPhase::Candidate,
                       InstallError::CandidateFailed,
                       QStringLiteral("candidate_failed"));
    }
    const QString versionDirectory = QFileInfo(candidate.path).fileName();
    const PackageStoreResult activated = m_store.activate(
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
            candidate.path};
}
