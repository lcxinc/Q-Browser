#include "HostWorkerSessionIo.h"
#include "HostWorkerSessionTestHooks.h"

#include <QJsonObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <utility>

struct HostWorkerSessionBindingState final
{
    HostWorkerSessionBindingState(
        const quint64 boundGeneration,
        std::optional<TabCapabilityAuthority> boundAuthority)
        : generation(boundGeneration),
          capabilityAuthority(std::move(boundAuthority))
    {
    }

    const quint64 generation;
    const std::optional<TabCapabilityAuthority> capabilityAuthority;
    std::atomic_bool active{true};
};

HostWorkerSessionIo::HostWorkerSessionIo(std::unique_ptr<IpcSession> session,
                                         const quint64 generation,
                                         QThread *const ownerThread,
                                         std::optional<TabCapabilityAuthority>
                                             capabilityAuthority)
    : session_(std::move(session))
    , pollTimer_(new QTimer(this))
    , sendDeadlineTimer_(new QTimer(this))
    , shutdownDeadlineTimer_(new QTimer(this))
    , generation_(generation)
    , ownerThread_(ownerThread)
    , capabilityAuthority_(std::move(capabilityAuthority))
    , liveBinding_(std::make_shared<HostWorkerSessionBindingState>(
          generation_, capabilityAuthority_))
{
    pollTimer_->setInterval(2);
    connect(pollTimer_, &QTimer::timeout, this,
            &HostWorkerSessionIo::pollSession);
    sendDeadlineTimer_->setSingleShot(true);
    connect(sendDeadlineTimer_, &QTimer::timeout, this, [this] {
        if (terminal_ || pendingCommandId_ == 0) return;
        pendingCommandId_ = 0;
        PipeWriteCancellation cancellation = commandCancellation_;
        commandCancellation_ = {};
        (void)cancellation.cancel();
        fail(QStringLiteral("host.worker_session.send_timeout"));
    });
    shutdownDeadlineTimer_->setSingleShot(true);
    connect(shutdownDeadlineTimer_, &QTimer::timeout, this, [this] {
        if (terminal_ || !stopping_) return;
        PipeWriteCancellation cancellation = shutdownCancellation_;
        shutdownCancellation_ = {};
        (void)cancellation.cancel();
        fail(QStringLiteral("host.worker_session.shutdown_timeout"));
    });
}

HostWorkerSessionIo::~HostWorkerSessionIo()
{
    invalidateLiveBinding();
    if (session_ != nullptr) session_->close();
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
                                      const QString &route,
                                      std::optional<TabCapabilityAuthority>
                                          capabilityAuthority,
                                      std::shared_ptr<
                                          AuthorityAdmissionToken::UseGuard>
                                          capabilityUse)
{
    if (generation != generation_ || terminal_ || stopping_
        || session_ == nullptr) {
        emit commandFinished(generation, commandId, false, false,
                             QStringLiteral("host.worker_session.stale_command"));
        return;
    }
    const bool capabilitySend = capabilityAuthority.has_value()
        || capabilityUse != nullptr;
    if (capabilitySend
        && (!capabilityAuthority.has_value() || capabilityUse == nullptr
            || !capabilityAuthority_.has_value()
            || *capabilityAuthority != *capabilityAuthority_)) {
        const QString errorCode = QStringLiteral(
            "host.worker_session.capability_authority_mismatch");
        emit commandFinished(generation, commandId, false, false, errorCode);
        fail(errorCode);
        return;
    }

    IpcSendWork work;
    if (capabilitySend) {
        const TabCapabilityAuthority immutableAuthority =
            *capabilityAuthority;
        const quint64 immutableGeneration = generation;
        const std::shared_ptr<HostWorkerSessionBindingState> liveBinding =
            liveBinding_;
        work.publicationGate =
            [immutableAuthority, immutableGeneration, liveBinding,
             use = std::move(capabilityUse)] {
                return liveBinding != nullptr && use != nullptr
                    && use->publishIfStillAdmitted(
                        [immutableAuthority, immutableGeneration,
                         liveBinding] {
                            return liveBinding->active.load(
                                       std::memory_order_acquire)
                                && liveBinding->generation
                                    == immutableGeneration
                                && liveBinding->capabilityAuthority.has_value()
                                && *liveBinding->capabilityAuthority
                                    == immutableAuthority;
                        });
            };
    }
    work.completion = [this, generation, commandId, capabilitySend](
                          const PipeWriteResult &result) {
        (void)QMetaObject::invokeMethod(
            this,
            [this, generation, commandId, capabilitySend, result] {
                if (!completeCommandSubmission(commandId)) return;
                if (generation != generation_ || terminal_) return;
                if (result.status == PipeIoStatus::Ok) {
                    emit commandFinished(generation, commandId, true, true,
                                         QString{});
                    return;
                }
                if (result.status == PipeIoStatus::Cancelled
                    && capabilitySend) {
                    emit commandFinished(generation, commandId, true, false,
                                         result.errorCode);
                    return;
                }
                const QString errorCode = result.errorCode.isEmpty()
                    ? QStringLiteral("host.worker_session.send_failed")
                    : result.errorCode;
                emit commandFinished(generation, commandId, false, false,
                                     errorCode);
                fail(errorCode);
            },
            Qt::QueuedConnection);
    };
    IpcSendSubmission submission = trackedRouteLoad
        ? session_->submitRouteLoad(message.requestId(), route,
                                    routeLoadTimeoutMs, std::move(work))
        : session_->submitSend(message, std::move(work));
    if (!submission.accepted) {
        const QString errorCode = submission.errorCode.isEmpty()
            ? session_->lastErrorCode() : submission.errorCode;
        emit commandFinished(generation, commandId, false, false, errorCode);
        fail(errorCode.isEmpty()
                 ? QStringLiteral("host.worker_session.send_failed")
                 : errorCode);
        return;
    }
    armCommandDeadline(commandId, submission.cancellation);
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
    if (generation != generation_ || terminal_ || stopping_
        || session_ == nullptr) {
        return;
    }
    stopping_ = true;
    awaitingGui_ = false;
    invalidateLiveBinding();
    const auto message = ProtocolMessage::shutdown(reason);
    if (!message.has_value()) {
        fail(QStringLiteral("host.worker_session.shutdown_send_failed"));
        return;
    }
    IpcSendSubmission submission = session_->submitSend(
        *message,
        IpcSendWork{
            {},
            [this, generation](const PipeWriteResult &result) {
                (void)QMetaObject::invokeMethod(
                    this,
                    [this, generation, result] {
                        if (generation != generation_ || terminal_
                            || result.status == PipeIoStatus::Ok) {
                            if (generation == generation_)
                                shutdownCancellation_ = {};
                            return;
                        }
                        shutdownCancellation_ = {};
                        fail(result.errorCode.isEmpty()
                                 ? QStringLiteral(
                                       "host.worker_session.shutdown_send_failed")
                                 : result.errorCode);
                    },
                    Qt::QueuedConnection);
            }});
    if (!submission.accepted) {
        fail(session_->lastErrorCode().isEmpty()
                 ? QStringLiteral("host.worker_session.shutdown_send_failed")
                 : session_->lastErrorCode());
        return;
    }
    shutdownCancellation_ = submission.cancellation;
    shutdownDeadlineTimer_->start(shutdownTimeoutMs);
    pollTimer_->start();
}

void HostWorkerSessionIo::abort(const quint64 generation)
{
    if (generation != generation_) return;
    if (!terminal_) {
        terminal_ = true;
        invalidateLiveBinding();
        pollTimer_->stop();
        sendDeadlineTimer_->stop();
        shutdownDeadlineTimer_->stop();
        pendingCommandId_ = 0;
        commandCancellation_ = {};
        shutdownCancellation_ = {};
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
                capabilityAuthority_.value_or(TabCapabilityAuthority{}),
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
            invalidateLiveBinding();
            pollTimer_->stop();
            sendDeadlineTimer_->stop();
            shutdownDeadlineTimer_->stop();
            pendingCommandId_ = 0;
            commandCancellation_ = {};
            shutdownCancellation_ = {};
            session_->close();
            emit shutdownFinished(generation_);
            return;
        case ProtocolType::Heartbeat:
            emit heartbeatObserved(generation_);
            break;
        case ProtocolType::VisibilityChanged:
            fail(QStringLiteral("host.worker_session.unexpected_message"));
            return;
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
    invalidateLiveBinding();
    pollTimer_->stop();
    sendDeadlineTimer_->stop();
    shutdownDeadlineTimer_->stop();
    pendingCommandId_ = 0;
    commandCancellation_ = {};
    shutdownCancellation_ = {};
    if (session_ != nullptr) {
        session_->close();
    }
    emit sessionFailed(generation_, errorCode.isEmpty()
                                        ? QStringLiteral("host.worker_session.failed")
                                        : errorCode);
}

void HostWorkerSessionIo::invalidateLiveBinding() noexcept
{
    if (liveBinding_ != nullptr) {
        liveBinding_->active.store(false, std::memory_order_release);
    }
}

void HostWorkerSessionIo::armCommandDeadline(
    const quint64 commandId, PipeWriteCancellation cancellation)
{
    if (terminal_) return;
    if (pendingCommandId_ != 0) {
        (void)cancellation.cancel();
        fail(QStringLiteral("host.worker_session.concurrent_send"));
        return;
    }
    pendingCommandId_ = commandId;
    commandCancellation_ = std::move(cancellation);
    sendDeadlineTimer_->start(sendTimeoutMs);
}

bool HostWorkerSessionIo::completeCommandSubmission(const quint64 commandId)
{
    if (pendingCommandId_ != commandId) return false;
    pendingCommandId_ = 0;
    commandCancellation_ = {};
    sendDeadlineTimer_->stop();
    return true;
}
