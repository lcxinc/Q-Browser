#pragma once

#include "FrameCodec.h"
#include "ProtocolMessage.h"
#include "WinPipeTransport.h"

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QSet>

enum class IpcRole {
    Host,
    Worker,
};

enum class SessionStatus {
    MessageReady,
    TimedOut,
    PeerClosed,
    Failed,
};

struct HostLaunchContext {
    QString nonce;
    QString appIdentity;
};

struct SessionReceiveResult {
    SessionStatus status = SessionStatus::Failed;
    std::optional<ProtocolMessage> message;
    QString errorCode;
};

class IpcSession final
{
public:
    IpcSession(WinPipeTransport transport,
               IpcRole role,
               HostLaunchContext hostContext = {});

    IpcSession(const IpcSession &) = delete;
    IpcSession &operator=(const IpcSession &) = delete;
    IpcSession(IpcSession &&) = default;
    IpcSession &operator=(IpcSession &&) = default;

    bool send(const ProtocolMessage &message, int timeoutMs = 1000);
    bool sendRequest(const QString &requestId,
                     const QString &capability,
                     const QString &operation,
                     const QJsonObject &payload,
                     int timeoutMs);
    bool sendRouteLoad(const QString &requestId, const QString &route, int timeoutMs);
    SessionReceiveResult receive(int timeoutMs);

    bool isAuthenticated() const noexcept;
    QString appIdentity() const;
    bool isClosed() const noexcept;
    QString lastErrorCode() const;
    qsizetype pendingRequestCount() const noexcept;
    qint64 lastPeerActivityMonotonicMs() const noexcept;
    void close() noexcept;

private:
    SessionReceiveResult processFrame(const QJsonObject &object);
    SessionReceiveResult fail(SessionStatus status, const QString &code);
    bool pendingRequestExpired() const;
    bool sendTracked(const QString &requestId,
                     const std::optional<ProtocolMessage> &message,
                     int timeoutMs);

    WinPipeTransport transport_;
    IpcRole role_;
    HostLaunchContext hostContext_;
    FrameCodec codec_;
    QQueue<ProtocolMessage> receivedMessages_;
    QHash<QString, qint64> pendingRequests_;
    QSet<QString> receivedRequestIds_;
    QElapsedTimer clock_;
    bool authenticated_ = false;
    bool closed_ = false;
    QString lastErrorCode_;
    QString outboundNonce_;
    QString peerAssignedIdentity_;
    qint64 lastPeerActivityMs_ = 0;
};

Q_DECLARE_METATYPE(SessionStatus)
