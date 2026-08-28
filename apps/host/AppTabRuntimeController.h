#pragma once

#include "InstalledPackageWorkerLauncher.h"
#include "SandboxTrustBoundary.h"
#include "WorkerLaunchRequest.h"

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class HostCapabilityRuntime;
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

    [[nodiscard]] bool requestLaunch(const WorkerLaunchRequest &request);
    [[nodiscard]] bool requestRouteLoad(const QString &route);
    [[nodiscard]] bool sendVisibilityChanged(bool active);
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
    void retired(const QString &tabId, quint64 runtimeIncarnation);

private:
    struct PendingAttach final
    {
        WorkerLaunchRequest request;
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
    void clearPendingAttach() noexcept;
    void stopCurrent(const QString &reason);
    void rememberRequest(const WorkerLaunchRequest &request);
    [[nodiscard]] std::optional<WorkerLaunchRequest> requestForAttempt(
        const WorkerAttemptKey &key) const;

    QString tabId_;
    QPointer<TabController> tabController_;
    QPointer<MainWindow> mainWindow_;
    QPointer<HostGestureRouter> gestureRouter_;
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
    QHash<QString, WorkerLaunchRequest> requestHistory_;
    quint64 runtimeIncarnation_ = 0;
    quint64 activeGeneration_ = 0;
    bool accepting_ = true;
    bool closing_ = false;
    bool retiredEmitted_ = false;
};
