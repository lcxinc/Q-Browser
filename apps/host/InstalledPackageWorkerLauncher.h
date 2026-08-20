#pragma once

#include "IpcSession.h"
#include "SandboxLauncher.h"
#include "SandboxTrustBoundary.h"
#include "UpdateLifecycleCoordinator.h"
#include "WindowsStableIo.h"
#include "WorkerSurface.h"

#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class InstalledPackageWorkerLauncher final : public QObject
{
    Q_OBJECT

public:
    enum class AttachResult
    {
        Attached,
        ConsumedFailure,
    };

    using AttachCallback = std::function<AttachResult(
        std::unique_ptr<IpcSession>,
        std::unique_ptr<WorkerSurface>,
        std::shared_ptr<SandboxProcess>,
        WorkerAttemptKey)>;
    using StopCallback = std::function<void()>;
    using ExitCallback = std::function<void(WorkerAttemptKey, bool)>;
    using FailureCallback = std::function<void(WorkerAttemptKey, const QString &)>;
    using BindingValidator = std::function<InstallResult(
        const QString &, const ActivationBinding &)>;

    InstalledPackageWorkerLauncher(SandboxTrustBoundary boundary,
                                   QString workerExecutable,
                                   QString sandboxTempRoot,
                                   QUrl apiOrigin,
                                   BindingValidator validateBinding,
                                   AttachCallback attach,
                                   StopCallback stop,
                                   ExitCallback exited,
                                   FailureCallback failed,
                                   QObject *parent = nullptr);
    ~InstalledPackageWorkerLauncher() override;

    InstalledPackageWorkerLauncher(const InstalledPackageWorkerLauncher &) = delete;
    InstalledPackageWorkerLauncher &operator=(
        const InstalledPackageWorkerLauncher &) = delete;

    [[nodiscard]] bool requestLaunch(const UpdateLaunchRequest &request);
    void stopCurrent();
    void cancel() noexcept;
    [[nodiscard]] bool isAccepting() const noexcept;
#ifdef Q_BROWSER_HOST_TESTING
    [[nodiscard]] bool retryFatalCleanupForTesting();
#endif

signals:
    void ready(const QString &appId,
               const QString &version,
               const QString &packageDirectory,
               quint64 activation,
               quint64 attempt,
               quint32 processId);
    void unexpectedExit(quint64 activation, quint64 attempt);

private:
    struct LaunchRetirementContext;
    struct ReadyPayload;

    void completeLaunch(quint64 serial, std::shared_ptr<ReadyPayload> payload);
    void observeProcess(std::shared_ptr<LaunchRetirementContext> context);
    void handleRetirement(
        std::shared_ptr<LaunchRetirementContext> context,
        bool succeeded,
        const QString &stableError);
    void fail(WorkerAttemptKey key, const QString &stableError);

    SandboxTrustBoundary boundary_;
    QString workerExecutable_;
    QString sandboxTempRoot_;
    QUrl apiOrigin_;
    BindingValidator validateBinding_;
    AttachCallback attach_;
    StopCallback stop_;
    ExitCallback exited_;
    FailureCallback failed_;
    std::shared_ptr<SandboxProcess> currentProcess_;
    std::shared_ptr<LaunchRetirementContext> currentRetirement_;
    std::optional<WorkerAttemptKey> currentKey_;
    std::optional<WorkerAttemptKey> expectedStop_;
    std::optional<UpdateLaunchRequest> pendingRequest_;
    std::unordered_map<quint64, std::shared_ptr<LaunchRetirementContext>> inflight_;
    std::vector<std::shared_ptr<LaunchRetirementContext>> fatalCleanup_;
    quint64 serial_ = 0;
    bool accepting_ = true;
};
