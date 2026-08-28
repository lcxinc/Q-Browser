#include "MainWindow.h"

#include "AppTabRuntimeController.h"
#include "BrowserAddress.h"
#include "BrowserChrome.h"
#include "NavigationBar.h"
#include "WebSessionProfile.h"
#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QCoreApplication>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <utility>

namespace {

QVariantMap variantParameters(const QHash<QString, QString> &parameters)
{
    QVariantMap result;
    for (auto iterator = parameters.constBegin();
         iterator != parameters.constEnd(); ++iterator) {
        result.insert(iterator.key(), iterator.value());
    }
    return result;
}

} // namespace

MainWindow::MainWindow(RouteRegistry routeRegistry,
                       const QUrl &mockOrigin,
                       WorkerSurface *workerSurface,
                       QWidget *parent)
    : QMainWindow(parent)
    , routes_(std::move(routeRegistry))
    , mockOrigin_(mockOrigin)
    , webSessionProfile_(std::make_unique<WebSessionProfile>(mockOrigin_))
    , tabModel_(std::make_unique<BrowserTabModel>())
{
    setObjectName(QStringLiteral("qbrowser-main-window"));
    setWindowTitle(QStringLiteral("Q-Browser"));

    auto *const central = new QWidget(this);
    auto *const layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    browserChrome_ = new BrowserChrome(central);
    surfaceStack_ = new QStackedWidget(central);
    surfaceStack_->setObjectName(QStringLiteral("surface-stack"));
    layout->addWidget(browserChrome_);
    layout->addWidget(surfaceStack_, 1);
    setCentralWidget(central);

    connect(tabModel_.get(), &BrowserTabModel::tabInserted, this,
            [this](int, const QString &id) {
                createController(id);
                synchronizeChrome();
            });
    connect(tabModel_.get(), &BrowserTabModel::tabRemoved, this,
            [this](int, const QString &id) {
                removeController(id);
                synchronizeChrome();
            });
    connect(tabModel_.get(), &BrowserTabModel::tabMoved, this,
            [this](int, int) { synchronizeChrome(); });
    connect(tabModel_.get(), &BrowserTabModel::tabChanged, this,
            [this](int) { synchronizeChrome(); });
    connect(tabModel_.get(), &BrowserTabModel::activeTabChanged, this,
            [this](int, int) {
                const QString stableId = activeStableId();
                const QPointer<MainWindow> guard(this);
                QMetaObject::invokeMethod(
                    this,
                    [guard, stableId] {
                        if (guard && guard->isRunning()
                            && guard->activeStableId() == stableId) {
                            guard->activateStableTab(stableId);
                            guard->synchronizeChrome();
                        }
                    },
                    Qt::QueuedConnection);
                synchronizeChrome();
            });

    connect(browserChrome_, &BrowserChrome::addressSubmitted, this,
            [this](const QString &address) {
                const QString stableId = activeStableId();
                if (stableId.isEmpty()
                    || !navigateTab(stableId, address)) {
                    synchronizeChrome();
                }
            });
    connect(browserChrome_, &BrowserChrome::commandRequested, this,
            &MainWindow::handleCommand);
    connect(browserChrome_, &BrowserChrome::tabActivationRequested, this,
            [this](const QString &stableId) {
                if (!isRunning() || tabMutationInProgress()) {
                    synchronizeChrome();
                    return;
                }
                QScopedValueRollback<bool> transaction(
                    navigationInProgress_, true);
                const int index = tabModel_->indexOfId(stableId);
                if (index < 0 || !tabModel_->activateTab(index)) return;
                activateStableTab(stableId);
                synchronizeChrome();
            });
    connect(browserChrome_, &BrowserChrome::tabMoveRequested, this,
            [this](const QString &stableId, int, const int to) {
                moveStableTab(stableId, to);
            });
    connect(browserChrome_, &BrowserChrome::tabCloseRequested, this,
            [this](const QString &stableId) { closeStableTab(stableId); });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { (void)shutdown(); });

    const QString initialId = tabModel_->createTab(
        BrowserTabKind::Host, QStringLiteral("New tab"),
        QStringLiteral("qbrowser://newtab"));
    if (!initialId.isEmpty()) activateStableTab(initialId);
    synchronizeChrome();
    if (workerSurface != nullptr) {
        (void)attachWorkerSurface(
            std::unique_ptr<WorkerSurface>(workerSurface));
    }
}

MainWindow::~MainWindow()
{
    (void)shutdown();
}

bool MainWindow::shutdown()
{
    if (lifecycleState_ == LifecycleState::Complete) return true;
    if (shutdownInProgress_ || tabMutationInProgress()) return false;
    QScopedValueRollback<bool> shutdownTransaction(shutdownInProgress_, true);
    if (lifecycleState_ == LifecycleState::Running) {
        lifecycleState_ = LifecycleState::Closing;
        hide();
        const QList<TabController *> retiringControllers = controllers_.values();
        for (TabController *const controller : retiringControllers) {
            if (controller != nullptr) {
                emit tabClosing(controller->tabId(), controller->incarnation());
                trackRetiringController(controller);
                (void)controller->beginClosing();
            }
        }
        TabController *const legacyController =
            tabController(legacyWorkerOwnerId_);
        if (legacyController != nullptr
            && legacyController->workerSurface() != nullptr) {
            emit legacyWorkerRetirementRequested(legacyWorkerOwnerId_);
        }
        controllers_.clear();
        visibleTabId_.clear();
        legacyWorkerOwnerId_.clear();
        for (TabController *const controller : retiringControllers) {
            if (controller == nullptr) continue;
            if (retiringControllers_.contains(controller->tabId())) continue;
            (void)controller->retire();
            delete controller;
        }
    }
    if (webSessionProfile_ != nullptr) {
        const bool profileShutdown = webSessionProfile_->shutdown();
        if (!profileShutdown) return false;
        webSessionProfile_.reset();
    }
    lifecycleState_ = LifecycleState::Complete;
    return true;
}

bool MainWindow::navigate(const QStringView input)
{
    return navigateTab(activeStableId(), input);
}

void MainWindow::setPackageRuntimeEnabled(const bool enabled) noexcept
{
    packageRuntimeEnabled_ = enabled;
}

bool MainWindow::navigateFromWorker(const QString &packageId,
                                    const QString &route)
{
    return navigateFromWorker(activeStableId(), packageId, route);
}

bool MainWindow::navigateFromWorker(const QString &stableId,
                                    const QString &packageId,
                                    const QString &route)
{
    TabController *const controller = tabController(stableId);
    if (!isRunning() || controller == nullptr
        || controller->surfaceKind() != HostSurfaceKind::Worker
        || packageId.isEmpty() || packageId != controller->workerPackageId()
        || !route.startsWith(u'/')
        || route.startsWith(QStringLiteral("//"))) {
        return false;
    }
    const QString candidate = QStringLiteral("app://pilot") + route;
    const BrowserAddress parsed = BrowserAddress::parse(candidate);
    if (!parsed.isValid() || parsed.kind() != BrowserAddressKind::App) {
        return false;
    }
    const RouteMatch match = routes_.match(parsed.appPath());
    if (!match.isValid() || match.record.engine != Engine::QmlWorker
        || match.record.packageId != packageId) {
        return false;
    }
    if (controller->appRuntimeController() != nullptr
        && controller->appRuntimeController()->hasWorkerContext()) {
        // A worker-initiated navigation is already authenticated by this
        // session.  Commit only the owning tab's history/address; the session
        // controller will enqueue the single RouteLoad response.  Calling
        // navigateTab here would re-enter package launch and replace a
        // healthy worker for an ordinary in-app route change.
        const int index = tabModel_->indexOfId(stableId);
        if (index < 0) return false;
        const BrowserTabSnapshot before = tabModel_->snapshotAt(index);
        bool committed = false;
        {
            const QSignalBlocker blockModelSignals(tabModel_.get());
            committed = tabModel_->navigateTab(
                stableId, BrowserTabKind::App, parsed.canonical());
            if (committed) {
                const quint64 incarnation = controller->beginNavigation();
                if (incarnation == 0) return false;
            }
        }
        if (!committed) return false;
        const BrowserTabSnapshot after = tabModel_->snapshotAt(index);
        if (before != after) publishCommittedTabChange(stableId);
        if (activeStableId() == stableId && before.address != after.address) {
            emit currentUrlChanged(after.address);
        }
        if (before != after) emitPersistenceAfterTransition();
        emit appLaunchRequested(stableId, controller->incarnation(), packageId,
                                parsed.appPath());
        return true;
    }
    return navigateTab(stableId, candidate);
}

bool MainWindow::goBack()
{
    return traverseHistory(activeStableId(), false);
}

bool MainWindow::goForward()
{
    return traverseHistory(activeStableId(), true);
}

bool MainWindow::attachWorkerSurface(std::unique_ptr<WorkerSurface> surface)
{
    const QString stableId = activeStableId();
    if (stableId.isEmpty()) return false;
    if (!legacyWorkerOwnerId_.isEmpty() && legacyWorkerOwnerId_ != stableId) {
        return false;
    }
    if (!attachWorkerSurface(stableId, std::move(surface))) return false;
    legacyWorkerOwnerId_ = stableId;
    return true;
}

bool MainWindow::reserveLegacyWorkerOwner(const QString &stableId)
{
    if (!isRunning() || stableId.isEmpty()
        || tabController(stableId) == nullptr) {
        return false;
    }
    if (legacyWorkerOwnerId_.isEmpty()) {
        legacyWorkerOwnerId_ = stableId;
        return true;
    }
    return legacyWorkerOwnerId_ == stableId;
}

bool MainWindow::attachWorkerSurface(const QString &stableId,
                                     std::unique_ptr<WorkerSurface> surface)
{
    TabController *const controller = tabController(stableId);
    if (!isRunning() || tabMutationInProgress() || surface == nullptr
        || controller == nullptr
        || (legacyWorkerOwnerId_ == stableId
            && controller->workerSurface() != nullptr)
        || stableId.isEmpty()) {
        return false;
    }
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    if (!controller->attachLegacyWorkerSurface(std::move(surface))) return false;
    const int index = tabModel_->indexOfId(stableId);
    if (!packageRuntimeEnabled_ && index >= 0
        && tabModel_->snapshotAt(index).kind == BrowserTabKind::App) {
        (void)startCurrentDescriptor(stableId);
    }
    return true;
}

void MainWindow::detachWorkerSurface()
{
    if (legacyWorkerOwnerId_.isEmpty()) return;
    const QString stableId = legacyWorkerOwnerId_;
    detachWorkerSurface(stableId);
    legacyWorkerOwnerId_.clear();
}

void MainWindow::detachWorkerSurface(const QString &stableId)
{
    if (stableId.isEmpty()) return;
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    TabController *const controller = tabController(stableId);
    if (controller != nullptr) controller->detachLegacyWorkerSurface();
}

bool MainWindow::isRunning() const noexcept
{
    return lifecycleState_ == LifecycleState::Running;
}

bool MainWindow::isShutdownComplete() const noexcept
{
    return lifecycleState_ == LifecycleState::Complete;
}

bool MainWindow::hasValidWebSession() const noexcept
{
    return isRunning() && webSessionProfile_ != nullptr
        && webSessionProfile_->isConfigurationValid();
}

HostSurfaceKind MainWindow::activeSurface() const noexcept
{
    TabController *const controller = tabController(activeStableId());
    return controller != nullptr ? controller->surfaceKind()
                                 : HostSurfaceKind::TrustedError;
}

int MainWindow::activeSurfaceCount() const
{
    if (surfaceStack_ == nullptr) return 0;
    int visible = 0;
    for (int index = 0; index < surfaceStack_->count(); ++index) {
        QWidget *const surface = surfaceStack_->widget(index);
        if (surface != nullptr && !surface->isHidden()
            && surface->isVisibleTo(surfaceStack_)) {
            ++visible;
        }
    }
    return visible;
}

QString MainWindow::currentAppUrl() const
{
    return activeSnapshot().address;
}

int MainWindow::historyCount() const noexcept
{
    return static_cast<int>(activeSnapshot().history.size());
}

int MainWindow::historyIndex() const noexcept
{
    return activeSnapshot().historyIndex;
}

QString MainWindow::trustedErrorText() const
{
    TabController *const controller = tabController(activeStableId());
    return controller != nullptr ? controller->trustedErrorText() : QString{};
}

NavigationBar *MainWindow::navigationBar() const noexcept
{
    return browserChrome_ != nullptr ? browserChrome_->navigationBar() : nullptr;
}

BrowserChrome *MainWindow::browserChrome() const noexcept
{
    return browserChrome_;
}

BrowserTabModel *MainWindow::tabModel() const noexcept
{
    return tabModel_.get();
}

TabController *MainWindow::tabController(const QString &tabId) const noexcept
{
    return controllers_.value(tabId, nullptr);
}

QStackedWidget *MainWindow::surfaceStack() const noexcept
{
    return surfaceStack_;
}

WebSessionProfile *MainWindow::webSessionProfile() const noexcept
{
    return webSessionProfile_.get();
}

WebSurface *MainWindow::webSurface() const noexcept
{
    TabController *const controller = tabController(activeStableId());
    return controller != nullptr ? controller->webSurface() : nullptr;
}

WorkerSurface *MainWindow::workerSurface() const noexcept
{
    return workerSurface(activeStableId());
}

WorkerSurface *MainWindow::workerSurface(const QString &stableId) const noexcept
{
    TabController *const controller = tabController(stableId);
    return controller != nullptr ? controller->workerSurface() : nullptr;
}

std::optional<MainWindow::ResolvedNavigation> MainWindow::resolveAddress(
    const QStringView input,
    QString *plainError) const
{
    const auto fail = [plainError](const QString &message)
        -> std::optional<ResolvedNavigation> {
        if (plainError != nullptr) *plainError = message;
        return std::nullopt;
    };
    const BrowserAddress parsed = BrowserAddress::parse(input);
    if (!parsed.isValid()) {
        return fail(QStringLiteral(
            "The address is not a valid Q-Browser route."));
    }
    if (parsed.kind() == BrowserAddressKind::NewTab) {
        ResolvedNavigation resolved;
        resolved.canonicalAddress = parsed.canonical();
        resolved.kind = BrowserTabKind::Host;
        return resolved;
    }
    if (parsed.kind() != BrowserAddressKind::App) {
        return fail(QStringLiteral(
            "The address is not a valid Q-Browser route."));
    }

    const RouteMatch match = routes_.match(parsed.appPath());
    if (!match.isValid()) return fail(QStringLiteral("Route not found."));
    ResolvedNavigation resolved;
    resolved.canonicalAddress = parsed.canonical();
    resolved.appRoute = parsed.appPath();
    resolved.engine = match.record.engine;
    resolved.packageId = match.record.packageId;
    resolved.entryPoint = match.record.entryPoint;
    resolved.parameters = variantParameters(match.parameters);
    switch (match.record.engine) {
    case Engine::QmlWorker:
        resolved.kind = BrowserTabKind::App;
        return resolved;
    case Engine::WebEngine:
        resolved.kind = BrowserTabKind::Web;
        resolved.physicalEntry = QUrl(match.record.entryPoint, QUrl::StrictMode);
        if (!resolved.physicalEntry.isValid()) {
            return fail(QStringLiteral("The web content is unavailable."));
        }
        return resolved;
    case Engine::TrustedQml:
        return fail(QStringLiteral("The trusted page is unavailable."));
    case Engine::Invalid:
        return fail(QStringLiteral("Route not found."));
    }
    return fail(QStringLiteral("Route not found."));
}

bool MainWindow::navigateTab(const QString &stableTabId,
                             const QStringView input)
{
    TabController *const controller = tabController(stableTabId);
    if (!isRunning() || tabMutationInProgress() || stableTabId.isEmpty()
        || controller == nullptr) {
        return false;
    }
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    QString error;
    const std::optional<ResolvedNavigation> resolved = resolveAddress(input, &error);
    if (!resolved.has_value()) {
        showTrustedError(stableTabId, error);
        synchronizeChrome();
        return false;
    }

    const int index = tabModel_->indexOfId(stableTabId);
    if (index < 0) return false;
    const BrowserTabSnapshot before = tabModel_->snapshotAt(index);
    bool committed = false;
    quint64 navigationIncarnation = 0;
    {
        const QSignalBlocker blockModelSignals(tabModel_.get());
        committed = tabModel_->navigateTab(
            stableTabId, resolved->kind, resolved->canonicalAddress);
        if (committed) {
            if (resolved->kind == BrowserTabKind::Host) {
                const bool titleCommitted = tabModel_->setTitle(
                    stableTabId, QStringLiteral("New tab"));
                Q_ASSERT(titleCommitted);
            }
            navigationIncarnation = controller->beginNavigation();
        }
    }
    if (!committed) return false;
    Q_ASSERT(navigationIncarnation != 0);
    if (navigationIncarnation == 0) return false;
    const int committedIndex = tabModel_->indexOfId(stableTabId);
    if (committedIndex < 0) return false;
    const BrowserTabSnapshot after = tabModel_->snapshotAt(committedIndex);
    const bool modelChanged = before != after;
    if (modelChanged) publishCommittedTabChange(stableTabId);

    const bool started = startResolved(
        stableTabId, *resolved, navigationIncarnation);
    if (activeStableId() == stableTabId && before.address != after.address) {
        emit currentUrlChanged(after.address);
    }
    if (modelChanged) emitPersistenceAfterTransition();
    return started;
}

bool MainWindow::traverseHistory(const QString &stableTabId,
                                 const bool forward)
{
    if (!isRunning() || tabMutationInProgress() || stableTabId.isEmpty()) {
        return false;
    }
    const int index = tabModel_->indexOfId(stableTabId);
    if (index < 0) return false;
    const BrowserTabSnapshot before = tabModel_->snapshotAt(index);
    const int targetIndex = before.historyIndex + (forward ? 1 : -1);
    if (targetIndex < 0 || targetIndex >= before.history.size()) return false;

    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    QString error;
    const std::optional<ResolvedNavigation> resolved = resolveAddress(
        before.history.at(targetIndex), &error);
    if (!resolved.has_value()) {
        showTrustedError(stableTabId, error);
        synchronizeChrome();
        return false;
    }

    bool committed = false;
    quint64 navigationIncarnation = 0;
    TabController *const controller = tabController(stableTabId);
    if (controller == nullptr) return false;
    {
        const QSignalBlocker blockModelSignals(tabModel_.get());
        committed = forward
            ? tabModel_->goForward(stableTabId, resolved->kind)
            : tabModel_->goBack(stableTabId, resolved->kind);
        if (committed) {
            if (resolved->kind == BrowserTabKind::Host) {
                const bool titleCommitted = tabModel_->setTitle(
                    stableTabId, QStringLiteral("New tab"));
                Q_ASSERT(titleCommitted);
            }
            navigationIncarnation = controller->beginNavigation();
        }
    }
    if (!committed) return false;
    Q_ASSERT(navigationIncarnation != 0);
    if (navigationIncarnation == 0) return false;
    publishCommittedTabChange(stableTabId);
    (void)startResolved(
        stableTabId, *resolved, navigationIncarnation);
    if (activeStableId() == stableTabId) {
        emit currentUrlChanged(resolved->canonicalAddress);
    }
    emitPersistenceAfterTransition();
    return true;
}

bool MainWindow::startResolved(const QString &stableTabId,
                               const ResolvedNavigation &resolved,
                               const quint64 navigationIncarnation,
                               const bool reloadExisting)
{
    TabController *const controller = tabController(stableTabId);
    if (controller == nullptr) return false;
    if (resolved.kind != BrowserTabKind::App
        && controller->appRuntimeController() != nullptr) {
        (void)controller->appRuntimeController()->sendVisibilityChanged(false);
    }
    controller->setActive(activeStableId() == stableTabId);
    switch (resolved.kind) {
    case BrowserTabKind::Host:
        return controller->startHost(navigationIncarnation);
    case BrowserTabKind::Web:
        return controller->startWeb(
            resolved.physicalEntry, navigationIncarnation, reloadExisting);
    case BrowserTabKind::App:
        if (packageRuntimeEnabled_ && legacyWorkerOwnerId_ != stableTabId) {
            if (!reloadExisting && controller->appRuntimeController() != nullptr
                && controller->appRuntimeController()->hasWorkerContext()
                && controller->workerPackageId() == resolved.packageId) {
                if (!controller->bindAppWorkerSurface(
                        resolved.packageId, navigationIncarnation)) {
                    controller->showTrustedErrorForNavigation(
                        navigationIncarnation,
                        QStringLiteral("The package worker is unavailable."));
                    return false;
                }
                (void)tabModel_->setLoadState(stableTabId, true, 0);
                if (!controller->appRuntimeController()->requestRouteLoad(
                        resolved.appRoute)) {
                    controller->showTrustedErrorForNavigation(
                        navigationIncarnation,
                        QStringLiteral("The package worker is unavailable."));
                    return false;
                }
                emit appLaunchRequested(stableTabId, navigationIncarnation,
                                        resolved.packageId, resolved.appRoute);
                return true;
            }
            // Package-mode tabs are launched through the shared app runtime
            // coordinator.  Keep the tab in a loading state while the
            // per-tab controller obtains and authenticates its worker.
            if (!controller->prepareAppLaunch(
                    resolved.packageId, navigationIncarnation)) {
                return false;
            }
            if (reloadExisting) {
                emit appReloadRequested(stableTabId, navigationIncarnation,
                                        resolved.packageId, resolved.appRoute);
            } else {
                emit appLaunchRequested(stableTabId, navigationIncarnation,
                                        resolved.packageId, resolved.appRoute);
            }
            return true;
        }
        if (controller->workerSurface() == nullptr
            && legacyWorkerOwnerId_ != stableTabId) {
            controller->showTrustedErrorForNavigation(
                navigationIncarnation,
                QStringLiteral("The package worker is unavailable."));
            return false;
        }
        return controller->startLegacyApp(
            resolved.packageId, resolved.entryPoint, resolved.parameters,
            QUrl(resolved.canonicalAddress, QUrl::StrictMode),
            navigationIncarnation);
    case BrowserTabKind::TrustedError:
        break;
    }
    controller->showTrustedErrorForNavigation(
        navigationIncarnation, QStringLiteral("Route not found."));
    return false;
}

bool MainWindow::startCurrentDescriptor(const QString &stableTabId,
                                        const bool reloadExisting)
{
    const int index = tabModel_->indexOfId(stableTabId);
    if (index < 0) return false;
    QString error;
    const std::optional<ResolvedNavigation> resolved = resolveAddress(
        tabModel_->snapshotAt(index).address, &error);
    if (!resolved.has_value()) {
        showTrustedError(stableTabId, error);
        return false;
    }
    TabController *const controller = tabController(stableTabId);
    if (controller == nullptr) return false;
    const quint64 navigationIncarnation = controller->beginNavigation();
    if (navigationIncarnation == 0) return false;
    return startResolved(
        stableTabId, *resolved, navigationIncarnation, reloadExisting);
}

void MainWindow::showTrustedError(const QString &stableTabId,
                                  const QString &message,
                                  const bool retireWebSurface)
{
    TabController *const controller = tabController(stableTabId);
    if (controller == nullptr) return;
    controller->setActive(activeStableId() == stableTabId);
    controller->showTrustedError(
        message.isEmpty() ? QStringLiteral("The content is unavailable.") : message,
        retireWebSurface);
}

void MainWindow::publishCommittedTabChange(const QString &stableTabId)
{
    const int index = tabModel_->indexOfId(stableTabId);
    if (index < 0) return;
    const bool published = QMetaObject::invokeMethod(
        tabModel_.get(), "tabChanged", Qt::DirectConnection,
        Q_ARG(int, index));
    Q_ASSERT(published);
}

void MainWindow::emitPersistenceAfterTransition()
{
    const bool published = QMetaObject::invokeMethod(
        tabModel_.get(), "persistenceNeeded", Qt::DirectConnection);
    Q_ASSERT(published);
}

void MainWindow::createController(const QString &stableTabId)
{
    if (!isRunning() || stableTabId.isEmpty()
        || controllers_.contains(stableTabId)
        || tabModel_->indexOfId(stableTabId) < 0) {
        return;
    }
    auto *const controller = new TabController(
        stableTabId, tabModel_.get(), surfaceStack_,
        webSessionProfile_.get(), this);
    controllers_.insert(stableTabId, controller);
    connect(controller, &TabController::resourceMutationStarted, this,
            [this] { ++resourceMutationDepth_; }, Qt::DirectConnection);
    connect(controller, &TabController::resourceMutationFinished, this,
            [this] {
                Q_ASSERT(resourceMutationDepth_ > 0);
                if (resourceMutationDepth_ > 0) --resourceMutationDepth_;
            }, Qt::DirectConnection);
    connect(controller, &TabController::addressActivated, this,
            [this](const QString &tabId, const quint64 incarnation,
                   const QString &address) {
                TabController *const current = tabController(tabId);
                if (current == nullptr || current->incarnation() != incarnation
                    || activeStableId() != tabId) {
                    return;
                }
                (void)navigateTab(tabId, address);
            });
    connect(controller, &TabController::workerRouteRequested, this,
            [this, stableTabId, controller](
                const QString &packageId, const QString &entryPoint,
                const QVariantMap &parameters, const QUrl &appUrl) {
                if (tabController(stableTabId) != controller
                    || legacyWorkerOwnerId_ != stableTabId) {
                    return;
                }
                emit workerRouteRequested(
                    packageId, entryPoint, parameters, appUrl);
            });
    connect(controller, &TabController::workerRouteRequestedForTab, this,
            [this, stableTabId, controller](const QString &tabId,
                                             const quint64 incarnation,
                                             const QString &packageId,
                                             const QString &entryPoint,
                                             const QVariantMap &parameters,
                                             const QUrl &appUrl) {
                if (tabId != stableTabId || tabController(tabId) != controller) {
                    return;
                }
                emit workerRouteRequestedForTab(tabId, incarnation, packageId,
                                                entryPoint, parameters, appUrl);
            });
}

void MainWindow::removeController(const QString &stableTabId)
{
    TabController *const controller = controllers_.take(stableTabId);
    if (controller == nullptr) return;
    trackRetiringController(controller);
    (void)controller->beginClosing();
    if (retiringControllers_.contains(stableTabId)) return;
    // A controller without a package runtime has no asynchronous retirement
    // barrier and can be destroyed as soon as its model entry is removed.
    if (legacyWorkerOwnerId_ == stableTabId
        && controller->workerSurface() != nullptr) {
        emit legacyWorkerRetirementRequested(stableTabId);
    }
    if (legacyWorkerOwnerId_ == stableTabId) legacyWorkerOwnerId_.clear();
    if (visibleTabId_ == stableTabId) visibleTabId_.clear();
    (void)controller->retire();
    delete controller;
}

void MainWindow::trackRetiringController(TabController *const controller)
{
    if (controller == nullptr || retiringControllers_.contains(controller->tabId())) {
        return;
    }
    AppTabRuntimeController *const runtime = controller->appRuntimeController();
    if (runtime == nullptr) return;
    retiringControllers_.insert(controller->tabId(), controller);
    QPointer<MainWindow> guard(this);
    connect(runtime, &AppTabRuntimeController::retired, this,
            [guard](const QString &tabId, const quint64) {
                if (guard) guard->finalizeRetiringController(tabId);
            },
            Qt::QueuedConnection);
}

void MainWindow::finalizeRetiringController(const QString &stableTabId)
{
    TabController *const controller = retiringControllers_.take(stableTabId);
    if (controller == nullptr) return;
    (void)controller->retire();
    delete controller;
}

void MainWindow::activateStableTab(const QString &stableTabId)
{
    if (!isRunning() || stableTabId.isEmpty()
        || activeStableId() != stableTabId) {
        return;
    }
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    if (!visibleTabId_.isEmpty() && visibleTabId_ != stableTabId) {
        TabController *const oldController = tabController(visibleTabId_);
        if (oldController != nullptr) oldController->setActive(false);
    }
    visibleTabId_ = stableTabId;
    TabController *const controller = tabController(stableTabId);
    if (controller == nullptr) return;
    controller->setActive(true);
    if (controller->lifecycle() == BrowserTabLifecycle::Dormant) {
        (void)startCurrentDescriptor(stableTabId);
    }
}

void MainWindow::closeStableTab(const QString &stableTabId)
{
    if (!isRunning() || tabMutationInProgress()) return;
    const int index = tabModel_->indexOfId(stableTabId);
    TabController *const controller = tabController(stableTabId);
    if (index < 0 || controller == nullptr) return;
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    emit tabClosing(stableTabId, controller->incarnation());
    const bool retiresLegacyWorker = legacyWorkerOwnerId_ == stableTabId
        && controller->workerSurface() != nullptr;
    if (retiresLegacyWorker) {
        (void)controller->beginClosing();
        emit legacyWorkerRetirementRequested(stableTabId);
        (void)controller->retire();
    } else if (controller->appRuntimeController() != nullptr) {
        // Keep the controller alive until its launcher/retirement barrier
        // completes. The model entry may disappear now, but destroying the
        // runtime tuple here would turn close into an unbounded use-after-
        // retirement race for late worker callbacks.
        trackRetiringController(controller);
        (void)controller->beginClosing();
    } else {
        (void)controller->retire();
    }

    if (tabModel_->count() == 1) {
        bool closed = false;
        QString replacementId;
        {
            const QSignalBlocker blockModelSignals(tabModel_.get());
            closed = tabModel_->closeTab(index);
            if (closed) {
                replacementId = tabModel_->createTab(
                    BrowserTabKind::Host, QStringLiteral("New tab"),
                    QStringLiteral("qbrowser://newtab"));
            }
        }
        if (!closed) return;
        if (!replacementId.isEmpty()) createController(replacementId);
        const bool removedPublished = QMetaObject::invokeMethod(
            tabModel_.get(), "tabRemoved", Qt::DirectConnection,
            Q_ARG(int, index), Q_ARG(QString, stableTabId));
        Q_ASSERT(removedPublished);
        if (replacementId.isEmpty()) {
            const bool activePublished = QMetaObject::invokeMethod(
                tabModel_.get(), "activeTabChanged", Qt::DirectConnection,
                Q_ARG(int, index), Q_ARG(int, -1));
            Q_ASSERT(activePublished);
            emitPersistenceAfterTransition();
            synchronizeChrome();
            return;
        }
        const int replacementIndex = tabModel_->indexOfId(replacementId);
        const bool insertedPublished = QMetaObject::invokeMethod(
            tabModel_.get(), "tabInserted", Qt::DirectConnection,
            Q_ARG(int, replacementIndex), Q_ARG(QString, replacementId));
        const bool activePublished = QMetaObject::invokeMethod(
            tabModel_.get(), "activeTabChanged", Qt::DirectConnection,
            Q_ARG(int, index), Q_ARG(int, replacementIndex));
        Q_ASSERT(insertedPublished);
        Q_ASSERT(activePublished);
        activateStableTab(replacementId);
        emitPersistenceAfterTransition();
        synchronizeChrome();
        return;
    }

    if (!tabModel_->closeTab(index)) return;
    if (tabModel_->isEmpty()) {
        const QString replacementId = tabModel_->createTab(
            BrowserTabKind::Host, QStringLiteral("New tab"),
            QStringLiteral("qbrowser://newtab"));
        if (!replacementId.isEmpty()) activateStableTab(replacementId);
    } else {
        activateStableTab(activeStableId());
    }
    synchronizeChrome();
}

void MainWindow::moveStableTab(const QString &stableTabId,
                               const int destinationIndex)
{
    if (!isRunning()) return;
    if (tabMutationInProgress()) {
        synchronizeChrome();
        return;
    }
    const int sourceIndex = tabModel_->indexOfId(stableTabId);
    if (sourceIndex < 0 || destinationIndex < 0
        || destinationIndex >= tabModel_->count()) {
        synchronizeChrome();
        return;
    }
    QScopedValueRollback<bool> transaction(navigationInProgress_, true);
    if (tabModel_->moveTab(sourceIndex, destinationIndex)) {
        activateStableTab(activeStableId());
        synchronizeChrome();
    } else {
        synchronizeChrome();
    }
}

void MainWindow::handleCommand(const BrowserCommand command)
{
    if (!isRunning() || tabMutationInProgress()) return;
    const QString stableId = activeStableId();
    switch (command) {
    case BrowserCommand::NewTab: {
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        const QString createdId = tabModel_->createTab(
            BrowserTabKind::Host, QStringLiteral("New tab"),
            QStringLiteral("qbrowser://newtab"));
        if (!createdId.isEmpty()) activateStableTab(createdId);
        synchronizeChrome();
        return;
    }
    case BrowserCommand::CloseTab:
        closeStableTab(stableId);
        return;
    case BrowserCommand::ReopenClosedTab: {
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        const QString reopenedId = tabModel_->reopenMostRecentlyClosed();
        if (!reopenedId.isEmpty()) activateStableTab(reopenedId);
        synchronizeChrome();
        return;
    }
    case BrowserCommand::NextTab:
    case BrowserCommand::PreviousTab: {
        if (tabModel_->count() < 2) return;
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        const int offset = command == BrowserCommand::NextTab ? 1 : -1;
        const int target = (tabModel_->activeIndex() + offset
                            + tabModel_->count())
            % tabModel_->count();
        if (tabModel_->activateTab(target)) {
            activateStableTab(tabModel_->activeId());
            synchronizeChrome();
        }
        return;
    }
    case BrowserCommand::SelectLastTab:
    {
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        if (!tabModel_->isEmpty()
            && tabModel_->activateTab(tabModel_->count() - 1)) {
            activateStableTab(tabModel_->activeId());
            synchronizeChrome();
        }
        return;
    }
    case BrowserCommand::FocusAddress:
        return;
    case BrowserCommand::Back:
        (void)traverseHistory(stableId, false);
        return;
    case BrowserCommand::Forward:
        (void)traverseHistory(stableId, true);
        return;
    case BrowserCommand::Reload:
        if (!tabMutationInProgress()) {
            QScopedValueRollback<bool> transaction(navigationInProgress_, true);
            (void)startCurrentDescriptor(stableId, true);
        }
        return;
    case BrowserCommand::Stop: {
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        TabController *const controller = tabController(stableId);
        if (controller != nullptr) {
            const AppTabRuntimeController *const runtime =
                controller->appRuntimeController();
            const bool loadingApp = runtime != nullptr
                && (controller->lifecycle() == BrowserTabLifecycle::Starting
                    || controller->lifecycle() == BrowserTabLifecycle::Loading);
            const quint64 runtimeIncarnation = loadingApp
                ? runtime->runtimeIncarnation() : 0;
            const quint64 navigationIncarnation = controller->incarnation();
            if (controller->cancelAppLaunch()) {
                emit appStopRequested(stableId, navigationIncarnation,
                                      runtimeIncarnation);
            } else {
                controller->stop();
            }
        }
        return;
    }
    case BrowserCommand::Home:
        (void)navigateTab(stableId, QStringLiteral("qbrowser://newtab"));
        return;
    case BrowserCommand::SelectTab1:
    case BrowserCommand::SelectTab2:
    case BrowserCommand::SelectTab3:
    case BrowserCommand::SelectTab4:
    case BrowserCommand::SelectTab5:
    case BrowserCommand::SelectTab6:
    case BrowserCommand::SelectTab7:
    case BrowserCommand::SelectTab8: {
        const int target = numberedTabIndex(command);
        QScopedValueRollback<bool> transaction(navigationInProgress_, true);
        if (target >= 0 && target < tabModel_->count()
            && tabModel_->activateTab(target)) {
            activateStableTab(tabModel_->activeId());
            synchronizeChrome();
        }
        return;
    }
    }
}

void MainWindow::synchronizeChrome()
{
    if (isRunning() && browserChrome_ != nullptr && tabModel_ != nullptr) {
        (void)browserChrome_->synchronizeTabs(*tabModel_);
    }
}

QString MainWindow::activeStableId() const
{
    return tabModel_ != nullptr ? tabModel_->activeId() : QString{};
}

BrowserTabSnapshot MainWindow::activeSnapshot() const
{
    if (tabModel_ == nullptr) return {};
    const int index = tabModel_->activeIndex();
    return index >= 0 && index < tabModel_->count()
        ? tabModel_->snapshotAt(index) : BrowserTabSnapshot{};
}

bool MainWindow::tabMutationInProgress() const noexcept
{
    return navigationInProgress_ || resourceMutationDepth_ > 0;
}

int MainWindow::numberedTabIndex(const BrowserCommand command) noexcept
{
    switch (command) {
    case BrowserCommand::SelectTab1:
        return 0;
    case BrowserCommand::SelectTab2:
        return 1;
    case BrowserCommand::SelectTab3:
        return 2;
    case BrowserCommand::SelectTab4:
        return 3;
    case BrowserCommand::SelectTab5:
        return 4;
    case BrowserCommand::SelectTab6:
        return 5;
    case BrowserCommand::SelectTab7:
        return 6;
    case BrowserCommand::SelectTab8:
        return 7;
    default:
        return -1;
    }
}
