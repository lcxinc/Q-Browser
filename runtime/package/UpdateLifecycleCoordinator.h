#pragma once

#include "PackageInstaller.h"
#include "SafeEvent.h"
#include "WorkerSupervisor.h"

#include <QString>

#include <functional>
#include <optional>

class EventRecorder;
class PackageStore;

struct UpdateLaunchRequest final
{
    QString appId;
    QString packageVersion;
    QString packageDirectory;
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
                               EventRecorder *recorder = nullptr);

    // Configure before dispatching lifecycle work. The callback must stop and
    // detach the old attempt; it runs before any restart or recovery launch.
    void setBeforeRelaunchCallback(BeforeRelaunchCallback callback);

    [[nodiscard]] UpdateLifecycleResult installAndLaunch(
        const QString &packagePath,
        qint64 nowMs);
    [[nodiscard]] UpdateLifecycleResult startOffline(qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction authenticatedHandshake(
        WorkerAttemptKey key,
        qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction heartbeat(WorkerAttemptKey key,
                                                  qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction checkHealth(WorkerAttemptKey key,
                                                    qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction workerExited(WorkerAttemptKey key,
                                                     WorkerExitReason reason,
                                                     qint64 nowMs);
    void beginHostShutdown() noexcept;

    [[nodiscard]] std::optional<WorkerAttemptKey> currentAttemptKey() const noexcept;
    [[nodiscard]] bool failedClosed() const noexcept;

private:
    [[nodiscard]] UpdateLifecycleResult beginLaunch(
        QString version,
        QString path,
        qint64 nowMs,
        bool recovery,
        UpdateLifecycleAction successAction);
    [[nodiscard]] UpdateLifecycleAction applySupervisionAction(
        WorkerSupervisionAction action,
        qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction restart(qint64 nowMs);
    [[nodiscard]] UpdateLifecycleAction rollbackAndRecover(qint64 nowMs);
    void stopCurrentAttempt();
    [[nodiscard]] UpdateLifecycleAction enterFailedClosed();
    void record(SafeEventPhase phase,
                SafeEventCode code,
                qint64 nowMs,
                qint64 durationMs,
                const SafeMetrics &metrics = {}) const;

    QString appId_;
    PackageStore &store_;
    PackageInstaller &installer_;
    LaunchCallback launch_;
    BeforeRelaunchCallback beforeRelaunch_;
    EventRecorder *recorder_ = nullptr;
    WorkerSupervisor supervisor_;
    QString currentVersion_;
    QString currentPath_;
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
