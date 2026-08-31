#pragma once

#include "InstalledPackageWorkerLauncher.h"
#include "SandboxTrustBoundary.h"
#include "WorkerLaunchRequest.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class HostCapabilityRuntime;
class FileDialogCoordinator;
class HostGestureRouter;
class HostWorkerSessionController;
class MainWindow;
class RuntimePackageAuthority;
class TabController;

// Owns the complete runtime tuple for one browser App tab. The shared
// package authority/coordinator decides *what* may launch; this object owns
// *where* that launch is attached and keeps all session/capability events
// scoped to one tab.
class AppTabRuntimeController final : public QObject
{
    Q_OBJECT

public:
    using AdmissionCallback = InstalledPackageWorkerLauncher::AdmissionCallback;

    AppTabRuntimeController(QString tabId,
                            TabController *tabController,
                            MainWindow *mainWindow,
                            HostGestureRouter *gestureRouter,
                            FileDialogCoordinator *fileDialogCoordinator,
                            std::shared_ptr<RuntimePackageAuthority> authority,
                            SandboxApprovedRoots roots,
                            QString workerExecutable,
                            QString sandboxTempRoot,
                            QUrl apiOrigin,
                            QString storageDirectory,
                            quintptr hostWindowId,
                            AdmissionCallback admission,
                            std::shared_ptr<std::atomic_bool> transportGate,
                            std::shared_ptr<std::mutex> transportGateMutex,
                            QObject *parent = nullptr);
    ~AppTabRuntimeController() override;

    AppTabRuntimeController(const AppTabRuntimeController &) = delete;
    AppTabRuntimeController &operator=(const AppTabRuntimeController &) = delete;

    [[nodiscard]] QString tabId() const;
    [[nodiscard]] bool isAccepting() const noexcept;
    [[nodiscard]] bool hasWorkerContext() const noexcept;
    [[nodiscard]] quint64 runtimeIncarnation() const noexcept;
    [[nodiscard]] const std::optional<WorkerLaunchRequest> &currentRequest()
        const noexcept;
    [[nodiscard]] HostWorkerSessionController *sessionController() const noexcept;
    [[nodiscard]] InstalledPackageWorkerLauncher *launcher() const noexcept;
    [[nodiscard]] HostCapabilityRuntime *capabilityRuntime() const noexcept;

    [[nodiscard]] bool requestLaunch(
        const WorkerLaunchRequest &request,
        quint64 expectedNavigationIncarnation,
        const QString &originalCanonicalAddress);
    [[nodiscard]] bool retargetPendingLaunch(
        quint64 runtimeIncarnation,
        quint64 expectedNavigationIncarnation,
        const QString &route,
        const QString &originalCanonicalAddress);
    [[nodiscard]] bool cancelLaunchIfCurrent(
        const WorkerLaunchRequest &request,
        const QString &reason = QStringLiteral("host.worker.launch_stale"));
    [[nodiscard]] bool requestRouteLoad(const QString &route);
    [[nodiscard]] bool sendVisibilityChanged(bool active);
#ifdef Q_BROWSER_HOST_TESTING
    void deferNextAttachForTesting() noexcept;
    [[nodiscard]] bool hasPendingAttachForTesting() const noexcept;
    [[nodiscard]] bool resumePendingAttachForTesting();
    void setAfterPendingSessionAttachHookForTesting(
        std::function<void()> hook);
#endif
    void failClosed(const QString &reason =
                        QStringLiteral("host.runtime.critical_event_dropped"));
    void stop(const QString &reason = QStringLiteral("host.worker.stop"));
    void close(const QString &reason = QStringLiteral("host.worker.close"));

signals:
    void ready(const WorkerLaunchRequest &request, quint32 processId);
    void workerExited(const WorkerLaunchRequest &request, bool expected);
    void workerFailed(const WorkerLaunchRequest &request,
                      const QString &stableError,
                      quint32 nativeError);
    void heartbeatObserved(const WorkerLaunchRequest &request,
                           quint64 sessionGeneration);
    void routeLoadAcknowledged(const WorkerLaunchRequest &request,
                               const QString &route,
                               quint64 sessionGeneration);
    void pageMetadataChanged(const WorkerLaunchRequest &request,
                             const QString &title,
                             const QString &status,
                             quint64 sessionGeneration);
    void capabilityRequestObserved(const WorkerLaunchRequest &request,
                                   const QString &capability,
                                   const QString &operation,
                                   const QVariantMap &payload,
                                   quint64 sessionGeneration);
    void failed(const WorkerLaunchRequest &request,
                const QString &errorCode,
                quint64 sessionGeneration);
    void launcherTerminalFailure(const QString &tabId,
                                 const QString &stableError,
                                 quint32 nativeError);
    void retired(const QString &tabId, quint64 runtimeIncarnation);

private:
    struct ExpectedLaunchTarget final
    {
        WorkerLaunchRequest request;
        quint64 navigationIncarnation = 0;
        QString canonicalAddress;
    };

    struct PendingAttach final
    {
        WorkerLaunchRequest request;
        quint64 expectedNavigationIncarnation = 0;
        QString originalCanonicalAddress;
        std::unique_ptr<IpcSession> session;
        std::unique_ptr<WorkerSurface> surface;
        std::shared_ptr<SandboxProcess> process;
        std::shared_ptr<HostCapabilityRuntime> capability;
        quint64 expectedGeneration = 0;
    };

    [[nodiscard]] InstalledPackageWorkerLauncher::AttachResult
        realizeAttach(InstalledPackageWorkerLauncher::CommittedAttachTransaction
                          transaction);
    void completePendingAttach(quint64 detachedGeneration);
    [[nodiscard]] bool completeAttach(PendingAttach pending,
                                      quint64 expectedGeneration);
    [[nodiscard]] bool isExpectedLaunchTarget(
        const WorkerLaunchRequest &request,
        quint64 expectedNavigationIncarnation,
        const QString &originalCanonicalAddress) const;
    void clearPendingAttach() noexcept;
    void stopCurrent(const QString &reason);

    QString tabId_;
    QPointer<TabController> tabController_;
    QPointer<MainWindow> mainWindow_;
    QPointer<HostGestureRouter> gestureRouter_;
    FileDialogCoordinator *fileDialogCoordinator_ = nullptr;
    std::shared_ptr<RuntimePackageAuthority> authority_;
    QUrl mockOrigin_;
    QString storageDirectory_;
    quintptr hostWindowId_ = 0;
    std::unique_ptr<HostWorkerSessionController> sessionController_;
    std::shared_ptr<HostCapabilityRuntime> capabilityRuntime_;
    std::shared_ptr<SandboxProcess> processLifetime_;
    std::function<void()> stopProcess_;
    std::unique_ptr<InstalledPackageWorkerLauncher> launcher_;
    std::shared_ptr<std::atomic_bool> transportGate_;
    std::shared_ptr<std::mutex> transportGateMutex_;
    std::optional<PendingAttach> pendingAttach_;
    std::optional<WorkerLaunchRequest> currentRequest_;
    std::optional<ExpectedLaunchTarget> expectedLaunchTarget_;
    quint64 runtimeIncarnation_ = 0;
    quint64 activeGeneration_ = 0;
    bool accepting_ = true;
    bool closing_ = false;
    bool retiredEmitted_ = false;
#ifdef Q_BROWSER_HOST_TESTING
    bool deferNextAttachForTesting_ = false;
    bool holdPendingAttachForTesting_ = false;
    std::function<void()> afterPendingSessionAttachHookForTesting_;
#endif
};
