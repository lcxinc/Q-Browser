#pragma once

#include "IpcSession.h"
#include "BrowserSessionStore.h"
#include "HostRuntimeConfig.h"
#include "InstalledPackageWorkerLauncher.h"
#include "WorkerSupervisor.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <atomic>
#include <QPointer>

class MainWindow;
class AppTabRuntimeController;
class AppRuntimeCoordinator;
struct AppRuntimeResult;
class VerifiedCurrentPackage;
class HostWorkerSessionController;
class HostCapabilityRuntime;
class FileDialogCoordinator;
class HostGestureRouter;
class RuntimePackageAuthority;
class WorkerSurface;
class UpdateLifecycleCoordinator;
class QEvent;
class QTimer;
class QThread;

#ifdef Q_BROWSER_HOST_TESTING
struct HostWorkerAttachContext final
{
    std::unique_ptr<IpcSession> session;
    std::unique_ptr<WorkerSurface> surface;
    std::shared_ptr<void> processLifetime;
    WorkerLaunchRequest launchRequest;
    quint32 processId = 0;
    std::function<void()> stopProcess;
};
#endif

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
#ifdef Q_BROWSER_HOST_TESTING
    [[nodiscard]] InstalledPackageWorkerLauncher::AttachResult
        attachWorkerContextForTesting(HostWorkerAttachContext context);
#endif
    void detachWorkerContext(const QString &reason);
    [[nodiscard]] bool hasWorkerContext() const noexcept;
    [[nodiscard]] MainWindow *mainWindow() const noexcept;
    [[nodiscard]] HostWorkerSessionController *workerSessionController() const noexcept;
#ifdef Q_BROWSER_HOST_TESTING
    void forceLifecycleQueueFullForTesting(bool full) noexcept;
    void forceLifecycleShutdownQueueFailureForTesting() noexcept;
    [[nodiscard]] bool retryWorkerCleanupForTesting();
    [[nodiscard]] HostGestureRouter *gestureRouterForTesting() const noexcept;
    void holdNextPackageStartupReplyForTesting();
    [[nodiscard]] quint64 supersedePackageStartupForTesting(
        const QString &packagePath);
    [[nodiscard]] bool deliverHeldPackageStartupReplyForTesting(
        quint64 operationIncarnation);
    void setRestoredRoutePackageIdForTesting(QString packageId);
    void setAfterPrepareAppLaunchHookForTesting(
        std::function<void()> hook);
#endif

signals:
    void packageActivationVerified(quint64 operationIncarnation,
                                   const QString &appId,
                                   const QString &version,
                                   const QByteArray &digestHex);
    void updateLifecycleFailed(const QString &stableError,
                               quint32 nativeError = 0);
    void packageWorkerReady(const QString &appId,
                            const QString &version,
                            const QString &packageDirectory,
                            quint64 activation,
                            quint64 attempt,
                            quint32 processId);
    void packageWorkerExited(quint64 activation, quint64 attempt);
    void packageWorkerReadyForTab(const QString &tabId,
                                  quint64 runtimeIncarnation,
                                  const QString &appId,
                                  const QString &version,
                                  const QString &packageDirectory,
                                  quint64 activation,
                                  quint64 attempt,
                                  quint32 processId,
                                  quint64 leaseAuthorityEpoch);
    void packageWorkerExitedForTab(const QString &tabId,
                                   quint64 runtimeIncarnation,
                                   quint64 activation,
                                   quint64 attempt,
                                   quint64 leaseAuthorityEpoch,
                                   bool expected);
    void routeLoadAcknowledgedForTab(const QString &tabId,
                                     quint64 runtimeIncarnation,
                                     quint64 activation,
                                     quint64 attempt,
                                     quint64 leaseAuthorityEpoch,
                                     quint64 sessionGeneration,
                                     const QString &route);
    void workerCapabilityRequestObserved(const QString &capability,
                                         const QString &operation,
                                         const QVariantMap &payload);
    void workerCapabilityRequestObservedForTab(const QString &tabId,
                                               quint64 runtimeIncarnation,
                                               quint64 sessionGeneration,
                                               const QString &capability,
                                               const QString &operation,
                                               const QVariantMap &payload,
                                               quint64 leaseAuthorityEpoch);
#ifdef Q_BROWSER_HOST_TESTING
    void appLaunchIntentQueuedForTesting(const QString &tabId, int intent);
    void packageStartupReplyHeldForTesting(quint64 operationIncarnation);
#endif

private:
    struct WorkerAttachContext;
#ifdef Q_BROWSER_HOST_TESTING
    struct PackageStartupTestingState;
#endif
    enum class PackageOperationKind
    {
        StartupInstall,
        StartupOffline,
        Install,
        Offline,
    };
    enum class PackageStartupPhase
    {
        NotStarted,
        Verifying,
        Applied,
        Failed,
    };

    bool eventFilter(QObject *watched, QEvent *event) override;
    [[nodiscard]] bool requestPackageInstall(
        const QString &packagePath,
        std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority);
    [[nodiscard]] bool enqueueLifecycle(
        std::function<void(UpdateLifecycleCoordinator &)> operation);
    [[nodiscard]] bool enqueueAppRuntime(
        std::function<void(AppRuntimeCoordinator &)> operation);
    [[nodiscard]] bool enqueuePackageRuntime(
        std::function<void(AppRuntimeCoordinator &)> operation);
    [[nodiscard]] bool initializePackageRuntime();
    [[nodiscard]] bool beginPackageStartup();
    [[nodiscard]] bool enqueuePackageStartupInitialization(
        PackageOperationKind kind,
        quint64 operationIncarnation,
        QString packagePath,
        std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority);
    [[nodiscard]] bool enqueuePackageOperation(
        PackageOperationKind kind,
        quint64 operationIncarnation,
        QString packagePath = {},
        std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority = {});
    [[nodiscard]] quint64 issuePackageOperationIncarnation() noexcept;
    void handlePackageOperationResult(PackageOperationKind kind,
                                      quint64 operationIncarnation,
                                      const AppRuntimeResult &result,
                                      std::shared_ptr<RuntimePackageAuthority>
                                          replyAuthority);
    [[nodiscard]] bool applyVerifiedPackageStartup(
        const VerifiedCurrentPackage &verified);
    [[nodiscard]] AppTabRuntimeController *ensureAppTabRuntimeController(
        const QString &tabId);
    void handleAppLaunchRequested(const QString &tabId,
                                  quint64 navigationIncarnation,
                                  const QString &packageId,
                                  const QString &route,
                                  bool reload = false);
    void handleAppStopRequested(const QString &tabId,
                                quint64 navigationIncarnation,
                                quint64 runtimeIncarnation);
    void handleAppRuntimeResult(const AppRuntimeResult &result);
    void handleAppRuntimeFailure(const QString &stableError,
                                 quint32 nativeError = 0);
    void failClosedAppRuntime(const QString &stableError,
                              quint32 nativeError = 0);
    void clearPendingAppHeartbeat(const QString &key) noexcept;
    [[nodiscard]] quint64 runtimeIncarnationForTab(const QString &tabId);
    [[nodiscard]] quint64 advanceRuntimeIncarnationForTab(const QString &tabId);
    [[nodiscard]] std::optional<qint64> takePendingAppHeartbeat(
        const QString &key) noexcept;
    [[nodiscard]] InstalledPackageWorkerLauncher::AttachResult
        attachWorkerContext(
            InstalledPackageWorkerLauncher::CommittedAttachTransaction transaction);
    [[nodiscard]] InstalledPackageWorkerLauncher::AttachResult
        realizeWorkerContext(WorkerAttachContext context);
    [[nodiscard]] bool attachInitializedWorkerContext(
        WorkerAttachContext context);
    void completePendingWorkerContext();
    void clearPendingWorkerContext() noexcept;
    void synchronizeGestureAuthority();

    std::optional<HostRuntimeConfig> runtimeConfig_;
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::shared_ptr<BrowserSessionStore> browserSessionStore_;
    std::optional<BrowserSessionLoadResult> pendingBrowserSessionLoad_;
    std::unique_ptr<HostGestureRouter> gestureRouter_;
    std::unique_ptr<FileDialogCoordinator> fileDialogCoordinator_;
    std::unique_ptr<HostWorkerSessionController> workerSessionController_;
    std::shared_ptr<HostCapabilityRuntime> capabilityRuntime_;
    std::unique_ptr<WorkerAttachContext> pendingWorkerAttach_;
    std::shared_ptr<void> workerProcessLifetime_;
    std::function<void()> stopWorkerProcess_;
    std::unique_ptr<InstalledPackageWorkerLauncher> installedPackageLauncher_;
    std::shared_ptr<RuntimePackageAuthority> packageAuthority_;
    QString verifiedCurrentAppId_;
    QString verifiedCurrentVersion_;
    QByteArray verifiedCurrentDigestHex_;
    QSet<quint64> pendingPackageOperations_;
    PackageStartupPhase packageStartupPhase_ = PackageStartupPhase::NotStarted;
    quint64 startupIncarnation_ = 0;
    quint64 nextPackageOperationIncarnation_ = 1;
    QHash<QString, quint64> appRuntimeIncarnations_;
    QHash<QString, quint64> pendingAppNavigationIncarnations_;
    QHash<QString, QPointer<AppTabRuntimeController>> appTabRuntimeControllers_;
    std::unique_ptr<QTimer> updateHealthTimer_;
    std::optional<WorkerAttemptKey> attachedWorkerKey_;
    QPointer<QObject> updateLifecycleRuntime_;
    QThread *updateLifecycleThread_ = nullptr;
    quint64 nextCapabilityRuntimeIncarnation_ = 1;
    std::atomic_bool acceptingLifecycle_{true};
    std::atomic_bool appRuntimeFailedClosed_{false};
    std::shared_ptr<std::atomic_bool> appRuntimeTransportGate_ =
        std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<std::mutex> appRuntimeTransportMutex_ =
        std::make_shared<std::mutex>();
    mutable std::mutex appHeartbeatMutex_;
    QHash<QString, qint64> pendingAppHeartbeatTimes_;
    std::atomic_bool appHealthCheckPending_{false};
#ifdef Q_BROWSER_HOST_TESTING
    bool lifecycleQueueFullForTesting_ = false;
    bool forceLifecycleShutdownQueueFailureForTesting_ = false;
    std::unique_ptr<PackageStartupTestingState> packageStartupTesting_;
    std::function<void()> afterPrepareAppLaunchHookForTesting_;
#endif
};
