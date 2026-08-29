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

namespace {

QString attemptKey(const WorkerAttemptKey &key)
{
    return QStringLiteral("%1/%2").arg(key.activation.value).arg(key.attempt.value);
}

}

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
                emit failed(*currentRequest_, errorCode, generation);
                stopCurrent(QStringLiteral("host.worker_session.failed"));
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
        [guard](const WorkerAttemptKey key, const bool expected) {
            if (!guard) return;
            const auto request = guard->requestForAttempt(key);
            if (!request.has_value()) return;
            emit guard->workerExited(*request, expected);
            if (guard->currentRequest_.has_value()
                && guard->currentRequest_->attempt == key) {
                guard->stopCurrent(QStringLiteral("host.worker.exited"));
            }
        },
        [guard](const WorkerAttemptKey key, const QString &error,
                const quint32 nativeError) {
            if (!guard) return;
            const auto request = guard->requestForAttempt(key);
            if (request.has_value()) {
                emit guard->workerFailed(*request, error, nativeError);
                if (guard->currentRequest_.has_value()
                    && guard->currentRequest_->attempt == key) {
                    guard->stopCurrent(QStringLiteral("host.worker.failed"));
                }
            }
        },
        this);
    accepting_ = launcher_->isAccepting();

    connect(launcher_.get(), &InstalledPackageWorkerLauncher::ready, this,
            [this](const QString &, const QString &, const QString &,
                   const quint64 activation, const quint64 attempt,
                   const quint32 processId) {
                const WorkerAttemptKey key{WorkerActivationId{activation},
                                           WorkerAttemptId{attempt}};
                if (pendingAttach_.has_value()
                    && pendingAttach_->request.attempt == key) {
                    return;
                }
                const auto request = requestForAttempt(key);
                if (request.has_value()) emit ready(*request, processId);
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::unexpectedExit, this,
            [this](const quint64 activation, const quint64 attempt) {
                const auto request = requestForAttempt(
                    WorkerAttemptKey{WorkerActivationId{activation},
                                     WorkerAttemptId{attempt}});
                if (request.has_value() && currentRequest_.has_value()
                    && currentRequest_->attempt == request->attempt) {
                    stopCurrent(QStringLiteral("host.worker.unexpected_exit"));
                }
            },
            Qt::DirectConnection);
    connect(launcher_.get(),
            &InstalledPackageWorkerLauncher::retirementCompleted, this,
            [this](const quint64 activation, const quint64 attempt,
                   const bool) {
                const auto request = requestForAttempt(
                    WorkerAttemptKey{WorkerActivationId{activation},
                                     WorkerAttemptId{attempt}});
                if (!request.has_value()) return;
                const WorkerAttemptKey key = request->attempt;
                if (currentRequest_.has_value()
                    && currentRequest_->attempt == key) {
                    currentRequest_.reset();
                    activeGeneration_ = 0;
                }
                if (pendingAttach_.has_value()
                    && pendingAttach_->request.attempt == key) {
                    clearPendingAttach();
                }
                if (closing_ && !retiredEmitted_) {
                    retiredEmitted_ = true;
                    emit retired(tabId_, request->runtimeIncarnation);
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

bool AppTabRuntimeController::requestLaunch(const WorkerLaunchRequest &request)
{
    if (!isAccepting() || request.tabId != tabId_
        || request.runtimeIncarnation == 0 || launcher_ == nullptr) {
        return false;
    }
    rememberRequest(request);
    currentRequest_ = request;
    runtimeIncarnation_ = request.runtimeIncarnation;
    const bool launched = launcher_->requestLaunch(request);
    if (!launched && currentRequest_.has_value()
        && currentRequest_->attempt == request.attempt
        && !launcher_->hasPendingActivity(request.attempt)
        && !launcher_->hasPendingActivity()) {
        currentRequest_.reset();
        if (closing_ && !retiredEmitted_) {
            retiredEmitted_ = true;
            emit retired(tabId_, runtimeIncarnation_);
        }
    }
    return launched;
}

bool AppTabRuntimeController::requestRouteLoad(const QString &route)
{
    const bool queued = sessionController_ != nullptr
        && sessionController_->requestRouteLoad(route);
    if (queued && currentRequest_.has_value()) currentRequest_->route = route;
    return queued;
}

bool AppTabRuntimeController::sendVisibilityChanged(const bool active)
{
    return sessionController_ != nullptr
        && sessionController_->sendVisibilityChanged(active);
}

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
    if (launcher_ == nullptr) return;
    if (capabilityRuntime_ != nullptr) capabilityRuntime_->invalidate();
    if (sessionController_ != nullptr
        && sessionController_->state() == HostWorkerSessionState::Running) {
        (void)sessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker.stop") : reason);
    }
    launcher_->stopCurrent();
}

void AppTabRuntimeController::close(const QString &reason)
{
    if (closing_) return;
    closing_ = true;
    accepting_ = false;
    clearPendingAttach();
    if (capabilityRuntime_ != nullptr) capabilityRuntime_->invalidate();
    if (launcher_ != nullptr) {
        if (sessionController_ != nullptr
            && sessionController_->state() == HostWorkerSessionState::Running) {
            (void)sessionController_->shutdown(
                reason.isEmpty() ? QStringLiteral("host.worker.close") : reason);
        }
        launcher_->cancel();
        if (processLifetime_ == nullptr && !currentRequest_.has_value()
            && !launcher_->hasPendingActivity()
            && !retiredEmitted_) {
            retiredEmitted_ = true;
            emit retired(tabId_, runtimeIncarnation_);
        }
    } else {
        stopCurrent(reason);
        if (!retiredEmitted_) {
            retiredEmitted_ = true;
            emit retired(tabId_, runtimeIncarnation_);
        }
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
    if (currentRequest_.has_value() && currentRequest_->attempt != request.attempt) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    PendingAttach pending;
    pending.request = request;
    pending.session = transaction.takeSession();
    pending.surface = transaction.takeSurface();
    pending.process = transaction.takeProcess();
    if (pending.session == nullptr || pending.surface == nullptr
        || pending.process == nullptr || !pending.session->isAuthenticated()
        || pending.session->isClosed()
        || pending.session->appIdentity() != request.lease.appId) {
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
        request.tabId,
        request.runtimeIncarnation,
        request.lease.appId,
        pending.process->processId(),
        workerWindowId,
        expectedGeneration,
        request.lease.leaseAuthorityEpoch};
    if (!authority.isValid()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }

    QString capabilityError;
    auto capability = HostCapabilityRuntime::create(
        authority, request.admission, gestureRouter_.data(),
        request.lease.permissions, mockOrigin_, storageDirectory_, hostWindowId_,
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

    if (!pending.capability->isWorkerInitializationComplete()
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
    if (sessionController_->generation() == std::numeric_limits<quint64>::max()
        || !sessionController_->attach(std::move(pending.session),
                                       pending.capability.get())
        || sessionController_->state() != HostWorkerSessionState::Running
        || sessionController_->generation() != expectedGeneration) {
        if (pending.process != nullptr) {
            pending.process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        }
        HostCapabilityRuntime::retire(std::move(pending.capability));
        if (currentRequest_.has_value()
            && currentRequest_->attempt == request.attempt) {
            emit workerFailed(currentRequest_.value(),
                              QStringLiteral("host.launch.attach_failed"), 0);
            stopCurrent(QStringLiteral("host.worker.reattach_failed"));
        }
        return;
    }
    if (!completeAttach(std::move(pending), expectedGeneration)) {
        if (currentRequest_.has_value()
            && currentRequest_->attempt == request.attempt) {
            emit workerFailed(currentRequest_.value(),
                              QStringLiteral("host.launch.attach_failed"), 0);
        }
        stopCurrent(QStringLiteral("host.worker.reattach_failed"));
        return;
    }
    emit ready(request, processId);
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
        if (!mainWindow_->attachWorkerSurface(tabId_,
                                              std::move(pending.surface))) {
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
    const quint64 navigationIncarnation = tabController_->incarnation();
    if (!tabController_->bindAppWorkerSurface(
            pending.request.lease.appId, navigationIncarnation)
        || !sessionController_->requestRouteLoad(pending.request.route)) {
        stopCurrent(QStringLiteral("host.worker.attach_route_failed"));
        return false;
    }
    if (sessionController_->state() == HostWorkerSessionState::Running
        && sessionController_->generation() == activeGeneration_) {
        (void)sessionController_->sendVisibilityChanged(
            tabController_->isActive()
            && tabController_->surfaceKind() == HostSurfaceKind::Worker);
    }
    return true;
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
    clearPendingAttach();
    if (sessionController_ != nullptr
        && sessionController_->state() == HostWorkerSessionState::Running) {
        (void)sessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker.stop") : reason);
    }
    if (sessionController_ != nullptr && capabilityRuntime_ != nullptr) {
        sessionController_->unbindCapabilityRuntime(
            capabilityRuntime_->authority());
    }
    HostCapabilityRuntime::retire(std::exchange(capabilityRuntime_, {}));
    if (mainWindow_ != nullptr) mainWindow_->detachWorkerSurface(tabId_);
    if (stopProcess_) stopProcess_();
    stopProcess_ = {};
    processLifetime_.reset();
    activeGeneration_ = 0;
}

void AppTabRuntimeController::rememberRequest(const WorkerLaunchRequest &request)
{
    requestHistory_.insert(attemptKey(request.attempt), request);
    constexpr int maximumRememberedRequests = 128;
    while (requestHistory_.size() > maximumRememberedRequests) {
        requestHistory_.erase(requestHistory_.begin());
    }
}

std::optional<WorkerLaunchRequest> AppTabRuntimeController::requestForAttempt(
    const WorkerAttemptKey &key) const
{
    const auto found = requestHistory_.constFind(attemptKey(key));
    if (found != requestHistory_.cend()) return *found;
    if (currentRequest_.has_value() && currentRequest_->attempt == key) {
        return currentRequest_;
    }
    return std::nullopt;
}
