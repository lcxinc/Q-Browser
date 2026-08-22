#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#endif

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
        const QStringList &arguments);

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
    [[nodiscard]] const std::optional<QString> &installPackage() const noexcept;
    [[nodiscard]] qint64 healthWindowMs() const noexcept;
    [[nodiscard]] qint64 heartbeatTimeoutMs() const noexcept;

private:
    friend struct HostRuntimeConfigResult;

    HostRuntimeMode mode_ = HostRuntimeMode::TrustedShell;
    QUrl mockOrigin_{QStringLiteral("http://127.0.0.1:4173/")};
    QString appId_;
    QByteArray trustedPublicKeyPem_;
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsFileIdentity trustedPublicKeyIdentity_;
#endif
    QString packageStoreRoot_;
    QString sandboxTempRoot_;
    QStringList immutableRuntimeRoots_;
    QString workerExecutable_;
    QString telemetryDirectory_;
    QString storageDirectory_;
    std::optional<QString> installPackage_;
    qint64 healthWindowMs_ = 10'000;
    qint64 heartbeatTimeoutMs_ = 2'000;
};

struct HostRuntimeConfigResult final
{
    std::optional<HostRuntimeConfig> value;
    HostRuntimeConfigError error = HostRuntimeConfigError::None;
    QString stableError;
};
