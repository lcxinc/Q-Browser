#pragma once

#include "ArchiveLimits.h"
#include "ActivationState.h"
#include "Manifest.h"

#include <QByteArray>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

class PackageStore;

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
    std::optional<ActivationBinding> activationBinding;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == InstallError::None && phase == InstallPhase::Complete;
    }
};

struct InstallPolicy final
{
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
        const ActivationBinding &expected) const;

private:
    PackageStore &m_store;
    QByteArray m_trustedPublicKeyPem;
    InstallPolicy m_policy;
};
