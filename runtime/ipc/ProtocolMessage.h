#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

enum class ProtocolType {
    Handshake,
    HandshakeAck,
    SurfaceReady,
    RouteLoad,
    Ready,
    Request,
    Response,
    Heartbeat,
    StructuredLog,
    Shutdown,
};

enum class ProtocolError {
    None,
    InvalidEnvelope,
    UnsupportedVersion,
    UnknownType,
    InvalidRequestId,
    UnexpectedRequestId,
    InvalidPayload,
};

struct ProtocolParseResult;

class ProtocolMessage final
{
public:
    static constexpr int currentVersion() noexcept { return 1; }

    static ProtocolParseResult parse(const QJsonObject &object);
    static std::optional<ProtocolMessage> handshake(const QString &nonce);
    static std::optional<ProtocolMessage> handshakeAck(const QString &nonce,
                                                       const QString &appIdentity);
    static std::optional<ProtocolMessage> surfaceReady(const QString &windowHandle);
    static std::optional<ProtocolMessage> routeLoad(const QString &requestId,
                                                    const QString &route);
    static ProtocolMessage ready();
    static std::optional<ProtocolMessage> request(const QString &requestId,
                                                 const QString &capability,
                                                 const QString &operation,
                                                 const QJsonObject &payload);
    static std::optional<ProtocolMessage> successResponse(const QString &requestId,
                                                         const QJsonObject &result);
    static std::optional<ProtocolMessage> errorResponse(const QString &requestId,
                                                       const QString &code,
                                                       const QString &message);
    static ProtocolMessage heartbeat();
    static std::optional<ProtocolMessage> structuredLog(const QString &level,
                                                        const QString &category,
                                                        const QString &message);
    static std::optional<ProtocolMessage> shutdown(const QString &reason);

    int protocolVersion() const noexcept;
    ProtocolType type() const noexcept;
    QString requestId() const;
    QJsonObject payload() const;
    QJsonObject toJson() const;

private:
    ProtocolMessage(ProtocolType type, QString requestId, QJsonObject payload);
    static std::optional<ProtocolMessage> validated(ProtocolType type,
                                                    QString requestId,
                                                    QJsonObject payload);

    ProtocolType type_;
    QString requestId_;
    QJsonObject payload_;
};

struct ProtocolParseResult {
    std::optional<ProtocolMessage> message;
    ProtocolError error = ProtocolError::None;
    QString errorCode;
};

Q_DECLARE_METATYPE(ProtocolType)
Q_DECLARE_METATYPE(ProtocolError)
