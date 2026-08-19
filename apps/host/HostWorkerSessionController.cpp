#include "HostWorkerSessionController.h"

#include "MainWindow.h"

#include <QJsonObject>
#include <QTimer>

#include <utility>

HostWorkerSessionController::HostWorkerSessionController(MainWindow *window,
                                                         QObject *parent)
    : QObject(parent), window_(window), pollTimer_(new QTimer(this))
{
    pollTimer_->setInterval(5);
    connect(pollTimer_, &QTimer::timeout,
            this, &HostWorkerSessionController::pollSession);
}

HostWorkerSessionController::~HostWorkerSessionController() = default;

bool HostWorkerSessionController::attach(std::unique_ptr<IpcSession> session)
{
    if (state_ == HostWorkerSessionState::Running || window_ == nullptr
        || session == nullptr || !session->isAuthenticated() || session->isClosed()
        || session->appIdentity().isEmpty()) {
        lastErrorCode_ = QStringLiteral("host.worker_session.invalid_attachment");
        return false;
    }

    pendingRouteLoads_.clear();
    lastErrorCode_.clear();
    session_ = std::move(session);
    state_ = HostWorkerSessionState::Running;
    pollTimer_->start();
    QTimer::singleShot(0, this, &HostWorkerSessionController::pollSession);
    return true;
}

bool HostWorkerSessionController::shutdown(const QString &reason)
{
    if (state_ != HostWorkerSessionState::Running || session_ == nullptr
        || reason.isEmpty()) {
        return false;
    }
    const auto message = ProtocolMessage::shutdown(reason);
    if (!message.has_value() || !session_->send(*message, sendTimeoutMs)) {
        failClosed(session_->lastErrorCode().isEmpty()
                       ? QStringLiteral("host.worker_session.shutdown_send_failed")
                       : session_->lastErrorCode());
        return false;
    }
    pollTimer_->stop();
    session_->close();
    session_.reset();
    pendingRouteLoads_.clear();
    state_ = HostWorkerSessionState::Detached;
    return true;
}

HostWorkerSessionState HostWorkerSessionController::state() const noexcept
{
    return state_;
}

QString HostWorkerSessionController::lastErrorCode() const
{
    return lastErrorCode_;
}

qsizetype HostWorkerSessionController::pendingRouteLoadCount() const noexcept
{
    return pendingRouteLoads_.size();
}

void HostWorkerSessionController::pollSession()
{
    if (state_ != HostWorkerSessionState::Running || session_ == nullptr) {
        return;
    }

    constexpr int maximumMessagesPerTurn = 32;
    for (int index = 0; index < maximumMessagesPerTurn; ++index) {
        const SessionReceiveResult received = session_->poll(0);
        if (received.status == SessionStatus::TimedOut && !session_->isClosed()) {
            return;
        }
        if (received.status != SessionStatus::MessageReady
            || !received.message.has_value()) {
            failClosed(received.errorCode.isEmpty()
                           ? QStringLiteral("host.worker_session.receive_failed")
                           : received.errorCode);
            return;
        }
        handleMessage(*received.message);
        if (state_ != HostWorkerSessionState::Running) {
            return;
        }
    }
}

void HostWorkerSessionController::handleMessage(const ProtocolMessage &message)
{
    switch (message.type()) {
    case ProtocolType::NavigationRequest:
        handleNavigationRequest(message);
        break;
    case ProtocolType::Response:
        handleRouteLoadResponse(message);
        break;
    case ProtocolType::Heartbeat:
    case ProtocolType::Ready:
    case ProtocolType::SurfaceReady:
    case ProtocolType::StructuredLog:
        break;
    case ProtocolType::Shutdown:
        failClosed(QStringLiteral("host.worker_session.peer_shutdown"));
        break;
    case ProtocolType::Request: {
        const auto response = ProtocolMessage::errorResponse(
            message.requestId(), QStringLiteral("capability.unhandled"),
            QStringLiteral("No capability handler is attached."));
        if (!response.has_value() || !session_->send(*response, sendTimeoutMs)) {
            failClosed(session_->lastErrorCode().isEmpty()
                           ? QStringLiteral("host.worker_session.response_send_failed")
                           : session_->lastErrorCode());
        }
        break;
    }
    case ProtocolType::Handshake:
    case ProtocolType::HandshakeAck:
    case ProtocolType::RouteLoad:
        failClosed(QStringLiteral("host.worker_session.unexpected_message"));
        break;
    }
}

void HostWorkerSessionController::handleNavigationRequest(
    const ProtocolMessage &message)
{
    const QString route = message.payload().value(QStringLiteral("route")).toString();
    if (!pendingRouteLoads_.isEmpty()) {
        const auto response = ProtocolMessage::errorResponse(
            message.requestId(), QStringLiteral("navigation.busy"),
            QStringLiteral("A route load is already pending."));
        if (!response.has_value() || !session_->send(*response, sendTimeoutMs)) {
            failClosed(session_->lastErrorCode().isEmpty()
                           ? QStringLiteral("host.worker_session.response_send_failed")
                           : session_->lastErrorCode());
        }
        return;
    }

    if (!window_->navigateFromWorker(session_->appIdentity(), route)) {
        const auto response = ProtocolMessage::errorResponse(
            message.requestId(), QStringLiteral("navigation.denied"),
            QStringLiteral("The requested route is not assigned to this worker."));
        if (!response.has_value() || !session_->send(*response, sendTimeoutMs)) {
            failClosed(session_->lastErrorCode().isEmpty()
                           ? QStringLiteral("host.worker_session.response_send_failed")
                           : session_->lastErrorCode());
        }
        return;
    }

    const auto response = ProtocolMessage::successResponse(
        message.requestId(), QJsonObject{{QStringLiteral("route"), route}});
    if (!response.has_value() || !session_->send(*response, sendTimeoutMs)) {
        failClosed(session_->lastErrorCode().isEmpty()
                       ? QStringLiteral("host.worker_session.response_send_failed")
                       : session_->lastErrorCode());
        return;
    }

    const QString routeLoadId = QStringLiteral("host-route-%1")
                                    .arg(++nextRouteLoadId_);
    if (!session_->sendRouteLoad(routeLoadId, route, routeLoadTimeoutMs)) {
        failClosed(session_->lastErrorCode().isEmpty()
                       ? QStringLiteral("host.worker_session.route_load_send_failed")
                       : session_->lastErrorCode());
        return;
    }
    pendingRouteLoads_.insert(routeLoadId, route);
}

void HostWorkerSessionController::handleRouteLoadResponse(
    const ProtocolMessage &message)
{
    const auto iterator = pendingRouteLoads_.find(message.requestId());
    if (iterator == pendingRouteLoads_.end()) {
        failClosed(QStringLiteral("host.worker_session.unexpected_response"));
        return;
    }
    const QJsonObject payload = message.payload();
    const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
    if (!payload.value(QStringLiteral("ok")).toBool(false)
        || result.value(QStringLiteral("route")).toString() != iterator.value()) {
        failClosed(QStringLiteral("host.worker_session.route_load_rejected"));
        return;
    }
    pendingRouteLoads_.erase(iterator);
}

void HostWorkerSessionController::failClosed(const QString &errorCode)
{
    if (state_ == HostWorkerSessionState::Failed) {
        return;
    }
    pollTimer_->stop();
    if (session_ != nullptr) {
        session_->close();
    }
    pendingRouteLoads_.clear();
    state_ = HostWorkerSessionState::Failed;
    lastErrorCode_ = errorCode.isEmpty()
                         ? QStringLiteral("host.worker_session.failed")
                         : errorCode;
    emit failed(lastErrorCode_);
}
