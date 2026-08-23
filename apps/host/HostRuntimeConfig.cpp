#include "HostRuntimeConfig.h"

#include "HostOwnedFileAuthority.h"
#include "HostOwnedStateDirectory.h"
#include "SignatureVerifier.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <array>

namespace
{
constexpr qint64 MaximumPublicKeyBytes = 64 * 1024;

#ifdef Q_OS_WIN
QString windowsApiPath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.sliced(2);
    }
    return QStringLiteral("\\\\?\\") + native;
}
#endif

HostRuntimeConfigResult failure(const HostRuntimeConfigError error,
                                const QString &stableError)
{
    return {std::nullopt, error, stableError};
}

bool isValidAppId(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral(
        "^[a-z][a-z0-9]*(?:\\.[a-z0-9][a-z0-9-]*)+$"));
    return value.size() <= 128 && expression.match(value).hasMatch();
}

bool hasReparseComponent(const QString &absolutePath)
{
#ifdef Q_OS_WIN
    QFileInfo information(absolutePath);
    QString current = information.isDir() ? information.absoluteFilePath()
                                          : information.absolutePath();
    for (;;) {
        const QString apiPath = windowsApiPath(current);
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(apiPath.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
#else
    QFileInfo information(absolutePath);
    QString current = information.isDir() ? information.absoluteFilePath()
                                          : information.absolutePath();
    for (;;) {
        if (QFileInfo(current).isSymLink()) return true;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
#endif
    return false;
}

std::optional<QString> safeExistingPath(const QString &value, const bool directory)
{
    const QFileInfo information(value);
    if (value.isEmpty() || !information.isAbsolute() || information.isSymLink()
        || (directory ? !information.isDir() : !information.isFile())
        || hasReparseComponent(information.absoluteFilePath())) {
        return std::nullopt;
    }
    const QString canonical = directory ? QDir(value).canonicalPath()
                                        : information.canonicalFilePath();
    if (canonical.isEmpty()) return std::nullopt;
    return QDir::cleanPath(canonical);
}

bool overlaps(const QString &left, const QString &right)
{
    const QString leftPrefix = QDir::toNativeSeparators(left) + QDir::separator();
    const QString rightPrefix = QDir::toNativeSeparators(right) + QDir::separator();
    const QString nativeLeft = QDir::toNativeSeparators(left);
    const QString nativeRight = QDir::toNativeSeparators(right);
    return nativeLeft.compare(nativeRight, Qt::CaseInsensitive) == 0
        || nativeLeft.startsWith(rightPrefix, Qt::CaseInsensitive)
        || nativeRight.startsWith(leftPrefix, Qt::CaseInsensitive);
}

bool isStrictlyWithin(const QString &root, const QString &candidate)
{
    const QString nativeRoot = QDir::toNativeSeparators(root);
    const QString nativeCandidate = QDir::toNativeSeparators(candidate);
    return nativeCandidate.startsWith(
        nativeRoot + QDir::separator(), Qt::CaseInsensitive);
}

QString argumentValue(const QString &argument, const QStringView name)
{
    const QString prefix = QStringLiteral("--") + name + QLatin1Char('=');
    return argument.startsWith(prefix) ? argument.sliced(prefix.size()) : QString{};
}
}

HostRuntimeConfigResult HostRuntimeConfig::fromArguments(
    const QStringList &arguments,
    const HostRuntimeParseContext &context)
{
    HostRuntimeConfig config;
    bool trustedShell = false;
    bool packageMode = false;
    QHash<QString, QString> singleValues;
    QStringList runtimeRoots;
    for (const QString &argument : arguments) {
        if (argument == QStringLiteral("--trusted-shell")) {
            if (trustedShell) {
                return failure(HostRuntimeConfigError::DuplicateArgument,
                               QStringLiteral("host.config.duplicate_argument"));
            }
            trustedShell = true;
            continue;
        }
        if (argument == QStringLiteral("--package-mode")) {
            if (packageMode) {
                return failure(HostRuntimeConfigError::DuplicateArgument,
                               QStringLiteral("host.config.duplicate_argument"));
            }
            packageMode = true;
            continue;
        }
        static constexpr std::array<QStringView, 14> names{
            u"mock-origin", u"app-id", u"trusted-public-key", u"package-store",
            u"sandbox-temp", u"runtime-root", u"worker-executable",
            u"telemetry-directory", u"install-package", u"health-window-ms",
            u"heartbeat-timeout-ms", u"storage-directory", u"deployment-root",
            u"browser-state-directory"};
        bool recognized = false;
        for (const QStringView name : names) {
            const QString value = argumentValue(argument, name);
            if (value.isNull() || value.isEmpty()) continue;
            recognized = true;
            if (name == u"runtime-root") {
                runtimeRoots.push_back(value);
            } else {
                const QString key(name);
                if (singleValues.contains(key)) {
                    return failure(HostRuntimeConfigError::DuplicateArgument,
                                   QStringLiteral("host.config.duplicate_argument"));
                }
                singleValues.insert(key, value);
            }
            break;
        }
        if (!recognized) {
            return failure(HostRuntimeConfigError::UnknownArgument,
                           QStringLiteral("host.config.unknown_argument"));
        }
    }
    if (!trustedShell && !packageMode) {
        return failure(HostRuntimeConfigError::ModeRequired,
                       QStringLiteral("host.config.mode_required"));
    }
    if (trustedShell && packageMode) {
        return failure(HostRuntimeConfigError::AmbiguousMode,
                       QStringLiteral("host.config.ambiguous_mode"));
    }
    if (singleValues.contains(QStringLiteral("mock-origin"))) {
        QUrl origin(singleValues.value(QStringLiteral("mock-origin")),
                    QUrl::StrictMode);
        if (!origin.isValid() || origin.scheme() != QStringLiteral("http")
            || origin.host() != QStringLiteral("127.0.0.1")
            || origin.port() <= 0 || !origin.userInfo().isEmpty()
            || (!origin.path().isEmpty() && origin.path() != QLatin1String("/"))
            || origin.hasQuery() || origin.hasFragment()) {
            return failure(HostRuntimeConfigError::MissingArgument,
                           QStringLiteral("host.config.invalid_mock_origin"));
        }
        origin.setPath(QStringLiteral("/"));
        config.mockOrigin_ = origin;
    }
    if (trustedShell) {
        if (singleValues.size() > (singleValues.contains(QStringLiteral("mock-origin")) ? 1 : 0)
            || !runtimeRoots.isEmpty()) {
            return failure(HostRuntimeConfigError::AmbiguousMode,
                           QStringLiteral("host.config.shell_has_package_authority"));
        }
        return {std::move(config), HostRuntimeConfigError::None, {}};
    }

    if (!singleValues.contains(QStringLiteral("deployment-root"))) {
        return failure(HostRuntimeConfigError::MissingArgument,
                       QStringLiteral("host.config.missing_deployment_root"));
    }
    if (!singleValues.contains(QStringLiteral("browser-state-directory"))) {
        return failure(
            HostRuntimeConfigError::MissingArgument,
            QStringLiteral("host.config.missing_browser_state_directory"));
    }

    const QStringList required{QStringLiteral("app-id"),
                               QStringLiteral("trusted-public-key"),
                               QStringLiteral("package-store"),
                               QStringLiteral("sandbox-temp"),
                               QStringLiteral("worker-executable"),
                               QStringLiteral("telemetry-directory"),
                               QStringLiteral("storage-directory")};
    for (const QString &name : required) {
        if (!singleValues.contains(name)) {
            if (name == QStringLiteral("storage-directory")) {
                return failure(HostRuntimeConfigError::MissingArgument,
                               QStringLiteral("host.config.missing_storage_directory"));
            }
            return failure(HostRuntimeConfigError::MissingArgument,
                           QStringLiteral("host.config.missing_argument"));
        }
    }
    if (runtimeRoots.isEmpty()) {
        return failure(HostRuntimeConfigError::MissingArgument,
                       QStringLiteral("host.config.missing_runtime_root"));
    }
    if (!isValidAppId(singleValues.value(QStringLiteral("app-id")))) {
        return failure(HostRuntimeConfigError::InvalidAppId,
                       QStringLiteral("host.config.invalid_app_id"));
    }
    config.mode_ = HostRuntimeMode::Package;
    config.appId_ = singleValues.value(QStringLiteral("app-id"));

    const auto deployment = safeExistingPath(
        singleValues.value(QStringLiteral("deployment-root")), true);
    const auto browserState = safeExistingPath(
        singleValues.value(QStringLiteral("browser-state-directory")), true);
    if (!deployment.has_value()) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.unsafe_deployment_root"));
    }
    if (!browserState.has_value()) {
        return failure(
            HostRuntimeConfigError::UnsafePath,
            QStringLiteral("host.config.unsafe_browser_state_directory"));
    }
    if (!context.currentHostExecutable
        || !context.currentHostExecutable->revalidate()) {
        return failure(
            HostRuntimeConfigError::UnsafePath,
            QStringLiteral("host.config.current_host_unavailable"));
    }

    const auto keyPath = safeExistingPath(
        singleValues.value(QStringLiteral("trusted-public-key")), false);
    const auto store = safeExistingPath(
        singleValues.value(QStringLiteral("package-store")), true);
    const auto sandboxTemp = safeExistingPath(
        singleValues.value(QStringLiteral("sandbox-temp")), true);
    const auto worker = safeExistingPath(
        singleValues.value(QStringLiteral("worker-executable")), false);
    const auto telemetry = safeExistingPath(
        singleValues.value(QStringLiteral("telemetry-directory")), true);
    const auto storage = safeExistingPath(
        singleValues.value(QStringLiteral("storage-directory")), true);
    if (!keyPath.has_value()) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.unsafe_public_key_path"));
    }
    if (!store.has_value() || !sandboxTemp.has_value() || !worker.has_value()
        || !telemetry.has_value() || !storage.has_value()) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.unsafe_path"));
    }
    config.deploymentRoot_ = *deployment;
    config.browserStateDirectory_ = *browserState;
    config.packageStoreRoot_ = *store;
    config.sandboxTempRoot_ = *sandboxTemp;
    config.workerExecutable_ = *worker;
    config.telemetryDirectory_ = *telemetry;
    config.storageDirectory_ = *storage;
    for (const QString &runtimeRoot : runtimeRoots) {
        const auto root = safeExistingPath(runtimeRoot, true);
        if (!root.has_value()) {
            return failure(HostRuntimeConfigError::UnsafePath,
                           QStringLiteral("host.config.unsafe_runtime_root"));
        }
        config.immutableRuntimeRoots_.push_back(*root);
    }

    const QString keyParent = QDir(QFileInfo(*keyPath).absolutePath())
        .canonicalPath();
    if (keyParent.isEmpty()
        || !isStrictlyWithin(
            config.deploymentRoot_,
            context.currentHostExecutable->canonicalPath())
        || !isStrictlyWithin(config.deploymentRoot_, config.workerExecutable_)
        || !isStrictlyWithin(config.deploymentRoot_, *keyPath)) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.content_outside_deployment"));
    }
    for (const QString &runtimeRoot : config.immutableRuntimeRoots_) {
        if (!isStrictlyWithin(config.deploymentRoot_, runtimeRoot)) {
            return failure(
                HostRuntimeConfigError::UnsafePath,
                QStringLiteral("host.config.runtime_outside_deployment"));
        }
    }

    bool workerInRuntime = false;
    for (const QString &runtimeRoot : config.immutableRuntimeRoots_) {
        const QString prefix = QDir::toNativeSeparators(runtimeRoot)
            + QDir::separator();
        if (QDir::toNativeSeparators(config.workerExecutable_)
                .startsWith(prefix, Qt::CaseInsensitive)) {
            workerInRuntime = true;
            break;
        }
    }
    if (!workerInRuntime) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.worker_outside_runtime"));
    }

    QStringList mutableRoots{config.packageStoreRoot_, config.sandboxTempRoot_,
                             config.telemetryDirectory_, config.storageDirectory_,
                             config.browserStateDirectory_};
    for (const QString &root : mutableRoots) {
        if (overlaps(config.deploymentRoot_, root)) {
            return failure(HostRuntimeConfigError::OverlappingRoots,
                           QStringLiteral("host.config.overlapping_roots"));
        }
    }
    for (qsizetype left = 0; left < mutableRoots.size(); ++left) {
        for (qsizetype right = left + 1; right < mutableRoots.size(); ++right) {
            if (overlaps(mutableRoots[left], mutableRoots[right])) {
                return failure(HostRuntimeConfigError::OverlappingRoots,
                               QStringLiteral("host.config.overlapping_roots"));
            }
        }
    }
    for (const QString &runtimeRoot : config.immutableRuntimeRoots_) {
        if (overlaps(config.browserStateDirectory_, runtimeRoot)) {
            return failure(HostRuntimeConfigError::OverlappingRoots,
                           QStringLiteral("host.config.overlapping_roots"));
        }
    }
    if (overlaps(config.browserStateDirectory_, keyParent)) {
        return failure(HostRuntimeConfigError::OverlappingRoots,
                       QStringLiteral("host.config.overlapping_roots"));
    }

    std::optional<QString> installPackage;
    QString externalPackageParent;
    if (singleValues.contains(QStringLiteral("install-package"))) {
        installPackage = safeExistingPath(
            singleValues.value(QStringLiteral("install-package")), false);
        if (!installPackage.has_value()) {
            return failure(HostRuntimeConfigError::UnsafePath,
                           QStringLiteral("host.config.unsafe_install_package"));
        }
        const QString packageParent = QDir(
            QFileInfo(*installPackage).absolutePath()).canonicalPath();
        const auto canonicalDeployedPackages = safeExistingPath(
            QDir(config.deploymentRoot_).filePath(QStringLiteral("packages")),
            true);
        const bool deployedPackage = canonicalDeployedPackages.has_value()
            && isStrictlyWithin(*canonicalDeployedPackages, *installPackage);
        if (!deployedPackage) {
            if (packageParent.isEmpty()
                || overlaps(config.deploymentRoot_, packageParent)) {
                return failure(HostRuntimeConfigError::OverlappingRoots,
                               QStringLiteral("host.config.overlapping_roots"));
            }
            for (const QString &root : mutableRoots) {
                if (overlaps(root, packageParent)) {
                    return failure(HostRuntimeConfigError::OverlappingRoots,
                                   QStringLiteral("host.config.overlapping_roots"));
                }
            }
            externalPackageParent = packageParent;
        }
    }

    QStringList stateDisjointFrom{config.deploymentRoot_,
                                  config.packageStoreRoot_,
                                  config.sandboxTempRoot_,
                                  config.telemetryDirectory_,
                                  config.storageDirectory_, keyParent};
    stateDisjointFrom.append(config.immutableRuntimeRoots_);
    if (!externalPackageParent.isEmpty()) {
        stateDisjointFrom.push_back(externalPackageParent);
    }
    QStringList deploymentDisjointFrom = mutableRoots;
    if (!externalPackageParent.isEmpty()) {
        deploymentDisjointFrom.push_back(externalPackageParent);
    }
    config.deploymentAuthority_ = HostOwnedStateDirectory::open(
        config.deploymentRoot_, deploymentDisjointFrom);
    config.browserStateAuthority_ = HostOwnedStateDirectory::open(
        config.browserStateDirectory_, stateDisjointFrom);
    config.currentHostExecutableAuthority_ = context.currentHostExecutable;
    config.workerExecutableAuthority_ = HostOwnedFileAuthority::open(
        config.workerExecutable_);
    config.trustedPublicKeyAuthority_ = HostOwnedFileAuthority::open(*keyPath);
    if (!config.deploymentAuthority_ || !config.browserStateAuthority_
        || !config.workerExecutableAuthority_
        || !config.trustedPublicKeyAuthority_) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.unsafe_protected_authority"));
    }
    QByteArray keyBytes;
    if (!config.trustedPublicKeyAuthority_->readBounded(
            MaximumPublicKeyBytes, keyBytes)
        || !config.trustedPublicKeyAuthority_->revalidate()
        || !SignatureVerifier::isValidPublicKeyPem(keyBytes)) {
        return failure(HostRuntimeConfigError::PublicKeyUnavailable,
                       QStringLiteral("host.config.public_key_unavailable"));
    }
    config.trustedPublicKeyPem_ = std::move(keyBytes);

    if (installPackage.has_value()) {
        if (!externalPackageParent.isEmpty()) {
            config.installPackageAuthority_ = HostOwnedFileAuthority::open(
                *installPackage);
            if (!config.installPackageAuthority_
                || !config.installPackageAuthority_->revalidate()) {
                return failure(
                    HostRuntimeConfigError::UnsafePath,
                    QStringLiteral("host.config.unsafe_install_package"));
            }
        }
        config.installPackage_ = *installPackage;
    }
    const auto boundedMilliseconds = [&](const QString &name,
                                         const qint64 defaultValue)
        -> std::optional<qint64> {
        if (!singleValues.contains(name)) return defaultValue;
        bool converted = false;
        const qint64 value = singleValues.value(name).toLongLong(&converted, 10);
        if (!converted || value < 100 || value > 60'000
            || QString::number(value) != singleValues.value(name)) {
            return std::nullopt;
        }
        return value;
    };
    const auto healthWindow = boundedMilliseconds(
        QStringLiteral("health-window-ms"), config.healthWindowMs_);
    const auto heartbeatTimeout = boundedMilliseconds(
        QStringLiteral("heartbeat-timeout-ms"), config.heartbeatTimeoutMs_);
    if (!healthWindow.has_value() || !heartbeatTimeout.has_value()) {
        return failure(HostRuntimeConfigError::MissingArgument,
                       QStringLiteral("host.config.invalid_timing"));
    }
    config.healthWindowMs_ = *healthWindow;
    config.heartbeatTimeoutMs_ = *heartbeatTimeout;
    if (!config.deploymentAuthority_->revalidate()
        || !config.browserStateAuthority_->revalidate()
        || !config.currentHostExecutableAuthority_->revalidate()
        || !config.workerExecutableAuthority_->revalidate()
        || !config.trustedPublicKeyAuthority_->revalidate()
        || (config.installPackageAuthority_
            && !config.installPackageAuthority_->revalidate())) {
        return failure(HostRuntimeConfigError::UnsafePath,
                       QStringLiteral("host.config.authority_changed"));
    }
    return {std::move(config), HostRuntimeConfigError::None, {}};
}

HostRuntimeMode HostRuntimeConfig::mode() const noexcept { return mode_; }
const QUrl &HostRuntimeConfig::mockOrigin() const noexcept { return mockOrigin_; }
const QString &HostRuntimeConfig::appId() const noexcept { return appId_; }
const QByteArray &HostRuntimeConfig::trustedPublicKeyPem() const noexcept
{
    return trustedPublicKeyPem_;
}
const QString &HostRuntimeConfig::packageStoreRoot() const noexcept
{
    return packageStoreRoot_;
}
const QString &HostRuntimeConfig::sandboxTempRoot() const noexcept
{
    return sandboxTempRoot_;
}
const QStringList &HostRuntimeConfig::immutableRuntimeRoots() const noexcept
{
    return immutableRuntimeRoots_;
}
const QString &HostRuntimeConfig::workerExecutable() const noexcept
{
    return workerExecutable_;
}
const QString &HostRuntimeConfig::telemetryDirectory() const noexcept
{
    return telemetryDirectory_;
}
const QString &HostRuntimeConfig::storageDirectory() const noexcept
{
    return storageDirectory_;
}
const QString &HostRuntimeConfig::deploymentRoot() const noexcept
{
    return deploymentRoot_;
}
const QString &HostRuntimeConfig::browserStateDirectory() const noexcept
{
    return browserStateDirectory_;
}
const std::optional<QString> &HostRuntimeConfig::installPackage() const noexcept
{
    return installPackage_;
}
const std::shared_ptr<const HostOwnedStateDirectory> &
HostRuntimeConfig::deploymentAuthority() const noexcept
{
    return deploymentAuthority_;
}
const std::shared_ptr<const HostOwnedStateDirectory> &
HostRuntimeConfig::browserStateAuthority() const noexcept
{
    return browserStateAuthority_;
}
const std::shared_ptr<const HostOwnedFileAuthority> &
HostRuntimeConfig::installPackageAuthority() const noexcept
{
    return installPackageAuthority_;
}
qint64 HostRuntimeConfig::healthWindowMs() const noexcept { return healthWindowMs_; }
qint64 HostRuntimeConfig::heartbeatTimeoutMs() const noexcept
{
    return heartbeatTimeoutMs_;
}
