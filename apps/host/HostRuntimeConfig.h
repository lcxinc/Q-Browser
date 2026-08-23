#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>
#include <memory>

class HostOwnedFileAuthority;
class HostOwnedStateDirectory;

struct HostRuntimeParseContext final
{
    std::shared_ptr<const HostOwnedFileAuthority> currentHostExecutable;
};

enum class HostRuntimeMode
{
    TrustedShell,
    Package,
};

enum class HostRuntimeConfigError
{
    None,
    ModeRequired,
    AmbiguousMode,
    UnknownArgument,
    DuplicateArgument,
    MissingArgument,
    InvalidAppId,
    PublicKeyUnavailable,
    UnsafePath,
    OverlappingRoots,
};

class HostRuntimeConfig final
{
public:
    [[nodiscard]] static struct HostRuntimeConfigResult fromArguments(
        const QStringList &arguments,
        const HostRuntimeParseContext &context = {});

    [[nodiscard]] HostRuntimeMode mode() const noexcept;
    [[nodiscard]] const QUrl &mockOrigin() const noexcept;
    [[nodiscard]] const QString &appId() const noexcept;
    [[nodiscard]] const QByteArray &trustedPublicKeyPem() const noexcept;
    [[nodiscard]] const QString &packageStoreRoot() const noexcept;
    [[nodiscard]] const QString &sandboxTempRoot() const noexcept;
    [[nodiscard]] const QStringList &immutableRuntimeRoots() const noexcept;
    [[nodiscard]] const QString &workerExecutable() const noexcept;
    [[nodiscard]] const QString &telemetryDirectory() const noexcept;
    [[nodiscard]] const QString &storageDirectory() const noexcept;
    [[nodiscard]] const QString &deploymentRoot() const noexcept;
    [[nodiscard]] const QString &browserStateDirectory() const noexcept;
    [[nodiscard]] const std::optional<QString> &installPackage() const noexcept;
    [[nodiscard]] const std::shared_ptr<const HostOwnedStateDirectory> &
    deploymentAuthority() const noexcept;
    [[nodiscard]] const std::shared_ptr<const HostOwnedStateDirectory> &
    browserStateAuthority() const noexcept;
    [[nodiscard]] const std::shared_ptr<const HostOwnedFileAuthority> &
    installPackageAuthority() const noexcept;
    [[nodiscard]] qint64 healthWindowMs() const noexcept;
    [[nodiscard]] qint64 heartbeatTimeoutMs() const noexcept;

private:
    friend struct HostRuntimeConfigResult;

    HostRuntimeMode mode_ = HostRuntimeMode::TrustedShell;
    QUrl mockOrigin_{QStringLiteral("http://127.0.0.1:4173/")};
    QString appId_;
    QByteArray trustedPublicKeyPem_;
    QString packageStoreRoot_;
    QString sandboxTempRoot_;
    QStringList immutableRuntimeRoots_;
    QString workerExecutable_;
    QString telemetryDirectory_;
    QString storageDirectory_;
    QString deploymentRoot_;
    QString browserStateDirectory_;
    std::optional<QString> installPackage_;
    std::shared_ptr<const HostOwnedStateDirectory> deploymentAuthority_;
    std::shared_ptr<const HostOwnedStateDirectory> browserStateAuthority_;
    std::shared_ptr<const HostOwnedFileAuthority> currentHostExecutableAuthority_;
    std::shared_ptr<const HostOwnedFileAuthority> workerExecutableAuthority_;
    std::shared_ptr<const HostOwnedFileAuthority> trustedPublicKeyAuthority_;
    std::shared_ptr<const HostOwnedFileAuthority> installPackageAuthority_;
    qint64 healthWindowMs_ = 10'000;
    qint64 heartbeatTimeoutMs_ = 2'000;
};

struct HostRuntimeConfigResult final
{
    std::optional<HostRuntimeConfig> value;
    HostRuntimeConfigError error = HostRuntimeConfigError::None;
    QString stableError;
};
