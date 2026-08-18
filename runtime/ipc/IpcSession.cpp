#include "IpcSession.h"

#include <algorithm>

namespace {

bool outgoingTypeAllowed(const IpcRole role, const ProtocolType type)
{
    if (role == IpcRole::Host) {
        return type == ProtocolType::HandshakeAck || type == ProtocolType::RouteLoad
            || type == ProtocolType::Response || type == ProtocolType::Heartbeat
            || type == ProtocolType::Shutdown;
    }
    return type == ProtocolType::Handshake || type == ProtocolType::SurfaceReady
        || type == ProtocolType::Ready || type == ProtocolType::Request
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
    if (closed_) {
        return false;
    }
    if (!authenticated_) {
        if (role_ != IpcRole::Worker || message.type() != ProtocolType::Handshake
            || !outboundNonce_.isEmpty()) {
            lastErrorCode_ = QStringLiteral("ipc.session.authentication_required");
            return false;
        }
    } else if (!outgoingTypeAllowed(role_, message.type())) {
        lastErrorCode_ = QStringLiteral("ipc.session.unexpected_message_direction");
        return false;
    }
    const ProtocolParseResult validated = ProtocolMessage::parse(message.toJson());
    if (!validated.message.has_value()) {
        lastErrorCode_ = validated.errorCode;
        return false;
    }
    const QByteArray frame = FrameCodec::encode(message.toJson());
    if (frame.isEmpty() || !transport_.writeAll(frame, timeoutMs)) {
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
    if (role_ == IpcRole::Worker && message.type() == ProtocolType::Handshake) {
        outboundNonce_ = message.payload().value(QStringLiteral("nonce")).toString();
    }
    return true;
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

SessionReceiveResult IpcSession::receive(const int timeoutMs)
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
        return {SessionStatus::MessageReady, receivedMessages_.dequeue(), {}};
    }

    QElapsedTimer receiveTimer;
    receiveTimer.start();
    for (;;) {
        const int remaining = std::max(0, timeoutMs - static_cast<int>(receiveTimer.elapsed()));
        const PipeReadResult read = transport_.readSome(64 * 1024, remaining);
        if (read.status == PipeIoStatus::TimedOut) {
            return fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.timeout"));
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
            receivedMessages_.enqueue(*processed.message);
        }
        if (!receivedMessages_.isEmpty()) {
            return {SessionStatus::MessageReady, receivedMessages_.dequeue(), {}};
        }
        if (receiveTimer.elapsed() >= timeoutMs) {
            return fail(SessionStatus::TimedOut, QStringLiteral("ipc.session.timeout"));
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
    return pendingRequests_.size();
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

    if (message.type() == ProtocolType::Request || message.type() == ProtocolType::RouteLoad) {
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
        if (!pendingRequests_.remove(message.requestId())) {
            return fail(SessionStatus::Failed, QStringLiteral("ipc.session.unknown_response"));
        }
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

bool IpcSession::pendingRequestExpired() const
{
    const qint64 now = clock_.elapsed();
    for (auto iterator = pendingRequests_.constBegin();
         iterator != pendingRequests_.constEnd();
         ++iterator) {
        if (iterator.value() <= now) {
            return true;
        }
    }
    return false;
}

bool IpcSession::sendTracked(const QString &requestId,
                             const std::optional<ProtocolMessage> &message,
                             const int timeoutMs)
{
    if (closed_) {
        return false;
    }
    if (pendingRequests_.contains(requestId)) {
        lastErrorCode_ = QStringLiteral("ipc.session.duplicate_request_id");
        return false;
    }
    constexpr qsizetype maximumPendingRequests = 1024;
    if (pendingRequests_.size() >= maximumPendingRequests) {
        lastErrorCode_ = QStringLiteral("ipc.session.pending_request_limit");
        return false;
    }
    if (!message.has_value()) {
        lastErrorCode_ = QStringLiteral("ipc.protocol.invalid_payload");
        return false;
    }
    if (!send(*message, timeoutMs)) {
        return false;
    }
    pendingRequests_.insert(requestId, clock_.elapsed() + timeoutMs);
    return true;
}
