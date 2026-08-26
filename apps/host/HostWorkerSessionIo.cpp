#include "HostWorkerSessionIo.h"
#include "HostWorkerSessionTestHooks.h"

#include <QJsonObject>
#include <QThread>
#include <QTimer>

#include <utility>

HostWorkerSessionIo::HostWorkerSessionIo(std::unique_ptr<IpcSession> session,
                                         const quint64 generation,
                                         QThread *const ownerThread)
    : session_(std::move(session))
    , pollTimer_(new QTimer(this))
    , generation_(generation)
    , ownerThread_(ownerThread)
{
    pollTimer_->setInterval(2);
    connect(pollTimer_, &QTimer::timeout, this,
            &HostWorkerSessionIo::pollSession);
}

void HostWorkerSessionIo::start()
{
    if (terminal_ || session_ == nullptr) {
        return;
    }
    pollTimer_->start();
    pollSession();
}

void HostWorkerSessionIo::sendMessage(const quint64 generation,
                                      const quint64 commandId,
                                      const ProtocolMessage &message,
                                      const bool trackedRouteLoad,
                                      const QString &route)
{
    if (generation != generation_ || terminal_ || stopping_
        || session_ == nullptr) {
        emit commandFinished(generation, commandId, false,
                             QStringLiteral("host.worker_session.stale_command"));
        return;
    }
    const bool sent = trackedRouteLoad
        ? session_->sendRouteLoad(message.requestId(), route, routeLoadTimeoutMs)
        : session_->send(message, sendTimeoutMs);
    const QString errorCode = sent ? QString{} : session_->lastErrorCode();
    emit commandFinished(generation, commandId, sent, errorCode);
    if (!sent) {
        fail(errorCode.isEmpty()
                 ? QStringLiteral("host.worker_session.send_failed")
                 : errorCode);
    }
}

void HostWorkerSessionIo::resumePolling(const quint64 generation)
{
    if (generation != generation_ || terminal_ || stopping_) {
        return;
    }
    awaitingGui_ = false;
    if (!pollTimer_->isActive()) {
        pollTimer_->start();
    }
    pollSession();
}

void HostWorkerSessionIo::beginShutdown(const quint64 generation,
                                        const QString &reason)
{
    if (generation != generation_ || terminal_ || session_ == nullptr) {
        return;
    }
    stopping_ = true;
    awaitingGui_ = false;
    const auto message = ProtocolMessage::shutdown(reason);
    if (!message.has_value() || !session_->send(*message, sendTimeoutMs)) {
        fail(session_->lastErrorCode().isEmpty()
                 ? QStringLiteral("host.worker_session.shutdown_send_failed")
                 : session_->lastErrorCode());
        return;
    }
    pollTimer_->start();
}

void HostWorkerSessionIo::abort(const quint64 generation)
{
    if (generation != generation_) return;
    if (!terminal_) {
        terminal_ = true;
        pollTimer_->stop();
        if (session_ != nullptr) session_->close();
    }
    QThread *const ioThread = QThread::currentThread();
    if (!transferToOwnerThread())
        qFatal("Host worker IO ownership transfer failed");
#ifdef Q_BROWSER_HOST_TESTING
    qbrowser_host_testing::runBeforeIoThreadQuitHook(generation_);
#endif
    ioThread->quit();
}

bool HostWorkerSessionIo::transferToOwnerThread()
{
    QThread *const current = QThread::currentThread();
    return ownerThread_ != nullptr && thread() == current
        && moveToThread(ownerThread_);
}

void HostWorkerSessionIo::pollSession()
{
    if (terminal_ || awaitingGui_ || session_ == nullptr) {
        return;
    }
    for (int index = 0; index < maximumMessagesPerTurn; ++index) {
        const SessionReceiveResult received = session_->poll(0);
        if (received.status == SessionStatus::TimedOut && !session_->isClosed()) {
            return;
        }
        if (received.status != SessionStatus::MessageReady
            || !received.message.has_value()) {
            fail(received.errorCode.isEmpty()
                     ? QStringLiteral("host.worker_session.receive_failed")
                     : received.errorCode);
            return;
        }
        const ProtocolMessage &message = *received.message;
        switch (message.type()) {
        case ProtocolType::NavigationRequest:
            awaitingGui_ = true;
            pollTimer_->stop();
            emit navigationRequested(generation_, message.requestId(),
                                     message.payload().value(QStringLiteral("route")).toString());
            return;
        case ProtocolType::Response:
            awaitingGui_ = true;
            pollTimer_->stop();
            emit routeLoadResponse(generation_, message.requestId(), message.payload());
            return;
        case ProtocolType::Request: {
            const QJsonObject requestPayload = message.payload();
            emit capabilityRequested(
                generation_, message.requestId(),
                requestPayload.value(QStringLiteral("capability")).toString(),
                requestPayload.value(QStringLiteral("operation")).toString(),
                requestPayload.value(QStringLiteral("payload")).toObject());
            break;
        }
        case ProtocolType::Shutdown:
            if (!stopping_) {
                fail(QStringLiteral("host.worker_session.peer_shutdown"));
                return;
            }
            terminal_ = true;
            pollTimer_->stop();
            session_->close();
            emit shutdownFinished(generation_);
            return;
        case ProtocolType::Heartbeat:
            emit heartbeatObserved(generation_);
            break;
        case ProtocolType::PageMetadata:
            emit pageMetadataReceived(
                generation_,
                message.payload().value(QStringLiteral("title")).toString(),
                message.payload().value(QStringLiteral("status")).toString());
            break;
        case ProtocolType::Ready:
        case ProtocolType::SurfaceReady:
        case ProtocolType::StructuredLog:
            break;
        case ProtocolType::Handshake:
        case ProtocolType::HandshakeAck:
        case ProtocolType::RouteLoad:
            fail(QStringLiteral("host.worker_session.unexpected_message"));
            return;
        }
    }
}

void HostWorkerSessionIo::fail(const QString &errorCode)
{
    if (terminal_) {
        return;
    }
    terminal_ = true;
    pollTimer_->stop();
    if (session_ != nullptr) {
        session_->close();
    }
    emit sessionFailed(generation_, errorCode.isEmpty()
                                        ? QStringLiteral("host.worker_session.failed")
                                        : errorCode);
}
