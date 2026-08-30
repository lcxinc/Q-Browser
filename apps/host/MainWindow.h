#pragma once

#include "BrowserSessionStore.h"
#include "BrowserTabModel.h"
#include "BrowserCommand.h"
#include "RouteRegistry.h"
#include "TabController.h"

#include <QHash>
#include <QMainWindow>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <functional>
#include <optional>

class BrowserChrome;
class NavigationBar;
class QStackedWidget;
class QTimer;
class WebSessionProfile;
class WebSurface;
class WorkerSurface;

enum class MainWindowInitialState
{
    ImmediateNewTab,
    DeferredSession,
};

using BrowserSessionSaveCallback =
    std::function<BrowserSessionSaveResult(const BrowserWindowSnapshot &)>;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RouteRegistry routeRegistry,
               const QUrl &mockOrigin,
               WorkerSurface *workerSurface = nullptr,
               QWidget *parent = nullptr);
    MainWindow(RouteRegistry routeRegistry,
               const QUrl &mockOrigin,
               MainWindowInitialState initialState,
               QWidget *parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] bool navigate(QStringView input);
    [[nodiscard]] bool applyBrowserSessionLoadResult(
        const BrowserSessionLoadResult &loadResult,
        const RestoredAddressResolver &resolver,
        const QRect &primaryAvailableGeometry,
        const QList<QRect> &availableScreenGeometries,
        BrowserSessionSaveCallback saveCallback);
    [[nodiscard]] bool freezeBrowserSessionAndFlush();
    void setPackageRuntimeEnabled(bool enabled) noexcept;
    [[nodiscard]] bool shutdown();
    [[nodiscard]] bool navigateFromWorker(const QString &packageId,
                                          const QString &route);
    [[nodiscard]] bool navigateFromWorker(const QString &tabId,
                                          const QString &packageId,
                                          const QString &route);
    [[nodiscard]] bool goBack();
    [[nodiscard]] bool goForward();
    [[nodiscard]] bool attachWorkerSurface(std::unique_ptr<WorkerSurface> surface);
    [[nodiscard]] bool attachWorkerSurface(const QString &tabId,
                                           std::unique_ptr<WorkerSurface> surface);
    [[nodiscard]] bool reserveLegacyWorkerOwner(const QString &tabId);
    void detachWorkerSurface();
    void detachWorkerSurface(const QString &tabId);
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool isShutdownComplete() const noexcept;
    [[nodiscard]] bool hasValidWebSession() const noexcept;

    [[nodiscard]] HostSurfaceKind activeSurface() const noexcept;
    [[nodiscard]] int activeSurfaceCount() const;
    [[nodiscard]] QString currentAppUrl() const;
    [[nodiscard]] int historyCount() const noexcept;
    [[nodiscard]] int historyIndex() const noexcept;
    [[nodiscard]] QString trustedErrorText() const;
    [[nodiscard]] NavigationBar *navigationBar() const noexcept;
    [[nodiscard]] BrowserChrome *browserChrome() const noexcept;
    [[nodiscard]] BrowserTabModel *tabModel() const noexcept;
    [[nodiscard]] TabController *tabController(const QString &tabId) const noexcept;
    [[nodiscard]] QStackedWidget *surfaceStack() const noexcept;
    [[nodiscard]] WebSessionProfile *webSessionProfile() const noexcept;
    [[nodiscard]] WebSurface *webSurface() const noexcept;
    [[nodiscard]] WorkerSurface *workerSurface() const noexcept;
    [[nodiscard]] WorkerSurface *workerSurface(const QString &tabId) const noexcept;

signals:
    void currentUrlChanged(const QString &url);
    void workerRouteRequested(const QString &packageId,
                              const QString &entryPoint,
                              const QVariantMap &parameters,
                              const QUrl &appUrl);
    void workerRouteRequestedForTab(const QString &tabId,
                                    quint64 incarnation,
                                    const QString &packageId,
                                    const QString &entryPoint,
                                    const QVariantMap &parameters,
                                    const QUrl &appUrl);
    void tabClosing(const QString &tabId, quint64 incarnation);
    void appLaunchRequested(const QString &tabId,
                            quint64 navigationIncarnation,
                            const QString &packageId,
                            const QString &route);
    void appReloadRequested(const QString &tabId,
                            quint64 navigationIncarnation,
                            const QString &packageId,
                            const QString &route);
    void appStopRequested(const QString &tabId,
                          quint64 navigationIncarnation,
                          quint64 runtimeIncarnation);
    void legacyWorkerRetirementRequested(const QString &tabId);

private:
    MainWindow(RouteRegistry routeRegistry,
               const QUrl &mockOrigin,
               WorkerSurface *workerSurface,
               QWidget *parent,
               MainWindowInitialState initialState);

    enum class LifecycleState
    {
        Running,
        Closing,
        Complete,
    };

    struct ResolvedNavigation final
    {
        QString canonicalAddress;
        BrowserTabKind kind = BrowserTabKind::Host;
        Engine engine = Engine::Invalid;
        QString packageId;
        QString entryPoint;
        QString appRoute;
        QVariantMap parameters;
        QUrl physicalEntry;
    };

    [[nodiscard]] std::optional<ResolvedNavigation> resolveAddress(
        QStringView input,
        QString *plainError = nullptr) const;
    [[nodiscard]] bool navigateTab(const QString &stableTabId,
                                   QStringView input);
    [[nodiscard]] bool traverseHistory(const QString &stableTabId,
                                       bool forward);
    [[nodiscard]] bool startResolved(const QString &stableTabId,
                                     const ResolvedNavigation &resolved,
                                     quint64 navigationIncarnation,
                                     bool reloadExisting = false);
    [[nodiscard]] bool startCurrentDescriptor(const QString &stableTabId,
                                              bool reloadExisting = false);
    void showTrustedError(const QString &stableTabId,
                          const QString &message,
                          bool retireWebSurface = false);
    void publishCommittedTabChange(const QString &stableTabId);
    void emitPersistenceAfterTransition();
    [[nodiscard]] BrowserWindowSnapshot browserSessionSnapshot() const;
    [[nodiscard]] bool performBrowserSessionSave(
        const BrowserWindowSnapshot &snapshot) noexcept;

    void createController(const QString &stableTabId);
    void removeController(const QString &stableTabId);
    void trackRetiringController(TabController *controller);
    void finalizeRetiringController(const QString &stableTabId);
    void activateStableTab(const QString &stableTabId);
    void closeStableTab(const QString &stableTabId);
    void moveStableTab(const QString &stableTabId, int destinationIndex);
    void handleCommand(BrowserCommand command);
    void synchronizeChrome();

    [[nodiscard]] QString activeStableId() const;
    [[nodiscard]] BrowserTabSnapshot activeSnapshot() const;
    [[nodiscard]] bool tabMutationInProgress() const noexcept;
    [[nodiscard]] static int numberedTabIndex(BrowserCommand command) noexcept;

    RouteRegistry routes_;
    QUrl mockOrigin_;
    std::unique_ptr<WebSessionProfile> webSessionProfile_;
    std::unique_ptr<BrowserTabModel> tabModel_;
    BrowserChrome *browserChrome_ = nullptr;
    QStackedWidget *surfaceStack_ = nullptr;
    QHash<QString, TabController *> controllers_;
    QHash<QString, TabController *> retiringControllers_;
    QString visibleTabId_;
    QString legacyWorkerOwnerId_;
    bool navigationInProgress_ = false;
    int resourceMutationDepth_ = 0;
    bool shutdownInProgress_ = false;
    bool packageRuntimeEnabled_ = false;
    BrowserSessionSaveCallback browserSessionSave_;
    QTimer *browserSessionSaveTimer_ = nullptr;
    std::optional<BrowserWindowSnapshot> frozenBrowserSessionSnapshot_;
    bool browserSessionApplied_ = false;
    bool browserSessionApplyInProgress_ = false;
    bool browserSessionFrozen_ = false;
    bool browserSessionFinalSaveSucceeded_ = true;
    bool browserSessionSaveInProgress_ = false;
    LifecycleState lifecycleState_ = LifecycleState::Running;
};
