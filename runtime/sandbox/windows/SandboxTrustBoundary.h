#pragma once

#include "JobLimits.h"
#include "SandboxError.h"

#include <QString>
#include <QStringList>

#include <memory>

struct SandboxApprovedRoots final
{
    QString packageStoreRoot;
    QString sandboxTempRoot;
    QStringList immutableRuntimeRoots;
};

#ifdef Q_BROWSER_SANDBOX_TESTING
enum class SandboxCompatibilityCapabilityForTesting {
    RegistryRead,
    None,
    LpacCom,
};
#endif

struct SandboxLaunchRequest final
{
    QString appId;
    QString executablePath;
    QString packageDirectory;
    QString tempDirectory;
    QStringList arguments;
    SandboxResourceLimits resourceLimits;
#ifdef Q_BROWSER_SANDBOX_TESTING
    SandboxCompatibilityCapabilityForTesting compatibilityCapabilityForTesting =
        SandboxCompatibilityCapabilityForTesting::RegistryRead;
#endif
};

struct SandboxLaunchState;

class SandboxLaunchConfig final
{
public:
    SandboxLaunchConfig() = default;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const QString &appId() const noexcept;
    [[nodiscard]] const QString &executablePath() const noexcept;
    [[nodiscard]] const QString &packageDirectory() const noexcept;
    [[nodiscard]] const QString &tempDirectory() const noexcept;
    [[nodiscard]] const QStringList &runtimeResources() const noexcept;
    [[nodiscard]] const QStringList &arguments() const noexcept;
    [[nodiscard]] const SandboxResourceLimits &resourceLimits() const noexcept;
#ifdef Q_BROWSER_SANDBOX_TESTING
    [[nodiscard]] SandboxCompatibilityCapabilityForTesting
    compatibilityCapabilityForTesting() const noexcept;
#endif

private:
    friend class SandboxLauncher;
    friend class SandboxTrustBoundary;

    explicit SandboxLaunchConfig(std::shared_ptr<const SandboxLaunchState> state,
                                 SandboxLaunchRequest request) noexcept;
    [[nodiscard]] SandboxValueResult<bool> revalidateTrust() const;

    std::shared_ptr<const SandboxLaunchState> state_;
    SandboxLaunchRequest request_;
};

struct SandboxTrustState;

class SandboxTrustBoundary final
{
public:
    SandboxTrustBoundary() = default;
    ~SandboxTrustBoundary() = default;

    SandboxTrustBoundary(const SandboxTrustBoundary &) = delete;
    SandboxTrustBoundary &operator=(const SandboxTrustBoundary &) = delete;
    SandboxTrustBoundary(SandboxTrustBoundary &&) noexcept = default;
    SandboxTrustBoundary &operator=(SandboxTrustBoundary &&) noexcept = default;

    static SandboxValueResult<SandboxTrustBoundary> create(
        const SandboxApprovedRoots &roots);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] SandboxValueResult<SandboxLaunchConfig> makeLaunchConfig(
        const SandboxLaunchRequest &request) const;

private:
    explicit SandboxTrustBoundary(
        std::shared_ptr<const SandboxTrustState> state) noexcept;

    std::shared_ptr<const SandboxTrustState> state_;
};
