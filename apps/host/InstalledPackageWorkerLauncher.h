#pragma once

#include "IpcSession.h"
#include "SandboxLauncher.h"
#include "SandboxTrustBoundary.h"
#include "WindowsStableIo.h"
#include "WorkerLaunchRequest.h"
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

    struct AdmissionResult final
    {
        bool accepted = false;
        QString stableError;
        bool ignoredStale = false;
        std::optional<WorkerLaunchRequest> authority;
    };

    class CommittedAttachTransaction final
    {
    public:
        ~CommittedAttachTransaction();
        CommittedAttachTransaction(const CommittedAttachTransaction &) = delete;
        CommittedAttachTransaction &operator=(
            const CommittedAttachTransaction &) = delete;
        CommittedAttachTransaction(CommittedAttachTransaction &&) noexcept;
        CommittedAttachTransaction &operator=(
            CommittedAttachTransaction &&) noexcept;

        [[nodiscard]] const WorkerLaunchRequest &request() const noexcept;
        [[nodiscard]] quint32 processId() const noexcept;
        [[nodiscard]] std::unique_ptr<IpcSession> takeSession() noexcept;
        [[nodiscard]] std::unique_ptr<WorkerSurface> takeSurface() noexcept;
        [[nodiscard]] std::shared_ptr<SandboxProcess> takeProcess() noexcept;

    private:
        friend class InstalledPackageWorkerLauncher;
        struct State;
        explicit CommittedAttachTransaction(std::unique_ptr<State> state);

        std::unique_ptr<State> state_;
    };

    using AttachCallback = std::function<AttachResult(
        CommittedAttachTransaction)>;
    using StopCallback = std::function<void()>;
    using ExitCallback = std::function<void(WorkerAttemptKey, bool)>;
    using FailureCallback = std::function<void(WorkerAttemptKey,
                                               const QString &,
                                               quint32)>;
    using BindingValidator = std::function<InstallResult(
        const WorkerLaunchRequest &,
        std::shared_ptr<const ImmutablePackageGuard>)>;
    using AdmissionCompletion = std::function<void(AdmissionResult)>;
    using AdmissionCallback = std::function<bool(
        const WorkerLaunchRequest &, AdmissionCompletion)>;

    InstalledPackageWorkerLauncher(SandboxTrustBoundary boundary,
                                   QString workerExecutable,
                                   QString sandboxTempRoot,
                                   QUrl apiOrigin,
                                   BindingValidator validateBinding,
                                   AdmissionCallback requestAdmission,
                                   AttachCallback attach,
                                   StopCallback stop,
                                   ExitCallback exited,
                                   FailureCallback failed,
                                   QObject *parent = nullptr);
    ~InstalledPackageWorkerLauncher() override;

    InstalledPackageWorkerLauncher(const InstalledPackageWorkerLauncher &) = delete;
    InstalledPackageWorkerLauncher &operator=(
        const InstalledPackageWorkerLauncher &) = delete;

    [[nodiscard]] bool requestLaunch(const WorkerLaunchRequest &request);
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
    struct ObserverStartGate;
    struct ReadyPayload;

    void requestAdmission(quint64 serial, std::shared_ptr<ReadyPayload> payload);
    void completeLaunch(quint64 serial,
                        std::shared_ptr<ReadyPayload> payload,
                        AdmissionResult admission);
    [[nodiscard]] std::shared_ptr<ObserverStartGate> observeProcess(
        std::shared_ptr<LaunchRetirementContext> context,
        SandboxProcessWaitHandle waitHandle);
    void handleRetirement(
        std::shared_ptr<LaunchRetirementContext> context,
        bool succeeded,
        const QString &stableError);
    void fail(WorkerAttemptKey key,
              const QString &stableError,
              quint32 nativeError = 0);

    SandboxTrustBoundary boundary_;
    QString workerExecutable_;
    QString sandboxTempRoot_;
    QUrl apiOrigin_;
    BindingValidator validateBinding_;
    AdmissionCallback requestAdmission_;
    AttachCallback attach_;
    StopCallback stop_;
    ExitCallback exited_;
    FailureCallback failed_;
    std::shared_ptr<SandboxProcess> currentProcess_;
    std::shared_ptr<LaunchRetirementContext> currentRetirement_;
    std::optional<WorkerAttemptKey> currentKey_;
    std::optional<WorkerAttemptKey> expectedStop_;
    std::optional<WorkerLaunchRequest> pendingRequest_;
    std::unordered_map<quint64, std::shared_ptr<LaunchRetirementContext>> inflight_;
    std::vector<std::shared_ptr<LaunchRetirementContext>> fatalCleanup_;
    quint64 serial_ = 0;
    bool accepting_ = true;
};
