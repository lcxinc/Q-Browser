#include "IpcSession.h"

#include <QScopedValueRollback>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>

struct IpcPendingRequestState final
{
    enum class Phase { Reserved, Published, Terminal };

    std::atomic<qint64> deadlineMonotonicMs{0};
    std::atomic<Phase> phase{Phase::Reserved};
};

namespace {

qint64 steadyMonotonicMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class ArmableSendCompletion final
{
public:
    explicit ArmableSendCompletion(
        std::function<void(const PipeWriteResult &)> callback)
        : callback_(std::move(callback))
    {
    }

    void complete(const PipeWriteResult &result) noexcept
    {
        std::function<void(const PipeWriteResult &)> callback;
        {
            const std::lock_guard lock(mutex_);
            if (delivered_) return;
            if (!armed_) {
                result_ = result;
                return;
            }
            delivered_ = true;
            callback = std::move(callback_);
        }
        invoke(callback, result);
    }

    void arm() noexcept
    {
        std::function<void(const PipeWriteResult &)> callback;
        std::optional<PipeWriteResult> result;
        {
            const std::lock_guard lock(mutex_);
            if (armed_) return;
            armed_ = true;
            if (!result_.has_value()) return;
            delivered_ = true;
            result = std::move(result_);
            callback = std::move(callback_);
        }
        invoke(callback, *result);
    }

private:
    static void invoke(
        const std::function<void(const PipeWriteResult &)> &callback,
        const PipeWriteResult &result) noexcept
    {
        if (!callback) return;
        try {
            callback(result);
        } catch (...) {
        }
    }

    std::mutex mutex_;
    std::function<void(const PipeWriteResult &)> callback_;
    std::optional<PipeWriteResult> result_;
    bool armed_ = false;
    bool delivered_ = false;
};

bool outgoingTypeAllowed(const IpcRole role, const ProtocolType type)
{
    if (role == IpcRole::Host) {
        return type == ProtocolType::HandshakeAck || type == ProtocolType::RouteLoad
            || type == ProtocolType::Response || type == ProtocolType::Heartbeat
            || type == ProtocolType::VisibilityChanged || type == ProtocolType::Shutdown;
    }
    return type == ProtocolType::Handshake || type == ProtocolType::SurfaceReady
        || type == ProtocolType::Ready || type == ProtocolType::Request
        || type == ProtocolType::NavigationRequest
        || type == ProtocolType::PageMetadata
        || type == ProtocolType::Response || type == ProtocolType::Heartbeat
        || type == ProtocolType::StructuredLog || type == ProtocolType::Shutdown;
}

bool incomingTypeAllowed(const IpcRole role, const ProtocolType type)
{
    if (role == IpcRole::Host) {
        return outgoingTypeAllowed(IpcRole::Worker, type);
    }
    return outgoingTypeAllowed(IpcRole::Host, type);
}

} // namespace

IpcSession::IpcSession(WinPipeTransport transport,
                       const IpcRole role,
                       HostLaunchContext hostContext)
    : transport_(std::move(transport)), role_(role), hostContext_(std::move(hostContext))
{
    clock_.start();
    if (!transport_.isValid()
        || (role_ == IpcRole::Host
            && (hostContext_.nonce.isEmpty() || hostContext_.appIdentity.isEmpty()))) {
        closed_ = true;
        lastErrorCode_ = QStringLiteral("ipc.session.invalid_context");
        transport_.close();
    }
}

bool IpcSession::send(const ProtocolMessage &message, const int timeoutMs)
{
    return sendInternal(message, timeoutMs, false);
}

IpcSendSubmission IpcSession::submitSend(const ProtocolMessage &message,
                                         IpcSendWork work)
{
    return submitInternal(message, std::move(work), false);
}

IpcSendSubmission IpcSession::submitInternal(const ProtocolMessage &message,
                                             IpcSendWork work,
                                             const bool allowTrackedMessage,
                                             std::function<void(
                                                 const PipeWriteResult &)>
                                                 terminalObserver,
                                             std::function<void()>
                                                 rejectionObserver)
{
    std::optional<QByteArray> frame = prepareSend(message, allowTrackedMessage);
    if (!frame.has_value()) {
        if (rejectionObserver) rejectionObserver();
        return {false, lastErrorCode_, {}};
    }

    const auto completion = std::make_shared<ArmableSendCompletion>(
        std::move(work.completion));

    PipeWriteSubmission submission = transport_.submitWrite(PipeWriteWork{
        std::move(*frame), std::move(work.publicationGate),
        [completion, observer = std::move(terminalObserver)](
            const PipeWriteResult &result) {
            if (observer) observer(result);
            completion->complete(result);
        }});
    if (!submission.accepted) {
        if (rejectionObserver) rejectionObserver();
        lastErrorCode_ = submission.errorCode;
        return submission;
    }
    noteAcceptedSend(message);
    // This is deliberately the final operation which can invoke user code.
    // A tiny write may have completed on the transport thread before
    // submitWrite returned, but session bookkeeping is now committed first.
    completion->arm();
    return submission;
}

bool IpcSession::sendInternal(const ProtocolMessage &message,
                              const int timeoutMs,
                              const bool allowTrackedMessage)
{
    const std::optional<QByteArray> frame =
        prepareSend(message, allowTrackedMessage);
    if (!frame.has_value()) return false;
    if (!transport_.writeAll(*frame, timeoutMs)) {
        const PipeIoStatus status = transport_.lastStatus();
        if (status == PipeIoStatus::TimedOut) {
            fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.timeout"));
        } else if (status == PipeIoStatus::PeerClosed) {
            fail(SessionStatus::PeerClosed, QStringLiteral("ipc.session.peer_closed"));
        } else {
            fail(SessionStatus::Failed, QStringLiteral("ipc.session.write_failed"));
        }
        return false;
    }
    noteAcceptedSend(message);
    return true;
}

std::optional<QByteArray> IpcSession::prepareSend(
    const ProtocolMessage &message,
    const bool allowTrackedMessage)
{
    if (closed_) return std::nullopt;
    if (!allowTrackedMessage
        && (message.type() == ProtocolType::Request
            || message.type() == ProtocolType::RouteLoad
            || message.type() == ProtocolType::NavigationRequest)) {
        lastErrorCode_ = QStringLiteral("ipc.session.tracking_required");
        return std::nullopt;
    }
    if (!authenticated_) {
        if (role_ != IpcRole::Worker || message.type() != ProtocolType::Handshake
            || !outboundNonce_.isEmpty()) {
            lastErrorCode_ = QStringLiteral("ipc.session.authentication_required");
            return std::nullopt;
        }
    } else if (!outgoingTypeAllowed(role_, message.type())) {
        lastErrorCode_ = QStringLiteral("ipc.session.unexpected_message_direction");
        return std::nullopt;
    }
    if (role_ == IpcRole::Worker && message.type() == ProtocolType::PageMetadata
        && !readySent_) {
        lastErrorCode_ = QStringLiteral("ipc.session.ready_required");
        return std::nullopt;
    }
    const ProtocolParseResult validated = ProtocolMessage::parse(message.toJson());
    if (!validated.message.has_value()) {
        lastErrorCode_ = validated.errorCode;
        return std::nullopt;
    }
    QByteArray frame = FrameCodec::encode(message.toJson());
    if (frame.isEmpty()) {
        lastErrorCode_ = QStringLiteral("ipc.session.frame_encode_failed");
        return std::nullopt;
    }
    return frame;
}

void IpcSession::noteAcceptedSend(const ProtocolMessage &message)
{
    if (role_ == IpcRole::Worker && message.type() == ProtocolType::Handshake) {
        outboundNonce_ = message.payload().value(QStringLiteral("nonce")).toString();
    } else if (role_ == IpcRole::Worker && message.type() == ProtocolType::Ready) {
        readySent_ = true;
    }
}

std::shared_ptr<IpcPendingRequestState>
IpcSession::reservePendingRequest(const QString &requestId)
{
    pruneTerminalPendingRequests();
    if (pendingRequests_.contains(requestId)) {
        lastErrorCode_ = QStringLiteral("ipc.session.duplicate_request_id");
        return {};
    }
    constexpr qsizetype maximumPendingRequests = 1024;
    if (pendingRequests_.size() >= maximumPendingRequests) {
        lastErrorCode_ = QStringLiteral("ipc.session.pending_request_limit");
        return {};
    }
    auto reservation = std::make_shared<IpcPendingRequestState>();
    pendingRequests_.insert(requestId, reservation);
    return reservation;
}

void IpcSession::removePendingRequest(
    const QString &requestId,
    const std::shared_ptr<IpcPendingRequestState> &expected)
{
    const auto iterator = pendingRequests_.find(requestId);
    if (iterator != pendingRequests_.end() && iterator.value() == expected) {
        pendingRequests_.erase(iterator);
    }
}

void IpcSession::pruneTerminalPendingRequests()
{
    for (auto iterator = pendingRequests_.begin();
         iterator != pendingRequests_.end();) {
        const std::shared_ptr<IpcPendingRequestState> &request =
            iterator.value();
        if (request == nullptr
            || request->phase.load(std::memory_order_acquire)
                == IpcPendingRequestState::Phase::Terminal) {
            iterator = pendingRequests_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

bool IpcSession::sendRequest(const QString &requestId,
                            const QString &capability,
                            const QString &operation,
                            const QJsonObject &payload,
                            const int timeoutMs)
{
    return sendTracked(requestId,
                       ProtocolMessage::request(requestId, capability, operation, payload),
                       timeoutMs);
}

bool IpcSession::sendRouteLoad(const QString &requestId,
                               const QString &route,
                               const int timeoutMs)
{
    return sendTracked(requestId, ProtocolMessage::routeLoad(requestId, route), timeoutMs);
}

IpcSendSubmission IpcSession::submitRouteLoad(const QString &requestId,
                                              const QString &route,
                                              const int responseTimeoutMs,
                                              IpcSendWork work)
{
    if (closed_) return {false, lastErrorCode_, {}};
    if (responseTimeoutMs < 0) {
        lastErrorCode_ = QStringLiteral("ipc.session.invalid_timeout");
        return {false, lastErrorCode_, {}};
    }
    const std::shared_ptr<IpcPendingRequestState> reservation =
        reservePendingRequest(requestId);
    if (reservation == nullptr) return {false, lastErrorCode_, {}};
    const auto message = ProtocolMessage::routeLoad(requestId, route);
    if (!message.has_value()) {
        removePendingRequest(requestId, reservation);
        lastErrorCode_ = QStringLiteral("ipc.protocol.invalid_payload");
        return {false, lastErrorCode_, {}};
    }

    return submitInternal(
        *message, std::move(work), true,
        [reservation, responseTimeoutMs](const PipeWriteResult &result) {
            if (result.status != PipeIoStatus::Ok) {
                reservation->phase.store(
                    IpcPendingRequestState::Phase::Terminal,
                    std::memory_order_release);
                return;
            }
            reservation->deadlineMonotonicMs.store(
                steadyMonotonicMs() + responseTimeoutMs,
                std::memory_order_relaxed);
            reservation->phase.store(
                IpcPendingRequestState::Phase::Published,
                std::memory_order_release);
        },
        [reservation] {
            reservation->phase.store(
                IpcPendingRequestState::Phase::Terminal,
                std::memory_order_release);
        });
}

bool IpcSession::sendNavigationRequest(const QString &requestId,
                                       const QString &route,
                                       const int timeoutMs)
{
    return sendTracked(requestId,
                       ProtocolMessage::navigationRequest(requestId, route),
                       timeoutMs);
}

bool IpcSession::sendVisibilityChanged(const bool active, const int timeoutMs)
{
    const auto message = ProtocolMessage::visibilityChanged(active);
    if (!message.has_value()) {
        lastErrorCode_ = QStringLiteral("ipc.protocol.invalid_payload");
        return false;
    }
    return send(*message, timeoutMs);
}

SessionReceiveResult IpcSession::receive(const int timeoutMs)
{
    return receiveImpl(timeoutMs, true);
}

SessionReceiveResult IpcSession::poll(const int timeoutMs)
{
    return receiveImpl(timeoutMs, false);
}

void IpcSession::setPageMetadataHandler(PageMetadataHandler handler)
{
    pageMetadataHandler_ = std::move(handler);
    deliverQueuedPageMetadata();
}

void IpcSession::deliverQueuedPageMetadata()
{
    if (role_ != IpcRole::Host || closed_ || deliveringPageMetadata_
        || !pageMetadataHandler_) {
        return;
    }
    QScopedValueRollback deliveryGuard(deliveringPageMetadata_, true);
    for (;;) {
        if (closed_ || !pageMetadataHandler_) return;
        qsizetype pendingIndex = -1;
        for (qsizetype index = 0; index < receivedMessages_.size(); ++index) {
            const QueuedMessage &queued = receivedMessages_.at(index);
            if (queued.message.type() == ProtocolType::PageMetadata
                && !queued.pageMetadataDelivered) {
                pendingIndex = index;
                break;
            }
        }
        if (pendingIndex < 0) return;

        QueuedMessage &queued = receivedMessages_[pendingIndex];
        queued.pageMetadataDelivered = true;
        const QJsonObject payload = queued.message.payload();
        const QString title = payload.value(QStringLiteral("title")).toString();
        const QString status = payload.value(QStringLiteral("status")).toString();
        const PageMetadataHandler handler = pageMetadataHandler_;
        if (!handler) return;
        handler(title, status);
    }
}

SessionReceiveResult IpcSession::receiveImpl(const int timeoutMs,
                                             const bool closeOnCallerTimeout)
{
    if (closed_) {
        return {SessionStatus::Failed, std::nullopt, lastErrorCode_};
    }
    if (timeoutMs < 0) {
        return fail(SessionStatus::Failed, QStringLiteral("ipc.session.invalid_timeout"));
    }
    if (pendingRequestExpired()) {
        return fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.request_timeout"));
    }
    if (!receivedMessages_.isEmpty()) {
        QueuedMessage queued = receivedMessages_.dequeue();
        return {SessionStatus::MessageReady, std::move(queued.message), {}};
    }

    QElapsedTimer receiveTimer;
    receiveTimer.start();
    for (;;) {
        const qint64 now = steadyMonotonicMs();
        const int callerRemaining = std::max(
            0, timeoutMs - static_cast<int>(receiveTimer.elapsed()));
        int remaining = callerRemaining;
        bool publicationPending = false;
        if (const auto requestDeadline =
                nearestPendingDeadline(publicationPending);
            requestDeadline.has_value()) {
            const qint64 requestRemaining = *requestDeadline - now;
            if (requestRemaining <= 0) {
                return fail(SessionStatus::TimedOut,
                            QStringLiteral("ipc.session.request_timeout"));
            }
            remaining = std::min(remaining, static_cast<int>(std::min<qint64>(
                                                requestRemaining,
                                                std::numeric_limits<int>::max())));
        }
        if (publicationPending) {
            // Publication and its response deadline are armed by the writer
            // thread. Re-sample briefly so a route published after this loop
            // entered readSome cannot inherit the caller's much longer wait.
            constexpr int publicationStatePollMs = 5;
            remaining = std::min(remaining, publicationStatePollMs);
        }
        const PipeReadResult read = transport_.readSome(64 * 1024, remaining);
        if (read.status == PipeIoStatus::TimedOut) {
            if (pendingRequestExpired()) {
                return fail(SessionStatus::TimedOut,
                            QStringLiteral("ipc.session.request_timeout"));
            }
            if (receiveTimer.elapsed() < timeoutMs) continue;
            if (closeOnCallerTimeout) {
                return fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.timeout"));
            }
            return {SessionStatus::TimedOut, std::nullopt,
                    QStringLiteral("ipc.session.timeout")};
        }
        if (read.status == PipeIoStatus::PeerClosed) {
            return fail(SessionStatus::PeerClosed, QStringLiteral("ipc.session.peer_closed"));
        }
        if (read.status != PipeIoStatus::Ok || read.bytes.isEmpty()) {
            return fail(SessionStatus::Failed, QStringLiteral("ipc.session.read_failed"));
        }

        const FrameFeedResult frames = codec_.feed(read.bytes);
        if (frames.status == FrameStatus::Failed) {
            return fail(SessionStatus::Failed, frames.errorCode);
        }
        for (const QJsonObject &frame : frames.frames) {
            const SessionReceiveResult processed = processFrame(frame);
            if (processed.status != SessionStatus::MessageReady) {
                return processed;
            }
            receivedMessages_.enqueue(QueuedMessage{*processed.message});
            deliverQueuedPageMetadata();
            if (closed_) {
                return {SessionStatus::Failed, std::nullopt, lastErrorCode_};
            }
        }
        if (!receivedMessages_.isEmpty()) {
            QueuedMessage queued = receivedMessages_.dequeue();
            return {SessionStatus::MessageReady, std::move(queued.message), {}};
        }
        if (receiveTimer.elapsed() >= timeoutMs) {
            if (closeOnCallerTimeout) {
                return fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.timeout"));
            }
            return {SessionStatus::TimedOut, std::nullopt,
                    QStringLiteral("ipc.session.timeout")};
        }
    }
}

bool IpcSession::isAuthenticated() const noexcept
{
    return authenticated_;
}

QString IpcSession::appIdentity() const
{
    return role_ == IpcRole::Host ? hostContext_.appIdentity : peerAssignedIdentity_;
}

bool IpcSession::isClosed() const noexcept
{
    return closed_;
}

QString IpcSession::lastErrorCode() const
{
    return lastErrorCode_;
}

qsizetype IpcSession::pendingRequestCount() const noexcept
{
    qsizetype active = 0;
    for (const std::shared_ptr<IpcPendingRequestState> &request :
         pendingRequests_) {
        if (request != nullptr
            && request->phase.load(std::memory_order_acquire)
                != IpcPendingRequestState::Phase::Terminal) {
            ++active;
        }
    }
    return active;
}

qint64 IpcSession::lastPeerActivityMonotonicMs() const noexcept
{
    return lastPeerActivityMs_;
}

void IpcSession::close() noexcept
{
    closed_ = true;
    transport_.close();
    receivedMessages_.clear();
    pendingRequests_.clear();
    pageMetadataHandler_ = {};
}

SessionReceiveResult IpcSession::processFrame(const QJsonObject &object)
{
    const ProtocolParseResult parsed = ProtocolMessage::parse(object);
    if (!parsed.message.has_value()) {
        return fail(SessionStatus::Failed, parsed.errorCode);
    }

    const ProtocolMessage &message = *parsed.message;
    bool authenticationMessage = false;
    if (role_ == IpcRole::Host && !authenticated_) {
        if (message.type() != ProtocolType::Handshake) {
            return fail(SessionStatus::Failed,
                        QStringLiteral("ipc.session.handshake_required"));
        }
        if (message.payload().value(QStringLiteral("nonce")).toString()
            != hostContext_.nonce) {
            return fail(SessionStatus::Failed, QStringLiteral("ipc.session.nonce_mismatch"));
        }
        authenticated_ = true;
        authenticationMessage = true;
        const auto acknowledgement = ProtocolMessage::handshakeAck(hostContext_.nonce,
                                                                   hostContext_.appIdentity);
        if (!acknowledgement.has_value() || !send(*acknowledgement)) {
            return fail(SessionStatus::Failed,
                        lastErrorCode_.isEmpty() ? QStringLiteral("ipc.session.ack_failed")
                                                 : lastErrorCode_);
        }
    } else if (role_ == IpcRole::Worker && !authenticated_) {
        if (message.type() != ProtocolType::HandshakeAck || outboundNonce_.isEmpty()) {
            return fail(SessionStatus::Failed,
                        QStringLiteral("ipc.session.handshake_ack_required"));
        }
        if (message.payload().value(QStringLiteral("nonce")).toString()
            != outboundNonce_) {
            return fail(SessionStatus::Failed,
                        QStringLiteral("ipc.session.nonce_mismatch"));
        }
        peerAssignedIdentity_ = message.payload()
                                    .value(QStringLiteral("appIdentity"))
                                    .toString();
        authenticated_ = true;
        authenticationMessage = true;
    } else if (message.type() == ProtocolType::Handshake
               || message.type() == ProtocolType::HandshakeAck) {
        return fail(SessionStatus::Failed, QStringLiteral("ipc.session.unexpected_handshake"));
    }

    if (!authenticationMessage && !incomingTypeAllowed(role_, message.type())) {
        return fail(SessionStatus::Failed,
                    QStringLiteral("ipc.session.unexpected_message_direction"));
    }

    if (role_ == IpcRole::Host && message.type() == ProtocolType::PageMetadata
        && !peerReady_) {
        return fail(SessionStatus::Failed, QStringLiteral("ipc.session.ready_required"));
    }
    if (role_ == IpcRole::Host && message.type() == ProtocolType::Ready) {
        peerReady_ = true;
    }

    if (message.type() == ProtocolType::Request || message.type() == ProtocolType::RouteLoad
        || message.type() == ProtocolType::NavigationRequest) {
        if (receivedRequestIds_.contains(message.requestId())) {
            return fail(SessionStatus::Failed,
                        QStringLiteral("ipc.session.duplicate_request_id"));
        }
        constexpr qsizetype maximumRememberedRequestIds = 4096;
        if (receivedRequestIds_.size() >= maximumRememberedRequestIds) {
            return fail(SessionStatus::Failed,
                        QStringLiteral("ipc.session.request_id_limit"));
        }
        receivedRequestIds_.insert(message.requestId());
    } else if (message.type() == ProtocolType::Response) {
        pruneTerminalPendingRequests();
        const auto pending = pendingRequests_.find(message.requestId());
        if (pending == pendingRequests_.end()) {
            return fail(SessionStatus::Failed, QStringLiteral("ipc.session.unknown_response"));
        }
        pendingRequests_.erase(pending);
    }
    lastPeerActivityMs_ = std::max<qint64>(1, clock_.elapsed());
    return {SessionStatus::MessageReady, message, {}};
}

SessionReceiveResult IpcSession::fail(const SessionStatus status, const QString &code)
{
    lastErrorCode_ = code;
    close();
    return {status, std::nullopt, code};
}

bool IpcSession::pendingRequestExpired()
{
    pruneTerminalPendingRequests();
    const qint64 now = steadyMonotonicMs();
    for (const std::shared_ptr<IpcPendingRequestState> &request :
         pendingRequests_) {
        if (request != nullptr
            && request->phase.load(std::memory_order_acquire)
                == IpcPendingRequestState::Phase::Published
            && request->deadlineMonotonicMs.load(std::memory_order_relaxed)
                <= now) {
            return true;
        }
    }
    return false;
}

std::optional<qint64> IpcSession::nearestPendingDeadline(
    bool &publicationPending) const
{
    publicationPending = false;
    std::optional<qint64> nearest;
    for (const std::shared_ptr<IpcPendingRequestState> &request :
         pendingRequests_) {
        if (request == nullptr) continue;
        const IpcPendingRequestState::Phase phase = request->phase.load(
            std::memory_order_acquire);
        if (phase == IpcPendingRequestState::Phase::Reserved) {
            publicationPending = true;
            continue;
        }
        if (phase != IpcPendingRequestState::Phase::Published) continue;
        const qint64 deadline = request->deadlineMonotonicMs.load(
            std::memory_order_relaxed);
        if (!nearest.has_value() || deadline < *nearest) {
            nearest = deadline;
        }
    }
    return nearest;
}

bool IpcSession::sendTracked(const QString &requestId,
                             const std::optional<ProtocolMessage> &message,
                             const int timeoutMs)
{
    if (closed_) {
        return false;
    }
    const std::shared_ptr<IpcPendingRequestState> reservation =
        reservePendingRequest(requestId);
    if (reservation == nullptr) return false;
    if (!message.has_value()) {
        removePendingRequest(requestId, reservation);
        lastErrorCode_ = QStringLiteral("ipc.protocol.invalid_payload");
        return false;
    }
    if (!sendInternal(*message, timeoutMs, true)) {
        removePendingRequest(requestId, reservation);
        return false;
    }
    reservation->deadlineMonotonicMs.store(
        steadyMonotonicMs() + timeoutMs, std::memory_order_relaxed);
    reservation->phase.store(IpcPendingRequestState::Phase::Published,
                             std::memory_order_release);
    return true;
}
