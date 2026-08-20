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

    InstalledPackageWorkerLauncher(SandboxTrustBoundary boundary,
                                   QString workerExecutable,
                                   QString sandboxTempRoot,
                                   QUrl apiOrigin,
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

signals:
    void ready(const QString &appId,
               const QString &version,
               const QString &packageDirectory,
               quint64 activation,
               quint64 attempt,
               quint32 processId);
    void unexpectedExit(quint64 activation, quint64 attempt);

private:
    struct ReadyPayload;

    void completeLaunch(quint64 serial, std::shared_ptr<ReadyPayload> payload);
    void observeProcess(WorkerAttemptKey key,
                        std::shared_ptr<SandboxProcess> process,
                        std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree,
                        QString tempDirectory,
                        quint64 serial);
    void fail(WorkerAttemptKey key, const QString &stableError);

    SandboxTrustBoundary boundary_;
    QString workerExecutable_;
    QString sandboxTempRoot_;
    QUrl apiOrigin_;
    AttachCallback attach_;
    StopCallback stop_;
    ExitCallback exited_;
    FailureCallback failed_;
    std::shared_ptr<SandboxProcess> currentProcess_;
    std::optional<WorkerAttemptKey> currentKey_;
    std::optional<WorkerAttemptKey> expectedStop_;
    std::optional<UpdateLaunchRequest> pendingRequest_;
    quint64 serial_ = 0;
    bool accepting_ = true;
};
