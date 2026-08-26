#pragma once

#include "ArchiveLimits.h"
#include "ActivationState.h"
#include "Manifest.h"

#include <QByteArray>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class PackageStore;
class ImmutablePackageGuard;

enum class InstallPhase
{
    Staging,
    Verify,
    Preflight,
    Candidate,
    Activate,
    Complete,
};

enum class InstallError
{
    None,
    StagingFailed,
    ArchiveInvalid,
    SignatureMissing,
    SignatureInvalid,
    ContentInvalid,
    ManifestInvalid,
    RuntimeIncompatible,
    ImportDenied,
    PreflightRejected,
    AppIdMismatch,
    CandidateFailed,
    ActivationFailed,
};

struct InstallResult final
{
    InstallPhase phase = InstallPhase::Staging;
    InstallError error = InstallError::None;
    QString stableError;
    QString appId;
    QString version;
    QString path;
    QString entryPoint;
    ManifestPermissions permissions;
    std::optional<ActivationBinding> activationBinding;
    std::shared_ptr<const ImmutablePackageGuard> immutableGuard;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == InstallError::None && phase == InstallPhase::Complete;
    }
};

class ImmutablePackageGuard final
{
public:
    ~ImmutablePackageGuard();

    ImmutablePackageGuard(const ImmutablePackageGuard &) = delete;
    ImmutablePackageGuard &operator=(const ImmutablePackageGuard &) = delete;
    ImmutablePackageGuard(ImmutablePackageGuard &&) = delete;
    ImmutablePackageGuard &operator=(ImmutablePackageGuard &&) = delete;

private:
    friend class PackageInstaller;

    struct State;
    explicit ImmutablePackageGuard(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

struct VerifiedPackageLease final
{
    QString appId;
    QString version;
    QString versionDirectory;
    QString packageDirectory;
    QString entryPoint;
    ManifestPermissions permissions;
    QByteArray digestHex;
    qint64 activationGenerationAtIssue = 0;
    quint64 leaseAuthorityEpoch = 0;

    friend bool operator==(const VerifiedPackageLease &left,
                           const VerifiedPackageLease &right)
    {
        return left.appId == right.appId
            && left.version == right.version
            && left.versionDirectory == right.versionDirectory
            && left.packageDirectory == right.packageDirectory
            && left.entryPoint == right.entryPoint
            && left.permissions.network.hosts
                   == right.permissions.network.hosts
            && left.permissions.network.methods
                   == right.permissions.network.methods
            && left.permissions.storage == right.permissions.storage
            && left.permissions.clipboardWrite
                   == right.permissions.clipboardWrite
            && left.permissions.clipboardRead
                   == right.permissions.clipboardRead
            && left.permissions.fileOpen == right.permissions.fileOpen
            && left.digestHex == right.digestHex
            && left.activationGenerationAtIssue
                   == right.activationGenerationAtIssue
            && left.leaseAuthorityEpoch == right.leaseAuthorityEpoch;
    }
};

struct InstallPolicy final
{
    QString expectedAppId;
    QString runtimeVersion;
    QSet<QString> allowedImports;
    std::function<bool(const Manifest &, const QString &)> preflight;
    ArchiveLimits archiveLimits;
};

class PackageInstaller final
{
public:
    PackageInstaller(PackageStore &store,
                     QByteArray trustedPublicKeyPem,
                     InstallPolicy policy);

    [[nodiscard]] InstallResult install(const QString &packagePath) const;
    [[nodiscard]] InstallResult verifyInstalled(
        const QString &appId,
        const QString &versionDirectory) const;
    [[nodiscard]] InstallResult reverifyInstalledVersion(
        const QString &appId,
        const ActivationBinding &expected,
        std::shared_ptr<const ImmutablePackageGuard> retainedGuard = {}) const;
    [[nodiscard]] InstallResult reverifyPinnedLease(
        const VerifiedPackageLease &lease,
        std::shared_ptr<const ImmutablePackageGuard> retainedGuard = {}) const;

private:
    [[nodiscard]] InstallResult verifyInstalledImmutable(
        const QString &appId,
        const QString &versionDirectory,
        std::shared_ptr<const ImmutablePackageGuard> retainedGuard) const;
    PackageStore &m_store;
    QByteArray m_trustedPublicKeyPem;
    InstallPolicy m_policy;
};
