#pragma once

#include "FrameCodec.h"
#include "ProtocolMessage.h"
#include "WinPipeTransport.h"

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QSet>

#include <functional>
#include <memory>

struct IpcPendingRequestState;

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

struct IpcSendWork final
{
    std::function<bool()> publicationGate;
    std::function<void(const PipeWriteResult &)> completion;
};

using IpcSendSubmission = PipeWriteSubmission;

class IpcSession final
{
public:
    using PageMetadataHandler =
        std::function<void(const QString &title, const QString &status)>;

    IpcSession(WinPipeTransport transport,
               IpcRole role,
               HostLaunchContext hostContext = {});

    IpcSession(const IpcSession &) = delete;
    IpcSession &operator=(const IpcSession &) = delete;
    IpcSession(IpcSession &&) = default;
    IpcSession &operator=(IpcSession &&) = default;

    bool send(const ProtocolMessage &message, int timeoutMs = 1000);
    [[nodiscard]] IpcSendSubmission submitSend(
        const ProtocolMessage &message,
        IpcSendWork work = {});
    bool sendRequest(const QString &requestId,
                     const QString &capability,
                     const QString &operation,
                     const QJsonObject &payload,
                     int timeoutMs);
    bool sendRequest(const QString &requestId,
                     const QString &capability,
                     const QString &operation,
                     const QJsonObject &payload,
                     int writeTimeoutMs,
                     int responseTimeoutMs);
    bool sendRouteLoad(const QString &requestId, const QString &route, int timeoutMs);
    [[nodiscard]] IpcSendSubmission submitRouteLoad(
        const QString &requestId,
        const QString &route,
        int responseTimeoutMs,
        IpcSendWork work = {});
    bool sendNavigationRequest(const QString &requestId,
                               const QString &route,
                               int timeoutMs);
    bool sendVisibilityChanged(bool active, int timeoutMs = 1000);
    SessionReceiveResult receive(int timeoutMs);
    SessionReceiveResult poll(int timeoutMs = 0);
    void setPageMetadataHandler(PageMetadataHandler handler);

    bool isAuthenticated() const noexcept;
    QString appIdentity() const;
    bool isClosed() const noexcept;
    QString lastErrorCode() const;
    qsizetype pendingRequestCount() const noexcept;
    qint64 lastPeerActivityMonotonicMs() const noexcept;
    void close() noexcept;

private:
    struct QueuedMessage final {
        ProtocolMessage message;
        bool pageMetadataDelivered = false;
    };

    SessionReceiveResult processFrame(const QJsonObject &object);
    SessionReceiveResult receiveImpl(int timeoutMs, bool closeOnCallerTimeout);
    void deliverQueuedPageMetadata();
    SessionReceiveResult fail(SessionStatus status, const QString &code);
    bool pendingRequestExpired();
    std::optional<qint64> nearestPendingDeadline(
        bool &publicationPending) const;
    bool sendTracked(const QString &requestId,
                     const std::optional<ProtocolMessage> &message,
                     int writeTimeoutMs,
                     int responseTimeoutMs);
    bool sendInternal(const ProtocolMessage &message,
                      int timeoutMs,
                      bool allowTrackedMessage);
    [[nodiscard]] std::optional<QByteArray> prepareSend(
        const ProtocolMessage &message,
        bool allowTrackedMessage);
    [[nodiscard]] IpcSendSubmission submitInternal(
        const ProtocolMessage &message,
        IpcSendWork work,
        bool allowTrackedMessage,
        std::function<void(const PipeWriteResult &)> terminalObserver = {},
        std::function<void()> rejectionObserver = {});
    void noteAcceptedSend(const ProtocolMessage &message);
    [[nodiscard]] std::shared_ptr<IpcPendingRequestState>
    reservePendingRequest(const QString &requestId);
    void removePendingRequest(
        const QString &requestId,
        const std::shared_ptr<IpcPendingRequestState> &expected);
    void pruneTerminalPendingRequests();

    WinPipeTransport transport_;
    IpcRole role_;
    HostLaunchContext hostContext_;
    FrameCodec codec_;
    QQueue<QueuedMessage> receivedMessages_;
    QHash<QString, std::shared_ptr<IpcPendingRequestState>> pendingRequests_;
    QSet<QString> receivedRequestIds_;
    QElapsedTimer clock_;
    bool authenticated_ = false;
    bool closed_ = false;
    QString lastErrorCode_;
    QString outboundNonce_;
    QString peerAssignedIdentity_;
    PageMetadataHandler pageMetadataHandler_;
    bool deliveringPageMetadata_ = false;
    qint64 lastPeerActivityMs_ = 0;
    bool readySent_ = false;
    bool peerReady_ = false;
};

Q_DECLARE_METATYPE(SessionStatus)
