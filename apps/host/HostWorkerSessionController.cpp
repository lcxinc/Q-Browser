#include "HostWorkerSessionController.h"

#include "HostWorkerSessionIo.h"
#include "HostCapabilityRuntime.h"
#include "IpcSession.h"
#include "MainWindow.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QThread>

#include <limits>
#include <utility>

namespace {

constexpr qsizetype maximumRawPageTitleCodeUnits = 4096;
constexpr qsizetype maximumPageTitleCodeUnits = 256;

bool isBidiControl(const char16_t value)
{
    return value == 0x061c || (value >= 0x200e && value <= 0x200f)
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

std::optional<QString> canonicalPageTitle(const QString &untrusted)
{
    if (untrusted.isEmpty() || untrusted.size() > maximumRawPageTitleCodeUnits) {
        return std::nullopt;
    }
    QString canonical;
    canonical.reserve(maximumPageTitleCodeUnits);
    bool prefixComplete = false;
    for (qsizetype index = 0; index < untrusted.size(); ++index) {
        const QChar character = untrusted.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= untrusted.size()
                || !untrusted.at(index + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            if (!prefixComplete
                && canonical.size() + 2 <= maximumPageTitleCodeUnits) {
                canonical.append(character);
                canonical.append(untrusted.at(index + 1));
                prefixComplete = canonical.size() == maximumPageTitleCodeUnits;
            } else if (!prefixComplete) {
                prefixComplete = true;
            }
            ++index;
            continue;
        }
        if (character.isLowSurrogate() || character == u'<' || character == u'>') {
            return std::nullopt;
        }
        if (character.category() == QChar::Other_Control
            || isBidiControl(character.unicode())) {
            continue;
        }
        if (!prefixComplete) {
            canonical.append(character);
            prefixComplete = canonical.size() == maximumPageTitleCodeUnits;
        }
    }
    return canonical.isEmpty() ? std::nullopt
                               : std::optional<QString>(std::move(canonical));
}

} // namespace

HostWorkerSessionController::HostWorkerSessionController(
    NavigationCallback navigate, QObject *const parent)
    : QObject(parent), navigate_(std::move(navigate))
{
}

HostWorkerSessionController::HostWorkerSessionController(MainWindow *window,
                                                         QObject *parent)
    : HostWorkerSessionController(
          window == nullptr
              ? NavigationCallback{}
              : NavigationCallback{
                    [window = QPointer<MainWindow>(window)](
                        const QString &appId, const QString &route) {
                        return !window.isNull()
                            && window->navigateFromWorker(appId, route);
                    }},
          parent)
{}

HostWorkerSessionController::~HostWorkerSessionController()
{
    Q_ASSERT(QThread::currentThread() == thread());
    stopIoThreadForDestruction();
}

bool HostWorkerSessionController::attach(
    std::unique_ptr<IpcSession> session,
    HostCapabilityRuntime *const capabilityRuntime)
{
    if (QThread::currentThread() != thread()) {
        lastErrorCode_ = QStringLiteral("host.worker_session.wrong_thread");
        return false;
    }
    if (!navigate_ || session == nullptr || !session->isAuthenticated()
        || session->isClosed()
        || session->appIdentity().isEmpty()) {
        lastErrorCode_ = QStringLiteral("host.worker_session.invalid_attachment");
        return false;
    }
    std::optional<CapabilityBinding> capabilityBinding;
    if (capabilityRuntime != nullptr) {
        const TabCapabilityAuthority &authority =
            capabilityRuntime->authority();
        if (!authority.isValid()
            || authority.appIdentity != session->appIdentity()
            || generation_ == std::numeric_limits<quint64>::max()
            || authority.sessionGeneration != generation_ + 1) {
            lastErrorCode_ =
                QStringLiteral("host.worker_session.invalid_capability_binding");
            return false;
        }
        capabilityBinding = CapabilityBinding{capabilityRuntime, authority};
    }
    if (io_ != nullptr || ioThread_ != nullptr) {
        pendingSession_ = std::move(session);
        pendingCapabilityBinding_ = std::move(capabilityBinding);
        cleanupFinalState_ = HostWorkerSessionState::Detached;
        state_ = HostWorkerSessionState::ShuttingDown;
        pendingRouteLoads_.clear();
        outbound_.clear();
        activeCommand_.reset();
        activeCapabilityRequestId_.clear();
        invalidateCapabilityBinding();
        requestIoStop();
        return true;
    }
    return startSession(std::move(session), std::move(capabilityBinding));
}

bool HostWorkerSessionController::canAttachImmediately() const noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    return io_ == nullptr && ioThread_ == nullptr && pendingSession_ == nullptr;
}

bool HostWorkerSessionController::startSession(
    std::unique_ptr<IpcSession> session,
    std::optional<CapabilityBinding> capabilityBinding)
{
    if (session == nullptr || io_ != nullptr || ioThread_ != nullptr) return false;
    if (capabilityBinding.has_value()
        && (capabilityBinding->runtime.isNull()
            || !capabilityBinding->authority.isValid()
            || capabilityBinding->runtime->authority()
                != capabilityBinding->authority
            || capabilityBinding->authority.appIdentity
                != session->appIdentity()
            || generation_ == std::numeric_limits<quint64>::max()
            || capabilityBinding->authority.sessionGeneration
                != generation_ + 1)) {
        lastErrorCode_ =
            QStringLiteral("host.worker_session.invalid_capability_binding");
        return false;
    }
    QCoreApplication *const navigationDispatchContext =
        QCoreApplication::instance();
    if (navigationDispatchContext == nullptr
        || navigationDispatchContext->thread() != thread()) {
        lastErrorCode_ = QStringLiteral("host.worker_session.invalid_dispatch_context");
        return false;
    }
    session->setPageMetadataHandler({});
    ++generation_;
    const quint64 attachedGeneration = generation_;
    appIdentity_ = session->appIdentity();
    capabilityRuntime_ = capabilityBinding.has_value()
        ? capabilityBinding->runtime : QPointer<HostCapabilityRuntime>{};
    capabilityAuthority_ = capabilityBinding.has_value()
        ? std::optional<TabCapabilityAuthority>(capabilityBinding->authority)
        : std::nullopt;
    if (capabilityRuntime_ != nullptr) {
        const QPointer<HostWorkerSessionController> owningController(this);
        const TabCapabilityAuthority immutableAuthority = *capabilityAuthority_;
        if (!capabilityRuntime_->bindCompletionSubmitter(
                immutableAuthority,
                [owningController, immutableAuthority](
                    const std::shared_ptr<CapabilityDeliveryState> &delivery) {
                    HostWorkerSessionController *const controller =
                        owningController.data();
                    return controller != nullptr
                        && QThread::currentThread() == controller->thread()
                        && delivery != nullptr
                        && delivery->authority == immutableAuthority
                        && controller->submitCapabilityCompletion(delivery);
                })) {
            appIdentity_.clear();
            capabilityRuntime_ = nullptr;
            capabilityAuthority_.reset();
            lastErrorCode_ =
                QStringLiteral("host.worker_session.capability_bind_failed");
            return false;
        }
    }
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    activeCapabilityRequestId_.clear();
    lastErrorCode_.clear();
    ioThread_ = new QThread;
    ioThread_->setObjectName(QStringLiteral("host-worker-session-io"));
    io_ = new HostWorkerSessionIo(
        std::move(session), generation_, thread(), capabilityAuthority_);
    const QPointer<HostWorkerSessionIo> attachedIo = io_;
    QThread *const attachedThread = ioThread_;
    if (!io_->moveToThread(ioThread_)) {
        if (capabilityRuntime_ != nullptr && capabilityAuthority_.has_value()) {
            capabilityRuntime_->unbindCompletionSubmitter(
                *capabilityAuthority_);
        }
        delete io_.data();
        io_ = nullptr;
        delete ioThread_;
        ioThread_ = nullptr;
        appIdentity_.clear();
        capabilityRuntime_ = nullptr;
        capabilityAuthority_.reset();
        state_ = HostWorkerSessionState::Failed;
        lastErrorCode_ = QStringLiteral("host.worker_session.io_transfer_failed");
        return false;
    }
    connect(ioThread_, &QThread::started, io_.data(), &HostWorkerSessionIo::start);
    connect(ioThread_, &QThread::finished, this,
            [this, attachedIo, attachedThread, attachedGeneration] {
                handleIoThreadFinished(attachedIo, attachedThread,
                                       attachedGeneration);
            }, Qt::QueuedConnection);
    const QPointer<HostWorkerSessionController> navigationController(this);
    connect(io_.data(), &HostWorkerSessionIo::navigationRequested,
            navigationDispatchContext,
            [navigationController](const quint64 generation,
                                   const QString &requestId,
                                   const QString &route) {
                HostWorkerSessionController *const controller =
                    navigationController.data();
                if (controller != nullptr) {
                    controller->handleNavigationRequest(generation, requestId, route);
                }
            },
            Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::pageMetadataReceived, this,
            &HostWorkerSessionController::handlePageMetadata,
            Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::routeLoadResponse, this,
            &HostWorkerSessionController::handleRouteLoadResponse, Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::commandFinished, this,
            &HostWorkerSessionController::handleCommandFinished, Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::sessionFailed, this,
            [this](const quint64 generation, const QString &errorCode) {
                if (generation == generation_) failClosed(errorCode);
            }, Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::heartbeatObserved, this,
            [this](const quint64 generation) {
                if (generation == generation_
                    && state_ == HostWorkerSessionState::Running) {
                    emit heartbeatObserved(generation);
                }
            }, Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::capabilityRequested, this,
            &HostWorkerSessionController::handleCapabilityRequest,
            Qt::QueuedConnection);
    connect(io_.data(), &HostWorkerSessionIo::shutdownFinished, this,
            [this](const quint64 generation) {
                if (generation != generation_
                    || state_ != HostWorkerSessionState::ShuttingDown) return;
                pendingRouteLoads_.clear();
                outbound_.clear();
                activeCommand_.reset();
                appIdentity_.clear();
                cleanupFinalState_ = HostWorkerSessionState::Detached;
                requestIoStop();
            }, Qt::QueuedConnection);
    cleanupFinalState_ = HostWorkerSessionState::Failed;
    stopRequested_ = false;
    state_ = HostWorkerSessionState::Running;
    ioThread_->start();
    return true;
}

void HostWorkerSessionController::handlePageMetadata(
    const quint64 generation, const QString &title, const QString &status)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) {
        return;
    }
    const std::optional<QString> canonicalTitle = canonicalPageTitle(title);
    if (!canonicalTitle.has_value()) return;
    const auto validated = ProtocolMessage::pageMetadata(*canonicalTitle, status);
    if (!validated.has_value()) return;
    emit pageMetadataChanged(generation, *canonicalTitle, status);
}

bool HostWorkerSessionController::shutdown(const QString &reason)
{
    if (QThread::currentThread() != thread()) return false;
    if (state_ != HostWorkerSessionState::Running || io_ == nullptr || reason.isEmpty())
        return false;
    state_ = HostWorkerSessionState::ShuttingDown;
    cleanupFinalState_ = HostWorkerSessionState::Detached;
    outbound_.clear();
    activeCommand_.reset();
    activeCapabilityRequestId_.clear();
    pendingRouteLoads_.clear();
    invalidateCapabilityBinding();
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation, reason] {
        io->beginShutdown(generation, reason);
    }, Qt::QueuedConnection);
    return true;
}

bool HostWorkerSessionController::requestRouteLoad(const QString &route)
{
    return QThread::currentThread() == thread()
        && state_ == HostWorkerSessionState::Running
        && enqueueRouteLoad(route);
}

bool HostWorkerSessionController::sendVisibilityChanged(const bool active)
{
    if (QThread::currentThread() != thread()
        || state_ != HostWorkerSessionState::Running) {
        return false;
    }
    const auto message = ProtocolMessage::visibilityChanged(active);
    return message.has_value() && enqueueMessage(*message);
}

quint64 HostWorkerSessionController::generation() const noexcept
{
    return generation_;
}

HostWorkerSessionState HostWorkerSessionController::state() const noexcept { return state_; }
QString HostWorkerSessionController::lastErrorCode() const { return lastErrorCode_; }
qsizetype HostWorkerSessionController::pendingRouteLoadCount() const noexcept
{
    return pendingRouteLoads_.size();
}

qsizetype HostWorkerSessionController::pendingCapabilityCount() const noexcept
{
    return activeCapabilityRequestId_.isEmpty() ? 0 : 1;
}

bool HostWorkerSessionController::hasIoThread() const noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    return ioThread_ != nullptr;
}

bool HostWorkerSessionController::ioThreadRunning() const noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    return ioThread_ != nullptr && ioThread_->isRunning();
}

void HostWorkerSessionController::handleNavigationRequest(
    const quint64 generation, const QString &requestId, const QString &route)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) return;
    if (!pendingRouteLoads_.isEmpty()) {
        const auto response = ProtocolMessage::errorResponse(
            requestId, QStringLiteral("navigation.busy"),
            QStringLiteral("A route load is already pending."));
        if (!response.has_value() || !enqueueMessage(*response, true))
            failClosed(QStringLiteral("host.worker_session.response_queue_failed"));
        return;
    }
    const QPointer<HostWorkerSessionController> self(this);
    bool navigated = false;
    try {
        const NavigationCallback navigate = navigate_;
        const QString appIdentity = appIdentity_;
        navigated = navigate && navigate(appIdentity, route);
    } catch (...) {
        if (HostWorkerSessionController *const controller = self.data();
            controller != nullptr) {
            controller->failClosed(
                QStringLiteral("host.worker_session.navigation_callback_failed"));
        }
        return;
    }
    HostWorkerSessionController *const controller = self.data();
    if (controller == nullptr) return;
    if (generation != controller->generation_
        || controller->state_ != HostWorkerSessionState::Running) return;
    if (!navigated) {
        const auto response = ProtocolMessage::errorResponse(
            requestId, QStringLiteral("navigation.denied"),
            QStringLiteral("The requested route is not assigned to this worker."));
        if (!response.has_value() || !controller->enqueueMessage(*response, true))
            controller->failClosed(
                QStringLiteral("host.worker_session.response_queue_failed"));
        return;
    }
    const auto response = ProtocolMessage::successResponse(
        requestId, QJsonObject{{QStringLiteral("route"), route}});
    if (!response.has_value() || !controller->enqueueMessage(*response)
        || !controller->enqueueRouteLoad(route)) {
        controller->failClosed(QStringLiteral("host.worker_session.outbound_queue_full"));
        return;
    }
    if (!controller->outbound_.isEmpty())
        controller->outbound_.back().resumePollingAfter = true;
    else if (controller->activeCommand_.has_value())
        controller->activeCommand_->resumePollingAfter = true;
}

void HostWorkerSessionController::handleRouteLoadResponse(
    const quint64 generation, const QString &requestId, const QJsonObject &payload)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) return;
    const auto iterator = pendingRouteLoads_.find(requestId);
    if (iterator == pendingRouteLoads_.end()) {
        failClosed(QStringLiteral("host.worker_session.unexpected_response"));
        return;
    }
    const QString route = iterator.value();
    const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
    if (!payload.value(QStringLiteral("ok")).toBool(false)
        || result.value(QStringLiteral("route")).toString() != route) {
        failClosed(QStringLiteral("host.worker_session.route_load_rejected"));
        return;
    }
    pendingRouteLoads_.erase(iterator);
    emit routeLoadAcknowledged(route, generation);
    if (generation == generation_ && state_ == HostWorkerSessionState::Running)
        resumeIoPolling();
}

void HostWorkerSessionController::handleCapabilityRequest(
    const TabCapabilityAuthority &requestAuthority,
    const quint64 generation, const QString &requestId,
    const QString &capability, const QString &operation,
    const QJsonObject &payload)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running)
        return;
    emit capabilityRequestObserved(capability, operation, payload.toVariantMap(),
                                   generation);
    if (!activeCapabilityRequestId_.isEmpty()) {
        if (capabilityRuntime_ == nullptr || !capabilityAuthority_.has_value()
            || requestAuthority != *capabilityAuthority_
            || capabilityRuntime_->authority() != *capabilityAuthority_) {
            failClosed(QStringLiteral(
                "host.worker_session.capability_authority_mismatch"));
            return;
        }
        auto use = capabilityRuntime_->acquireResponseUse(
            requestAuthority, generation);
        if (use == nullptr
            || !queueCapabilityResponse(
                requestAuthority, std::move(use), generation, requestId,
                BrokerResult::failure(
                    QStringLiteral("capability.busy"),
                    QStringLiteral("A capability request is already active.")),
                false)) {
            failClosed(QStringLiteral("host.worker_session.response_queue_failed"));
        }
        return;
    }
    activeCapabilityRequestId_ = requestId;
    if (capabilityRuntime_ == nullptr || !capabilityAuthority_.has_value()) {
        if (requestAuthority.isValid()) {
            activeCapabilityRequestId_.clear();
            failClosed(QStringLiteral(
                "host.worker_session.capability_authority_mismatch"));
            return;
        }
        (void)queueCapabilityResponse(
            std::nullopt, nullptr, generation, requestId,
            BrokerResult::failure(QStringLiteral("capability.denied"),
                                  QStringLiteral("Capability is not permitted.")),
            true);
        return;
    }
    if (requestAuthority != *capabilityAuthority_
        || capabilityRuntime_->authority() != *capabilityAuthority_) {
        activeCapabilityRequestId_.clear();
        failClosed(
            QStringLiteral("host.worker_session.capability_authority_mismatch"));
        return;
    }
    capabilityRuntime_->dispatch(generation, requestId, capability, operation,
                                 payload);
}

bool HostWorkerSessionController::submitCapabilityCompletion(
    const std::shared_ptr<CapabilityDeliveryState> &delivery)
{
    if (delivery == nullptr || delivery->use == nullptr
        || QThread::currentThread() != thread()) {
        return false;
    }
    return queueCapabilityResponse(delivery->authority, delivery->use,
                                   delivery->generation, delivery->requestId,
                                   delivery->result, true);
}

bool HostWorkerSessionController::queueCapabilityResponse(
    std::optional<TabCapabilityAuthority> authority,
    std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
    const quint64 generation, const QString &requestId,
    const BrokerResult &result,
    const bool completesActiveRequest)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running
        || requestId.isEmpty()
        || (completesActiveRequest
            && requestId != activeCapabilityRequestId_)
        || (!completesActiveRequest
            && (activeCapabilityRequestId_.isEmpty()
                || requestId == activeCapabilityRequestId_
                || result.ok
                || result.errorCode != QStringLiteral("capability.busy")))) {
        return false;
    }
    if (authority.has_value()) {
        if (use == nullptr || !capabilityAuthority_.has_value()
            || *authority != *capabilityAuthority_
            || capabilityRuntime_ == nullptr
            || capabilityRuntime_->authority() != *capabilityAuthority_) {
            return false;
        }
    } else if (capabilityAuthority_.has_value()
               || use != nullptr || capabilityRuntime_ != nullptr || result.ok
               || result.errorCode != QStringLiteral("capability.denied")) {
        return false;
    }
    const auto response = result.ok
        ? ProtocolMessage::successResponse(requestId, result.value)
        : ProtocolMessage::errorResponse(requestId, result.errorCode,
                                         result.errorMessage);
    if (!response.has_value()) {
        (void)QMetaObject::invokeMethod(
            this,
            [this, generation] {
                if (generation == generation_)
                    failClosed(QStringLiteral("host.worker_session.response_invalid"));
            },
            Qt::QueuedConnection);
        return false;
    }
    if (outbound_.size() + (activeCommand_.has_value() ? 1 : 0)
        >= maximumQueuedCommands) {
        (void)QMetaObject::invokeMethod(
            this,
            [this, generation] {
                if (generation == generation_)
                    failClosed(QStringLiteral(
                        "host.worker_session.response_queue_failed"));
            },
            Qt::QueuedConnection);
        return false;
    }
    OutboundCommand command;
    command.id = ++nextCommandId_;
    command.message = *response;
    if (completesActiveRequest) command.capabilityRequestId = requestId;
    command.capabilityAuthority = std::move(authority);
    command.capabilityUse = std::move(use);
    outbound_.enqueue(std::move(command));
    pumpOutbound();
    const bool ok = result.ok;
    const QString resultErrorCode = result.errorCode;
    (void)QMetaObject::invokeMethod(
        this,
        [this, requestId, ok, resultErrorCode, generation] {
            if (generation == generation_)
                emit capabilityResponseQueued(requestId, ok, resultErrorCode,
                                              generation);
        },
        Qt::QueuedConnection);
    return true;
}

void HostWorkerSessionController::handleCommandFinished(
    const quint64 generation, const quint64 commandId, const bool success,
    const bool published,
    const QString &errorCode)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running
        || !activeCommand_.has_value() || activeCommand_->id != commandId) return;
    const bool resume = activeCommand_->resumePollingAfter;
    const QString completedCapability = activeCommand_->capabilityRequestId;
    activeCommand_.reset();
    if (!success) {
        failClosed(errorCode.isEmpty() ? QStringLiteral("host.worker_session.send_failed")
                                        : errorCode);
        return;
    }
    if (published && !completedCapability.isEmpty()
        && completedCapability == activeCapabilityRequestId_) {
        activeCapabilityRequestId_.clear();
        emit capabilityResponseSent(completedCapability, generation);
    } else if (!published && !completedCapability.isEmpty()
               && completedCapability == activeCapabilityRequestId_) {
        activeCapabilityRequestId_.clear();
    }
    pumpOutbound();
    if (resume && generation == generation_ && state_ == HostWorkerSessionState::Running)
        resumeIoPolling();
}

bool HostWorkerSessionController::enqueueMessage(const ProtocolMessage &message,
                                                 const bool resumePollingAfter)
{
    if (outbound_.size() + (activeCommand_.has_value() ? 1 : 0)
        >= maximumQueuedCommands) return false;
    OutboundCommand command;
    command.id = ++nextCommandId_;
    command.message = message;
    command.resumePollingAfter = resumePollingAfter;
    outbound_.enqueue(std::move(command));
    pumpOutbound();
    return true;
}

bool HostWorkerSessionController::enqueueRouteLoad(const QString &route)
{
    if (route.isEmpty() || pendingRouteLoads_.size() >= maximumQueuedCommands
        || outbound_.size() + (activeCommand_.has_value() ? 1 : 0)
               >= maximumQueuedCommands) return false;
    const QString requestId = QStringLiteral("host-route-%1").arg(++nextRouteLoadId_);
    const auto message = ProtocolMessage::routeLoad(requestId, route);
    if (!message.has_value()) return false;
    pendingRouteLoads_.insert(requestId, route);
    OutboundCommand command;
    command.id = ++nextCommandId_;
    command.message = *message;
    command.route = route;
    command.trackedRouteLoad = true;
    outbound_.enqueue(std::move(command));
    pumpOutbound();
    return true;
}

void HostWorkerSessionController::pumpOutbound()
{
    if (state_ != HostWorkerSessionState::Running || io_ == nullptr
        || activeCommand_.has_value() || outbound_.isEmpty()) return;
    activeCommand_ = outbound_.dequeue();
    const OutboundCommand command = *activeCommand_;
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation, command] {
        if (command.message.has_value())
            io->sendMessage(generation, command.id, *command.message,
                            command.trackedRouteLoad, command.route,
                            command.capabilityAuthority,
                            command.capabilityUse);
    }, Qt::QueuedConnection);
}

void HostWorkerSessionController::resumeIoPolling()
{
    if (io_ == nullptr) return;
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation] { io->resumePolling(generation); },
                              Qt::QueuedConnection);
}

void HostWorkerSessionController::failClosed(const QString &errorCode)
{
    if (state_ == HostWorkerSessionState::Failed
        || state_ == HostWorkerSessionState::Detached) return;
    state_ = HostWorkerSessionState::Failed;
    lastErrorCode_ = errorCode.isEmpty() ? QStringLiteral("host.worker_session.failed")
                                         : errorCode;
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    activeCapabilityRequestId_.clear();
    appIdentity_.clear();
    pendingCapabilityBinding_.reset();
    invalidateCapabilityBinding();
    cleanupFinalState_ = HostWorkerSessionState::Failed;
    requestIoStop();
    emit failed(lastErrorCode_, generation_);
}

void HostWorkerSessionController::unbindCapabilityRuntime(
    const TabCapabilityAuthority &authority) noexcept
{
    if (!capabilityAuthority_.has_value()
        || *capabilityAuthority_ != authority) {
        return;
    }
    invalidateCapabilityBinding();
}

void HostWorkerSessionController::invalidateCapabilityBinding() noexcept
{
    HostCapabilityRuntime *const runtime = capabilityRuntime_.data();
    if (runtime != nullptr && capabilityAuthority_.has_value()) {
        runtime->unbindCompletionSubmitter(*capabilityAuthority_);
    }
    capabilityRuntime_ = nullptr;
    capabilityAuthority_.reset();
    if (runtime != nullptr) runtime->invalidate();
}

void HostWorkerSessionController::requestIoStop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (stopRequested_ || ioThread_ == nullptr) return;
    if (!ioThread_->isRunning()) return;
    HostWorkerSessionIo *const io = io_.data();
    if (io == nullptr) {
        stopRequested_ = true;
        ioThread_->quit();
        return;
    }
    stopRequested_ = true;
    const quint64 generation = generation_;
    if (!QMetaObject::invokeMethod(
            io, [io, generation] { io->abort(generation); }, Qt::QueuedConnection))
        qFatal("Unable to queue host worker IO stop");
}

void HostWorkerSessionController::handleIoThreadFinished(
    const QPointer<HostWorkerSessionIo> oldIo, QThread *const oldThread,
    const quint64 generation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation_ != generation || io_ != oldIo || ioThread_ != oldThread) {
        if (!oldIo.isNull() && oldIo->thread() == thread()) oldIo->deleteLater();
        oldThread->deleteLater();
        return;
    }

    HostWorkerSessionIo *const completedIo = io_.data();
    io_ = nullptr;
    ioThread_ = nullptr;
    stopRequested_ = false;
    if (completedIo != nullptr) {
        Q_ASSERT(completedIo->thread() == thread());
        delete completedIo;
    }
    delete oldThread;
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    activeCapabilityRequestId_.clear();
    appIdentity_.clear();
    invalidateCapabilityBinding();

    if (pendingSession_ != nullptr) {
        std::unique_ptr<IpcSession> replacement = std::move(pendingSession_);
        std::optional<CapabilityBinding> replacementBinding =
            std::move(pendingCapabilityBinding_);
        pendingCapabilityBinding_.reset();
        state_ = HostWorkerSessionState::Detached;
        if (!startSession(std::move(replacement),
                          std::move(replacementBinding))) {
            state_ = HostWorkerSessionState::Failed;
            lastErrorCode_ = QStringLiteral("host.worker_session.reattach_failed");
            emit failed(lastErrorCode_, generation_);
        }
        return;
    }

    state_ = cleanupFinalState_;
    emit sessionDetached(generation_);
}

void HostWorkerSessionController::stopIoThreadForDestruction()
{
    Q_ASSERT(QThread::currentThread() == thread());
    pendingSession_.reset();
    pendingCapabilityBinding_.reset();
    invalidateCapabilityBinding();
    HostWorkerSessionIo *const oldIo = io_.data();
    QThread *const oldThread = ioThread_;
    const quint64 oldGeneration = generation_;
    if (oldThread == nullptr) return;

    disconnect(oldThread, nullptr, this, nullptr);
    if (oldIo != nullptr) {
        disconnect(oldIo, nullptr, this, nullptr);
        connect(oldThread, &QThread::finished, oldIo, &QObject::deleteLater,
                Qt::QueuedConnection);
        if (oldThread->isRunning() && !stopRequested_
            && !QMetaObject::invokeMethod(
                oldIo, [oldIo, oldGeneration] { oldIo->abort(oldGeneration); },
                Qt::QueuedConnection))
            qFatal("Unable to queue host worker IO destruction");
    } else {
        if (oldThread->isRunning()) oldThread->quit();
    }

    if (!oldThread->isRunning() || oldThread->wait(50)) {
        if (oldIo != nullptr && oldIo->thread() == thread()) delete oldIo;
        delete oldThread;
    } else {
        connect(oldThread, &QThread::finished, oldThread,
                &QObject::deleteLater, Qt::QueuedConnection);
    }
    ioThread_ = nullptr;
    io_ = nullptr;
    stopRequested_ = false;
}
