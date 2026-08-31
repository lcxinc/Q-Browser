#include "AppTabRuntimeController.h"

#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "HostWorkerSessionController.h"
#include "MainWindow.h"
#include "RuntimePackageAuthority.h"
#include "TabController.h"

#include <QMetaObject>
#include <QTimer>

#include <limits>
#include <utility>

AppTabRuntimeController::AppTabRuntimeController(
    QString tabId,
    TabController *const tabController,
    MainWindow *const mainWindow,
    HostGestureRouter *const gestureRouter,
    FileDialogCoordinator *const fileDialogCoordinator,
    std::shared_ptr<RuntimePackageAuthority> authority,
    SandboxApprovedRoots roots,
    QString workerExecutable,
    QString sandboxTempRoot,
    QUrl apiOrigin,
    QString storageDirectory,
    const quintptr hostWindowId,
    AdmissionCallback admission,
    std::shared_ptr<std::atomic_bool> transportGate,
    std::shared_ptr<std::mutex> transportGateMutex,
    QObject *const parent)
    : QObject(parent)
    , tabId_(std::move(tabId))
    , tabController_(tabController)
    , mainWindow_(mainWindow)
    , gestureRouter_(gestureRouter)
    , fileDialogCoordinator_(fileDialogCoordinator)
    , authority_(std::move(authority))
    , mockOrigin_(std::move(apiOrigin))
    , storageDirectory_(std::move(storageDirectory))
    , hostWindowId_(hostWindowId)
    , transportGate_(std::move(transportGate))
    , transportGateMutex_(std::move(transportGateMutex))
{
    Q_ASSERT(!tabId_.isEmpty());
    Q_ASSERT(tabController_ != nullptr);
    Q_ASSERT(mainWindow_ != nullptr);

    sessionController_ = std::make_unique<HostWorkerSessionController>(
        [window = QPointer<MainWindow>(mainWindow_), tabId = tabId_](
            const QString &appId, const QString &route) {
            return !window.isNull()
                && window->navigateFromWorker(tabId, appId, route);
        },
        this);

    connect(sessionController_.get(),
            &HostWorkerSessionController::sessionDetached, this,
            [this](const quint64 generation) {
                completePendingAttach(generation);
            },
            Qt::DirectConnection);

    connect(tabController_, &TabController::activeChanged, this,
            [this](const QString &tabId, const bool active) {
                if (tabId != tabId_) return;
                if (sessionController_ != nullptr
                    && sessionController_->state()
                           == HostWorkerSessionState::Running) {
                    (void)sessionController_->sendVisibilityChanged(
                        active && tabController_ != nullptr
                        && tabController_->surfaceKind() == HostSurfaceKind::Worker);
                }
            },
            Qt::DirectConnection);
    connect(sessionController_.get(),
            &HostWorkerSessionController::heartbeatObserved, this,
            [this](const quint64 generation) {
                if (generation != activeGeneration_ || !currentRequest_.has_value()) {
                    return;
                }
                emit heartbeatObserved(*currentRequest_, generation);
            },
            Qt::DirectConnection);
    connect(sessionController_.get(),
            &HostWorkerSessionController::routeLoadAcknowledged, this,
            [this](const QString &route, const quint64 generation) {
                if (generation != activeGeneration_ || !currentRequest_.has_value()) {
                    return;
                }
                emit routeLoadAcknowledged(*currentRequest_, route, generation);
            },
            Qt::DirectConnection);
    connect(sessionController_.get(),
            &HostWorkerSessionController::pageMetadataChanged, this,
            [this](const quint64 generation, const QString &title,
                   const QString &status) {
                if (generation != activeGeneration_ || !currentRequest_.has_value()) {
                    return;
                }
                emit pageMetadataChanged(*currentRequest_, title, status,
                                         generation);
            },
            Qt::DirectConnection);
    connect(sessionController_.get(),
            &HostWorkerSessionController::capabilityRequestObserved, this,
            [this](const QString &capability, const QString &operation,
                   const QVariantMap &payload, const quint64 generation) {
                if (generation != activeGeneration_ || !currentRequest_.has_value()) {
                    return;
                }
                emit capabilityRequestObserved(*currentRequest_, capability,
                                               operation, payload, generation);
            },
            Qt::DirectConnection);
    connect(sessionController_.get(), &HostWorkerSessionController::failed, this,
            [this](const QString &errorCode, const quint64 generation) {
                const bool currentGeneration = generation == activeGeneration_;
                const bool pendingGeneration =
                    pendingAttach_.has_value()
                    && generation == pendingAttach_->expectedGeneration;
                if ((!currentGeneration && !pendingGeneration)
                    || !currentRequest_.has_value()) {
                    return;
                }
                const WorkerLaunchRequest failedRequest = *currentRequest_;
                const QPointer<AppTabRuntimeController> self(this);
                emit failed(failedRequest, errorCode, generation);
                if (!self || !self->currentRequest_.has_value()
                    || !hasSameWorkerLaunchAuthority(
                           *self->currentRequest_, failedRequest)
                    || self->sessionController_ == nullptr
                    || self->sessionController_->generation() != generation
                    || self->sessionController_->state()
                           != HostWorkerSessionState::Failed
                    || (generation != self->activeGeneration_
                        && (!self->pendingAttach_.has_value()
                            || generation
                                   != self->pendingAttach_
                                          ->expectedGeneration))) {
                    return;
                }
                self->stopCurrent(
                    QStringLiteral("host.worker_session.failed"));
            },
            Qt::DirectConnection);

    auto boundary = SandboxTrustBoundary::create(roots);
    if (!boundary.value.has_value() || authority_ == nullptr || admission == nullptr
        || gestureRouter_ == nullptr || hostWindowId == 0) {
        accepting_ = false;
        return;
    }

    QPointer<AppTabRuntimeController> guard(this);
    launcher_ = std::make_unique<InstalledPackageWorkerLauncher>(
        std::move(*boundary.value), std::move(workerExecutable),
        std::move(sandboxTempRoot), mockOrigin_,
        [authority = authority_](
            const WorkerLaunchRequest &request,
            std::shared_ptr<const ImmutablePackageGuard> retainedGuard) {
            return authority->revalidateWorkerLaunch(request,
                                                     std::move(retainedGuard));
        },
        std::move(admission),
        [guard](InstalledPackageWorkerLauncher::CommittedAttachTransaction tx) {
            if (!guard) return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
            return guard->realizeAttach(std::move(tx));
        },
        [guard] {
            if (guard) guard->stopCurrent(QStringLiteral("host.worker.relaunch"));
        },
        [](WorkerAttemptKey, bool) {},
        [](WorkerAttemptKey, const QString &, quint32) {},
        this);
    accepting_ = launcher_->isAccepting();

    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::readyForRequest, this,
            [this](const WorkerLaunchRequest &request,
                   const quint32 processId) {
                if (pendingAttach_.has_value()
                    && hasSameWorkerLaunchAuthority(
                        pendingAttach_->request, request)) {
                    return;
                }
                const WorkerLaunchRequest projectedRequest =
                    currentRequest_.has_value()
                        && hasSameWorkerLaunchAuthority(*currentRequest_, request)
                    ? *currentRequest_ : request;
                emit ready(projectedRequest, processId);
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::workerExitedForRequest, this,
            [this](const WorkerLaunchRequest &request, const bool expected) {
                QPointer<AppTabRuntimeController> self(this);
                emit self->workerExited(request, expected);
                if (!self) return;
                if (self->currentRequest_.has_value()
                    && hasSameWorkerLaunchAuthority(*self->currentRequest_,
                                                    request)) {
                    self->stopCurrent(
                        expected ? QStringLiteral("host.worker.expected_exit")
                                 : QStringLiteral(
                                       "host.worker.unexpected_exit"));
                }
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::launchFailedForRequest, this,
            [this](const WorkerLaunchRequest &request, const QString &error,
                   const quint32 nativeError) {
                QPointer<AppTabRuntimeController> self(this);
                emit self->workerFailed(request, error, nativeError);
                if (!self) return;
                if (self->currentRequest_.has_value()
                    && hasSameWorkerLaunchAuthority(*self->currentRequest_,
                                                    request)) {
                    self->stopCurrent(QStringLiteral("host.worker.failed"));
                }
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::terminalFailure, this,
            [this](const QString &error, const quint32 nativeError) {
                accepting_ = false;
                expectedLaunchTarget_.reset();
                currentRequest_.reset();
                activeGeneration_ = 0;
                const QString stableTabId = tabId_;
                const QPointer<AppTabRuntimeController> self(this);
                stopCurrent(error);
                if (self) {
                    emit self->launcherTerminalFailure(
                        stableTabId, error, nativeError);
                }
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::retirementCompletedForRequest,
            this,
            [this](const WorkerLaunchRequest &request, const bool) {
                if (currentRequest_.has_value()
                    && hasSameWorkerLaunchAuthority(*currentRequest_,
                                                    request)) {
                    currentRequest_.reset();
                    activeGeneration_ = 0;
                }
                if (expectedLaunchTarget_.has_value()
                    && hasSameWorkerLaunchAuthority(
                        expectedLaunchTarget_->request, request)) {
                    expectedLaunchTarget_.reset();
                }
                if (pendingAttach_.has_value()
                    && hasSameWorkerLaunchAuthority(
                        pendingAttach_->request, request)) {
                    clearPendingAttach();
                }
                if (closing_ && !retiredEmitted_
                    && !currentRequest_.has_value()
                    && !pendingAttach_.has_value()
                    && (launcher_ == nullptr
                        || !launcher_->hasPendingActivity())) {
                    retiredEmitted_ = true;
                    emit retired(tabId_, runtimeIncarnation_);
                }
            },
            Qt::DirectConnection);
}

AppTabRuntimeController::~AppTabRuntimeController()
{
    close(QStringLiteral("host.worker.controller_destroyed"));
}

QString AppTabRuntimeController::tabId() const
{
    return tabId_;
}

bool AppTabRuntimeController::isAccepting() const noexcept
{
    return accepting_ && !closing_
        && (transportGate_ == nullptr
            || transportGate_->load(std::memory_order_acquire))
        && launcher_ != nullptr
        && launcher_->isAccepting();
}

bool AppTabRuntimeController::hasWorkerContext() const noexcept
{
    return processLifetime_ != nullptr && sessionController_ != nullptr
        && sessionController_->state() == HostWorkerSessionState::Running;
}

quint64 AppTabRuntimeController::runtimeIncarnation() const noexcept
{
    return runtimeIncarnation_;
}

const std::optional<WorkerLaunchRequest> &
AppTabRuntimeController::currentRequest() const noexcept
{
    return currentRequest_;
}

HostWorkerSessionController *AppTabRuntimeController::sessionController() const noexcept
{
    return sessionController_.get();
}

InstalledPackageWorkerLauncher *AppTabRuntimeController::launcher() const noexcept
{
    return launcher_.get();
}

HostCapabilityRuntime *AppTabRuntimeController::capabilityRuntime() const noexcept
{
    return capabilityRuntime_.get();
}

bool AppTabRuntimeController::requestLaunch(
    const WorkerLaunchRequest &request,
    const quint64 expectedNavigationIncarnation,
    const QString &originalCanonicalAddress)
{
    if (!isAccepting() || request.tabId != tabId_
        || request.runtimeIncarnation == 0 || launcher_ == nullptr
        || expectedNavigationIncarnation == 0
        || originalCanonicalAddress.isEmpty() || mainWindow_.isNull()
        || !mainWindow_->isAppLaunchTargetCurrent(
            tabId_, expectedNavigationIncarnation, request.lease.appId,
            request.route, originalCanonicalAddress)) {
        return false;
    }
    currentRequest_ = request;
    expectedLaunchTarget_ = ExpectedLaunchTarget{
        request, expectedNavigationIncarnation, originalCanonicalAddress};
    runtimeIncarnation_ = request.runtimeIncarnation;
    InstalledPackageWorkerLauncher *const launcher = launcher_.get();
    const QPointer<AppTabRuntimeController> self(this);
    const bool launched = launcher->requestLaunch(request);
    if (!self) return launched;
    if (!launched && self->launcher_.get() == launcher
        && self->currentRequest_.has_value()
        && hasSameWorkerLaunchAuthority(*self->currentRequest_, request)
        && !launcher->hasPendingActivity(request)
        && !launcher->hasPendingActivity()) {
        self->currentRequest_.reset();
        if (self->expectedLaunchTarget_.has_value()
            && hasSameWorkerLaunchAuthority(
                self->expectedLaunchTarget_->request, request)) {
            self->expectedLaunchTarget_.reset();
        }
        if (self->closing_ && !self->retiredEmitted_) {
            self->retiredEmitted_ = true;
            emit self->retired(self->tabId_, self->runtimeIncarnation_);
        }
    }
    return launched;
}

bool AppTabRuntimeController::retargetPendingLaunch(
    const quint64 runtimeIncarnation,
    const quint64 expectedNavigationIncarnation,
    const QString &route,
    const QString &originalCanonicalAddress)
{
    if (!isAccepting() || runtimeIncarnation == 0
        || expectedNavigationIncarnation == 0 || route.isEmpty()
        || originalCanonicalAddress.isEmpty() || mainWindow_.isNull()
        || !currentRequest_.has_value()
        || currentRequest_->runtimeIncarnation != runtimeIncarnation
        || !expectedLaunchTarget_.has_value()
        || !hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, *currentRequest_)) {
        return false;
    }
    WorkerLaunchRequest updatedRequest = *currentRequest_;
    updatedRequest.route = route;
    if (!mainWindow_->isAppLaunchTargetCurrent(
            tabId_, expectedNavigationIncarnation,
            updatedRequest.lease.appId, route, originalCanonicalAddress)) {
        return false;
    }
    currentRequest_ = updatedRequest;
    expectedLaunchTarget_ = ExpectedLaunchTarget{
        updatedRequest, expectedNavigationIncarnation,
        originalCanonicalAddress};
    if (pendingAttach_.has_value()
        && hasSameWorkerLaunchAuthority(
            pendingAttach_->request, updatedRequest)) {
        pendingAttach_->request = updatedRequest;
        pendingAttach_->expectedNavigationIncarnation =
            expectedNavigationIncarnation;
        pendingAttach_->originalCanonicalAddress = originalCanonicalAddress;
    }
    return true;
}

bool AppTabRuntimeController::requestRouteLoad(const QString &route)
{
    const bool queued = sessionController_ != nullptr
        && sessionController_->requestRouteLoad(route);
    if (queued && currentRequest_.has_value()) currentRequest_->route = route;
    return queued;
}

bool AppTabRuntimeController::cancelLaunchIfCurrent(
    const WorkerLaunchRequest &request, const QString &reason)
{
    if (reason.isEmpty() || !currentRequest_.has_value()
        || !hasSameWorkerLaunchAuthority(*currentRequest_, request)) {
        return false;
    }
    InstalledPackageWorkerLauncher *const launcher = launcher_.get();
    if (launcher != nullptr) (void)launcher->cancelLaunch(request);
    const QPointer<AppTabRuntimeController> self(this);
    stopCurrent(reason);
    if (!self) return true;
    if (self->currentRequest_.has_value()
        && hasSameWorkerLaunchAuthority(*self->currentRequest_, request)) {
        self->currentRequest_.reset();
        self->activeGeneration_ = 0;
    }
    if (self->expectedLaunchTarget_.has_value()
        && hasSameWorkerLaunchAuthority(
            self->expectedLaunchTarget_->request, request)) {
        self->expectedLaunchTarget_.reset();
    }
    return true;
}

bool AppTabRuntimeController::sendVisibilityChanged(const bool active)
{
    return sessionController_ != nullptr
        && sessionController_->sendVisibilityChanged(active);
}

#ifdef Q_BROWSER_HOST_TESTING
void AppTabRuntimeController::deferNextAttachForTesting() noexcept
{
    deferNextAttachForTesting_ = true;
}

bool AppTabRuntimeController::hasPendingAttachForTesting() const noexcept
{
    return pendingAttach_.has_value();
}

bool AppTabRuntimeController::resumePendingAttachForTesting()
{
    if (!pendingAttach_.has_value()) return false;
    holdPendingAttachForTesting_ = false;
    completePendingAttach(
        sessionController_ != nullptr ? sessionController_->generation() : 0);
    return true;
}

void AppTabRuntimeController::setAfterPendingSessionAttachHookForTesting(
    std::function<void()> hook)
{
    afterPendingSessionAttachHookForTesting_ = std::move(hook);
}
#endif

void AppTabRuntimeController::failClosed(const QString &reason)
{
    if (transportGateMutex_ != nullptr) {
        std::lock_guard lock(*transportGateMutex_);
        if (transportGate_ != nullptr) {
            transportGate_->store(false, std::memory_order_release);
        }
    } else if (transportGate_ != nullptr) {
        transportGate_->store(false, std::memory_order_release);
    }
    clearPendingAttach();
    if (capabilityRuntime_ != nullptr) capabilityRuntime_->invalidate();
    QPointer<InstalledPackageWorkerLauncher> launcher(launcher_.get());
    if (launcher != nullptr) {
        (void)QMetaObject::invokeMethod(
            launcher,
            [launcher] {
                if (launcher) launcher->cancel();
            },
            Qt::QueuedConnection);
    }
    if (sessionController_ != nullptr
        && sessionController_->state() == HostWorkerSessionState::Running) {
        (void)sessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker.fail_closed")
                             : reason);
    }
}

void AppTabRuntimeController::stop(const QString &reason)
{
    Q_UNUSED(reason);
    const QPointer<InstalledPackageWorkerLauncher> launcher(launcher_.get());
    if (launcher) launcher->stopCurrent();
}

void AppTabRuntimeController::close(const QString &reason)
{
    if (closing_) return;
    closing_ = true;
    accepting_ = false;
    expectedLaunchTarget_.reset();
    const QPointer<AppTabRuntimeController> self(this);
    const QPointer<InstalledPackageWorkerLauncher> launcher(launcher_.get());
    if (launcher) launcher->cancel();
    else stopCurrent(reason);
    if (!self) return;
    self->currentRequest_.reset();
    self->activeGeneration_ = 0;
    if ((launcher.isNull() || !launcher->hasPendingActivity())
        && self->processLifetime_ == nullptr && !self->retiredEmitted_) {
        self->retiredEmitted_ = true;
        emit self->retired(self->tabId_, self->runtimeIncarnation_);
    }
}

InstalledPackageWorkerLauncher::AttachResult
AppTabRuntimeController::realizeAttach(
    InstalledPackageWorkerLauncher::CommittedAttachTransaction transaction)
{
    if (!accepting_ || closing_ || tabController_.isNull()
        || mainWindow_.isNull() || gestureRouter_.isNull()
        || (transportGate_ != nullptr
            && !transportGate_->load(std::memory_order_acquire))
        || sessionController_ == nullptr || capabilityRuntime_ != nullptr) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    const WorkerLaunchRequest request = transaction.request();
    if (request.tabId != tabId_ || request.runtimeIncarnation == 0
        || request.admission == nullptr || request.lease.appId.isEmpty()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    if (!expectedLaunchTarget_.has_value()
        || !currentRequest_.has_value()
        || !hasSameWorkerLaunchAuthority(*currentRequest_, request)
        || !hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, request)
        || !isExpectedLaunchTarget(
            request, expectedLaunchTarget_->navigationIncarnation,
            expectedLaunchTarget_->canonicalAddress)) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    const ExpectedLaunchTarget target = *expectedLaunchTarget_;
    PendingAttach pending;
    pending.request = target.request;
    pending.expectedNavigationIncarnation = target.navigationIncarnation;
    pending.originalCanonicalAddress = target.canonicalAddress;
    pending.session = transaction.takeSession();
    pending.surface = transaction.takeSurface();
    pending.process = transaction.takeProcess();
    if (pending.session == nullptr || pending.surface == nullptr
        || pending.process == nullptr || !pending.session->isAuthenticated()
        || pending.session->isClosed()
        || pending.session->appIdentity() != target.request.lease.appId) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    if (sessionController_->generation() == std::numeric_limits<quint64>::max()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }

    const quintptr workerWindowId =
        static_cast<quintptr>(pending.surface->nativeWindowId());
    const quint64 expectedGeneration = sessionController_->generation() + 1;
    pending.expectedGeneration = expectedGeneration;
    const TabCapabilityAuthority authority{
        target.request.tabId,
        target.request.runtimeIncarnation,
        target.request.lease.appId,
        pending.process->processId(),
        workerWindowId,
        expectedGeneration,
        target.request.lease.leaseAuthorityEpoch};
    if (!authority.isValid()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }

    QString capabilityError;
    auto capability = HostCapabilityRuntime::create(
        authority, target.request.admission, gestureRouter_.data(),
        target.request.lease.permissions, mockOrigin_, storageDirectory_, hostWindowId_,
        &capabilityError, fileDialogCoordinator_);
    // The production factory needs the configured storage directory. A
    // controller constructed by HostApplication supplies it through the
    // capability factory below; this guard keeps malformed test fixtures
    // fail-closed instead of attaching a session without capabilities.
    if (capability == nullptr) {
        Q_UNUSED(capabilityError);
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }

    pending.capability = std::move(capability);
    std::unique_lock<std::mutex> transportLock;
    if (transportGateMutex_ != nullptr) {
        transportLock = std::unique_lock<std::mutex>(*transportGateMutex_);
    }
    if (transportGate_ != nullptr
        && !transportGate_->load(std::memory_order_acquire)) {
        if (transportLock.owns_lock()) transportLock.unlock();
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    if (transportLock.owns_lock()) transportLock.unlock();

    if (pending.capability->isWorkerInitializationComplete()
        && !pending.capability->isWorkerReady()) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }

    bool forceDeferredAttach = false;
#ifdef Q_BROWSER_HOST_TESTING
    forceDeferredAttach = std::exchange(deferNextAttachForTesting_, false);
    if (forceDeferredAttach) holdPendingAttachForTesting_ = true;
#endif
    if (forceDeferredAttach
        || !pending.capability->isWorkerInitializationComplete()
        || !sessionController_->canAttachImmediately()) {
        if (pendingAttach_.has_value()) {
            return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
        }
        HostCapabilityRuntime *const pendingRuntime = pending.capability.get();
        pendingAttach_ = std::move(pending);
        connect(
            pendingRuntime,
            &HostCapabilityRuntime::workerInitializationFinished,
            this,
            [this, pendingRuntime](const bool, const QString &) {
                if (pendingAttach_.has_value()
                    && pendingAttach_->capability.get() == pendingRuntime) {
#ifdef Q_BROWSER_HOST_TESTING
                    if (holdPendingAttachForTesting_) return;
#endif
                    completePendingAttach(
                        sessionController_ != nullptr
                            ? sessionController_->generation()
                            : 0);
                }
            });
        if (sessionController_->state() == HostWorkerSessionState::Running
            && !sessionController_->shutdown(
                QStringLiteral("host.worker.rebind"))) {
            clearPendingAttach();
            return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
        }
        return InstalledPackageWorkerLauncher::AttachResult::Attached;
    }

    std::unique_ptr<IpcSession> session = std::move(pending.session);
    if (!sessionController_->attach(std::move(session),
                                    pending.capability.get())
        || sessionController_->state() != HostWorkerSessionState::Running
        || sessionController_->generation() != expectedGeneration) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    if (expectedLaunchTarget_.has_value()
        && hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, pending.request)) {
        pending.request = expectedLaunchTarget_->request;
        pending.expectedNavigationIncarnation =
            expectedLaunchTarget_->navigationIncarnation;
        pending.originalCanonicalAddress =
            expectedLaunchTarget_->canonicalAddress;
    }
    if (!completeAttach(std::move(pending), expectedGeneration)) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    return InstalledPackageWorkerLauncher::AttachResult::Attached;
}

void AppTabRuntimeController::completePendingAttach(
    const quint64 detachedGeneration)
{
    Q_UNUSED(detachedGeneration);
    if (!pendingAttach_.has_value() || sessionController_ == nullptr) return;
    if (closing_ || !accepting_
        || (transportGate_ != nullptr
            && !transportGate_->load(std::memory_order_acquire))) {
        clearPendingAttach();
        return;
    }
    if (pendingAttach_->capability == nullptr) {
        clearPendingAttach();
        return;
    }
    if (!isExpectedLaunchTarget(
            pendingAttach_->request,
            pendingAttach_->expectedNavigationIncarnation,
            pendingAttach_->originalCanonicalAddress)) {
        const WorkerLaunchRequest staleRequest = pendingAttach_->request;
        clearPendingAttach();
        (void)cancelLaunchIfCurrent(
            staleRequest, QStringLiteral("host.worker.launch_target_stale"));
        return;
    }
    if (!pendingAttach_->capability->isWorkerInitializationComplete()) {
        return;
    }
    if (!pendingAttach_->capability->isWorkerReady()) {
        const WorkerLaunchRequest request = pendingAttach_->request;
        QString errorCode = pendingAttach_->capability->workerInitializationError();
        if (errorCode.isEmpty()) {
            errorCode = QStringLiteral("host.capability.worker_unavailable");
        }
        clearPendingAttach();
        emit workerFailed(request, errorCode, 0);
        return;
    }
    if (!sessionController_->canAttachImmediately()) return;

    PendingAttach pending = std::move(*pendingAttach_);
    pendingAttach_.reset();
    const WorkerLaunchRequest request = pending.request;
    const quint32 processId = pending.process != nullptr
        ? pending.process->processId() : 0;
    const quint64 expectedGeneration = pending.expectedGeneration;
    const QPointer<AppTabRuntimeController> sessionAttachGuard(this);
    const QPointer<HostWorkerSessionController> sessionGuard(
        sessionController_.get());
    bool attachAccepted = false;
    if (sessionGuard
        && sessionGuard->generation()
            != std::numeric_limits<quint64>::max()) {
        attachAccepted = sessionGuard->attach(
            std::move(pending.session), pending.capability.get());
    }
    if (!sessionAttachGuard || !sessionGuard) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        return;
    }
    if (!attachAccepted
        || sessionGuard->state() != HostWorkerSessionState::Running
        || sessionGuard->generation() != expectedGeneration) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        if (sessionAttachGuard->currentRequest_.has_value()
            && hasSameWorkerLaunchAuthority(
                *sessionAttachGuard->currentRequest_, request)) {
            const WorkerLaunchRequest failedRequest =
                *sessionAttachGuard->currentRequest_;
            const quint64 observedSessionGeneration =
                sessionGuard->generation();
            const quint64 observedActiveGeneration =
                sessionAttachGuard->activeGeneration_;
            emit sessionAttachGuard->workerFailed(
                failedRequest, QStringLiteral("host.launch.attach_failed"), 0);
            const QPointer<AppTabRuntimeController> self(sessionAttachGuard);
            if (self && self->currentRequest_.has_value()
                && hasSameWorkerLaunchAuthority(
                    *self->currentRequest_, failedRequest)
                && self->sessionController_ != nullptr
                && self->sessionController_->generation()
                       == observedSessionGeneration
                && self->activeGeneration_ == observedActiveGeneration
                && !self->pendingAttach_.has_value()
                && observedSessionGeneration <= expectedGeneration) {
                self->stopCurrent(
                    QStringLiteral("host.worker.reattach_failed"));
            }
        }
        return;
    }
#ifdef Q_BROWSER_HOST_TESTING
    std::function<void()> afterSessionAttach = std::move(
        sessionAttachGuard->afterPendingSessionAttachHookForTesting_);
    sessionAttachGuard->afterPendingSessionAttachHookForTesting_ = {};
    if (afterSessionAttach) afterSessionAttach();
    if (!sessionAttachGuard) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        return;
    }
#endif
    if (expectedLaunchTarget_.has_value()
        && hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, pending.request)) {
        pending.request = expectedLaunchTarget_->request;
        pending.expectedNavigationIncarnation =
            expectedLaunchTarget_->navigationIncarnation;
        pending.originalCanonicalAddress =
            expectedLaunchTarget_->canonicalAddress;
    }
    const QPointer<AppTabRuntimeController> attachGuard(this);
    const bool attached = completeAttach(
        std::move(pending), expectedGeneration);
    if (!attachGuard) return;
    if (!attached) {
        if (attachGuard->currentRequest_.has_value()
            && hasSameWorkerLaunchAuthority(
                *attachGuard->currentRequest_, request)) {
            const WorkerLaunchRequest failedRequest =
                *attachGuard->currentRequest_;
            const quint64 observedSessionGeneration =
                attachGuard->sessionController_->generation();
            const quint64 observedActiveGeneration =
                attachGuard->activeGeneration_;
            emit attachGuard->workerFailed(
                failedRequest, QStringLiteral("host.launch.attach_failed"), 0);
            const QPointer<AppTabRuntimeController> self(attachGuard);
            if (!self || !self->currentRequest_.has_value()
                || !hasSameWorkerLaunchAuthority(
                    *self->currentRequest_, failedRequest)
                || self->sessionController_ == nullptr
                || self->sessionController_->generation()
                       != observedSessionGeneration
                || self->activeGeneration_ != observedActiveGeneration
                || self->pendingAttach_.has_value()
                || observedSessionGeneration != expectedGeneration) {
                return;
            }
            self->stopCurrent(
                QStringLiteral("host.worker.reattach_failed"));
        }
        return;
    }
    if (attachGuard) emit attachGuard->ready(request, processId);
}

bool AppTabRuntimeController::completeAttach(PendingAttach pending,
                                              const quint64 expectedGeneration)
{
    const auto rollback = [this, &pending] {
        if (sessionController_ != nullptr
            && sessionController_->state()
                == HostWorkerSessionState::Running
            && sessionController_->generation()
                == pending.expectedGeneration) {
            (void)sessionController_->shutdown(
                QStringLiteral("host.worker.attach_rollback"));
        }
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        return false;
    };
    if (pending.session != nullptr || pending.surface == nullptr
        || pending.process == nullptr || pending.capability == nullptr
        || !pending.capability->isWorkerReady()
        || sessionController_ == nullptr || tabController_.isNull()
        || mainWindow_.isNull()
        || !isExpectedLaunchTarget(
            pending.request, pending.expectedNavigationIncarnation,
            pending.originalCanonicalAddress)
        || sessionController_->state() != HostWorkerSessionState::Running
        || sessionController_->generation() != expectedGeneration) {
        return rollback();
    }
    const TabCapabilityAuthority authority{
        pending.request.tabId,
        pending.request.runtimeIncarnation,
        pending.request.lease.appId,
        pending.process->processId(),
        static_cast<quintptr>(pending.surface->nativeWindowId()),
        expectedGeneration,
        pending.request.lease.leaseAuthorityEpoch};
    if (!authority.isValid()
        || pending.capability->authority() != authority) {
        return rollback();
    }
    {
        std::unique_lock<std::mutex> transportLock;
        if (transportGateMutex_ != nullptr) {
            transportLock = std::unique_lock<std::mutex>(*transportGateMutex_);
        }
        if (transportGate_ != nullptr
            && !transportGate_->load(std::memory_order_acquire)) {
            return rollback();
        }
        const QPointer<AppTabRuntimeController> self(this);
        const QPointer<MainWindow> windowGuard(mainWindow_);
        const bool surfaceAttached = windowGuard
            && windowGuard->attachAppWorkerSurfaceIfCurrent(
                tabId_, pending.expectedNavigationIncarnation,
                pending.request.lease.appId, pending.request.route,
                pending.originalCanonicalAddress,
                std::move(pending.surface));
        if (!self) {
            if (pending.process != nullptr) {
                pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
            }
            HostCapabilityRuntime::retire(std::move(pending.capability));
            return false;
        }
        if (!surfaceAttached) return rollback();
        if (!self->isExpectedLaunchTarget(
                pending.request, pending.expectedNavigationIncarnation,
                pending.originalCanonicalAddress)) {
            if (windowGuard) windowGuard->detachWorkerSurface(tabId_);
            return rollback();
        }
    }

    processLifetime_ = std::move(pending.process);
    stopProcess_ = [processLifetime = processLifetime_] {
        if (processLifetime != nullptr) {
            processLifetime->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
    };
    capabilityRuntime_ = std::move(pending.capability);
    currentRequest_ = pending.request;
    runtimeIncarnation_ = pending.request.runtimeIncarnation;
    activeGeneration_ = expectedGeneration;
    if (transportGate_ != nullptr
        && !transportGate_->load(std::memory_order_acquire)) {
        stopCurrent(QStringLiteral("host.worker.attach_gate_closed"));
        return false;
    }
    if (!sessionController_->requestRouteLoad(pending.request.route)) {
        stopCurrent(QStringLiteral("host.worker.attach_route_failed"));
        return false;
    }
    if (expectedLaunchTarget_.has_value()
        && hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, pending.request)) {
        expectedLaunchTarget_.reset();
    }
    if (sessionController_->state() == HostWorkerSessionState::Running
        && sessionController_->generation() == activeGeneration_) {
        (void)sessionController_->sendVisibilityChanged(
            tabController_->isActive()
            && tabController_->surfaceKind() == HostSurfaceKind::Worker);
    }
    return true;
}

bool AppTabRuntimeController::isExpectedLaunchTarget(
    const WorkerLaunchRequest &request,
    const quint64 expectedNavigationIncarnation,
    const QString &originalCanonicalAddress) const
{
    return expectedLaunchTarget_.has_value() && !mainWindow_.isNull()
        && expectedLaunchTarget_->navigationIncarnation
            == expectedNavigationIncarnation
        && expectedLaunchTarget_->canonicalAddress
            == originalCanonicalAddress
        && hasSameWorkerLaunchAuthority(
            expectedLaunchTarget_->request, request)
        && mainWindow_->isAppLaunchTargetCurrent(
            tabId_, expectedNavigationIncarnation,
            expectedLaunchTarget_->request.lease.appId,
            expectedLaunchTarget_->request.route,
            originalCanonicalAddress);
}

void AppTabRuntimeController::clearPendingAttach() noexcept
{
    if (!pendingAttach_.has_value()) return;
    if (pendingAttach_->process != nullptr) {
        pendingAttach_->process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    }
    HostCapabilityRuntime::retire(std::move(pendingAttach_->capability));
    pendingAttach_.reset();
}

void AppTabRuntimeController::stopCurrent(const QString &reason)
{
    const bool launcherOwnsClosingRetirement = closing_ && launcher_ != nullptr;
    std::optional<PendingAttach> pending = std::move(pendingAttach_);
    pendingAttach_.reset();
    std::shared_ptr<HostCapabilityRuntime> capability =
        std::exchange(capabilityRuntime_, {});
    std::shared_ptr<SandboxProcess> process =
        std::exchange(processLifetime_, {});
    std::function<void()> stopProcess = std::exchange(stopProcess_, {});
    const QPointer<HostWorkerSessionController> session(
        sessionController_.get());
    const QPointer<MainWindow> window(mainWindow_);
    const QString stableTabId = tabId_;
    activeGeneration_ = 0;

    if (pending.has_value()) {
        if (pending->process != nullptr) {
            pending->process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending->capability));
    }
    if (session && capability != nullptr) {
        session->unbindCapabilityRuntime(capability->authority());
    }
    if (session && session->state() == HostWorkerSessionState::Running) {
        (void)session->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker.stop") : reason);
    }
    HostCapabilityRuntime::retire(std::move(capability));
    if (window && !closing_) window->detachWorkerSurface(stableTabId);
    if (stopProcess && !launcherOwnsClosingRetirement) stopProcess();
    Q_UNUSED(process);
}
