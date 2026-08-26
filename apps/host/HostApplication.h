#pragma once

#include "IpcSession.h"
#include "HostRuntimeConfig.h"
#include "InstalledPackageWorkerLauncher.h"
#include "WorkerSupervisor.h"

#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <functional>
#include <optional>
#include <atomic>
#include <QPointer>

class MainWindow;
class HostWorkerSessionController;
class HostCapabilityRuntime;
class HostGestureRouter;
class WorkerSurface;
class UpdateLifecycleCoordinator;
class QEvent;
class QTimer;
class QThread;

struct HostWorkerAttachContext final
{
    std::unique_ptr<IpcSession> session;
    std::unique_ptr<WorkerSurface> surface;
    std::shared_ptr<void> processLifetime;
    ManifestPermissions permissions;
    quint32 processId = 0;
    std::function<void()> stopProcess;
    std::optional<WorkerAttemptKey> supervisionKey;
};

class HostApplication final : public QObject
{
    Q_OBJECT

public:
    explicit HostApplication(QUrl mockOrigin, QObject *parent = nullptr);
    explicit HostApplication(HostRuntimeConfig runtimeConfig,
                             QObject *parent = nullptr);
    ~HostApplication() override;

    [[nodiscard]] bool start();
    [[nodiscard]] bool requestPackageInstall(const QString &packagePath);
    [[nodiscard]] bool requestOfflineStart();
    [[nodiscard]] bool attachWorkerSession(std::unique_ptr<IpcSession> session);
    [[nodiscard]] InstalledPackageWorkerLauncher::AttachResult attachWorkerContext(
        HostWorkerAttachContext context);
    void detachWorkerContext(const QString &reason);
    [[nodiscard]] bool hasWorkerContext() const noexcept;
    [[nodiscard]] MainWindow *mainWindow() const noexcept;
    [[nodiscard]] HostWorkerSessionController *workerSessionController() const noexcept;
#ifdef Q_BROWSER_HOST_TESTING
    void forceLifecycleQueueFullForTesting(bool full) noexcept;
    [[nodiscard]] bool retryWorkerCleanupForTesting();
    [[nodiscard]] HostGestureRouter *gestureRouterForTesting() const noexcept;
#endif

signals:
    void updateLifecycleFailed(const QString &stableError);
    void packageWorkerReady(const QString &appId,
                            const QString &version,
                            const QString &packageDirectory,
                            quint64 activation,
                            quint64 attempt,
                            quint32 processId);
    void packageWorkerExited(quint64 activation, quint64 attempt);
    void workerCapabilityRequestObserved(const QString &capability,
                                         const QString &operation,
                                         const QVariantMap &payload);

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    [[nodiscard]] bool requestPackageInstall(
        const QString &packagePath,
        std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority);
    [[nodiscard]] bool enqueueLifecycle(
        std::function<void(UpdateLifecycleCoordinator &)> operation);
    [[nodiscard]] bool initializePackageRuntime();
    void synchronizeGestureAuthority();

    std::optional<HostRuntimeConfig> runtimeConfig_;
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<HostGestureRouter> gestureRouter_;
    std::unique_ptr<HostWorkerSessionController> workerSessionController_;
    std::shared_ptr<HostCapabilityRuntime> capabilityRuntime_;
    std::shared_ptr<void> workerProcessLifetime_;
    std::function<void()> stopWorkerProcess_;
    std::unique_ptr<InstalledPackageWorkerLauncher> installedPackageLauncher_;
    std::unique_ptr<QTimer> updateHealthTimer_;
    std::optional<WorkerAttemptKey> attachedWorkerKey_;
    QPointer<QObject> updateLifecycleRuntime_;
    QThread *updateLifecycleThread_ = nullptr;
    quint64 nextCapabilityRuntimeIncarnation_ = 1;
    quint64 nextLeaseAuthorityEpoch_ = 1;
    std::atomic_bool acceptingLifecycle_{true};
#ifdef Q_BROWSER_HOST_TESTING
    bool lifecycleQueueFullForTesting_ = false;
#endif
};
