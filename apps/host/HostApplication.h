#pragma once

#include "IpcSession.h"
#include "WorkerSupervisor.h"

#include <QObject>
#include <QUrl>

#include <memory>
#include <functional>
#include <optional>
#include <atomic>

class MainWindow;
class HostWorkerSessionController;
class WorkerSurface;
class UpdateLifecycleCoordinator;
class QTimer;
class QThread;

struct HostWorkerAttachContext final
{
    std::unique_ptr<IpcSession> session;
    WorkerSurface *surface = nullptr;
    std::shared_ptr<void> processLifetime;
    std::function<void()> stopProcess;
    std::optional<WorkerAttemptKey> supervisionKey;
};

class HostApplication final : public QObject
{
    Q_OBJECT

public:
    explicit HostApplication(QUrl mockOrigin, QObject *parent = nullptr);
    ~HostApplication() override;

    [[nodiscard]] bool start();
    [[nodiscard]] bool setUpdateLifecycleCoordinator(
        std::unique_ptr<UpdateLifecycleCoordinator> coordinator);
    [[nodiscard]] bool requestPackageInstall(const QString &packagePath);
    [[nodiscard]] bool requestOfflineStart();
    [[nodiscard]] bool attachWorkerSession(std::unique_ptr<IpcSession> session);
    [[nodiscard]] bool attachWorkerContext(HostWorkerAttachContext context);
    void detachWorkerContext(const QString &reason);
    [[nodiscard]] bool hasWorkerContext() const noexcept;
    [[nodiscard]] MainWindow *mainWindow() const noexcept;
    [[nodiscard]] HostWorkerSessionController *workerSessionController() const noexcept;

signals:
    void updateLifecycleFailed(const QString &stableError);

private:
    [[nodiscard]] bool enqueueLifecycle(
        std::function<void(UpdateLifecycleCoordinator &)> operation);

    static constexpr qsizetype maximumPendingLifecycleOperations = 512;
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<HostWorkerSessionController> workerSessionController_;
    std::shared_ptr<void> workerProcessLifetime_;
    std::function<void()> stopWorkerProcess_;
    std::unique_ptr<UpdateLifecycleCoordinator> updateLifecycleCoordinator_;
    std::unique_ptr<QTimer> updateHealthTimer_;
    std::optional<WorkerAttemptKey> attachedWorkerKey_;
    std::unique_ptr<QThread> updateLifecycleThread_;
    QObject *updateLifecycleDispatch_ = nullptr;
    std::atomic<qsizetype> pendingLifecycleOperations_{0};
};
