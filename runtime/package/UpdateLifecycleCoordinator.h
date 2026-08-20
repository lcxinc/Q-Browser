#pragma once

#include "PackageInstaller.h"
#include "SafeEvent.h"
#include "WorkerSupervisor.h"

#include <QString>

#include <functional>
#include <optional>

class EventRecorder;
class PackageStore;

struct LifecycleClock final
{
    std::function<qint64()> steadyNowMilliseconds;
    std::function<qint64()> utcNowMilliseconds;

    [[nodiscard]] static LifecycleClock system();
    [[nodiscard]] bool isValid() const noexcept;
};

struct UpdateLaunchRequest final
{
    QString appId;
    QString packageVersion;
    QString packageDirectory;
    QString entryPoint;
    WorkerAttemptKey key;
    bool recovery = false;
};

enum class UpdateLifecycleError
{
    None,
    InvalidConfiguration,
    InstallRejected,
    StateUnavailable,
    PackageVerificationFailed,
    LaunchFailed,
    LkgUnavailable,
    StateCommitFailed,
};

enum class UpdateLifecycleAction
{
    None,
    LaunchRequested,
    IgnoredUntilHandshake,
    IgnoredStaleAttempt,
    IgnoredDuplicateFailure,
    MarkedHealthy,
    Restarted,
    RolledBackAndLaunched,
    RecoveredAndLaunched,
    FailedClosed,
};

struct UpdateLifecycleResult final
{
    UpdateLifecycleError error = UpdateLifecycleError::None;
    UpdateLifecycleAction action = UpdateLifecycleAction::None;
    QString stableError;
    QString appId;
    QString version;
    QString path;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == UpdateLifecycleError::None;
    }
};

class UpdateLifecycleCoordinator final
{
public:
    using LaunchCallback = std::function<bool(const UpdateLaunchRequest &)>;
    using BeforeRelaunchCallback = std::function<void()>;

    UpdateLifecycleCoordinator(QString appId,
                               PackageStore &store,
                               PackageInstaller &installer,
                               WorkerSupervisionPolicy supervisionPolicy,
                               LaunchCallback launch,
                               LifecycleClock clock = LifecycleClock::system(),
                               EventRecorder *recorder = nullptr);

    // Configure before dispatching lifecycle work. The callback must stop and
    // detach the old attempt; it runs before any restart or recovery launch.
    void setBeforeRelaunchCallback(BeforeRelaunchCallback callback);

    [[nodiscard]] UpdateLifecycleResult installAndLaunch(
        const QString &packagePath);
    [[nodiscard]] UpdateLifecycleResult startOffline();
    [[nodiscard]] UpdateLifecycleAction authenticatedHandshake(
        WorkerAttemptKey key);
    [[nodiscard]] UpdateLifecycleAction heartbeat(WorkerAttemptKey key);
    [[nodiscard]] UpdateLifecycleAction checkHealth(WorkerAttemptKey key);
    [[nodiscard]] UpdateLifecycleAction workerExited(WorkerAttemptKey key,
                                                     WorkerExitReason reason);
    void beginHostShutdown() noexcept;

    [[nodiscard]] std::optional<WorkerAttemptKey> currentAttemptKey() const noexcept;
    [[nodiscard]] bool failedClosed() const noexcept;

private:
    [[nodiscard]] UpdateLifecycleResult beginLaunch(
        QString version,
        QString path,
        QString entryPoint,
        ActivationBinding binding,
        qint64 nowMs,
        bool recovery,
        UpdateLifecycleAction successAction);
    [[nodiscard]] UpdateLifecycleAction applySupervisionAction(
        WorkerSupervisionAction action,
        qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction restart(qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction rollbackAndRecover(qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction transitionToHealthy(
        WorkerAttemptKey key,
        qint64 steadyNowMs);
    void stopCurrentAttempt();
    [[nodiscard]] UpdateLifecycleAction enterFailedClosed();
    void record(SafeEventPhase phase,
                SafeEventCode code,
                qint64 durationMs,
                const SafeMetrics &metrics = {}) const;

    QString appId_;
    PackageStore &store_;
    PackageInstaller &installer_;
    LaunchCallback launch_;
    BeforeRelaunchCallback beforeRelaunch_;
    EventRecorder *recorder_ = nullptr;
    LifecycleClock clock_;
    WorkerSupervisor supervisor_;
    QString currentVersion_;
    QString currentPath_;
    QString currentEntryPoint_;
    std::optional<ActivationBinding> currentBinding_;
    std::optional<WorkerAttemptKey> currentKey_;
    bool currentHealthy_ = false;
    bool handshakeAccepted_ = false;
    bool recoveryLaunch_ = false;
    bool restartRequested_ = false;
    bool rollbackRequested_ = false;
    bool hostShuttingDown_ = false;
    bool failedClosed_ = false;
    bool currentAttemptStopped_ = true;
};
