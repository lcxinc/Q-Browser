#include "ProtocolMessage.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace {

constexpr qsizetype maximumTokenCharacters = 256;
constexpr qsizetype maximumPageTitleCodeUnits = 256;
constexpr qsizetype maximumPageStatusBytes = 32;

bool isBidiControl(const char16_t value)
{
    return value == 0x061c || (value >= 0x200e && value <= 0x200f)
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

QString typeName(const ProtocolType type)
{
    switch (type) {
    case ProtocolType::Handshake:
        return QStringLiteral("handshake");
    case ProtocolType::HandshakeAck:
        return QStringLiteral("handshakeAck");
    case ProtocolType::SurfaceReady:
        return QStringLiteral("surfaceReady");
    case ProtocolType::RouteLoad:
        return QStringLiteral("routeLoad");
    case ProtocolType::NavigationRequest:
        return QStringLiteral("navigationRequest");
    case ProtocolType::PageMetadata:
        return QStringLiteral("pageMetadata");
    case ProtocolType::Ready:
        return QStringLiteral("ready");
    case ProtocolType::Request:
        return QStringLiteral("request");
    case ProtocolType::Response:
        return QStringLiteral("response");
    case ProtocolType::Heartbeat:
        return QStringLiteral("heartbeat");
    case ProtocolType::StructuredLog:
        return QStringLiteral("structuredLog");
    case ProtocolType::Shutdown:
        return QStringLiteral("shutdown");
    }
    return {};
}

std::optional<ProtocolType> parseType(const QString &name)
{
    for (const ProtocolType type : {ProtocolType::Handshake,
                                    ProtocolType::HandshakeAck,
                                    ProtocolType::SurfaceReady,
                                    ProtocolType::RouteLoad,
                                    ProtocolType::NavigationRequest,
                                    ProtocolType::PageMetadata,
                                    ProtocolType::Ready,
                                    ProtocolType::Request,
                                    ProtocolType::Response,
                                    ProtocolType::Heartbeat,
                                    ProtocolType::StructuredLog,
                                    ProtocolType::Shutdown}) {
        if (name == typeName(type)) {
            return type;
        }
    }
    return std::nullopt;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) {
        return false;
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!keys.contains(iterator.key())) {
            return false;
        }
    }
    return true;
}

bool validToken(const QJsonValue &value)
{
    if (!value.isString() || value.toString().isEmpty()
        || value.toString().size() > maximumTokenCharacters) {
        return false;
    }
    for (const QChar character : value.toString()) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}

bool validText(const QJsonValue &value, const qsizetype maximumCharacters)
{
    if (!value.isString() || value.toString().isEmpty()
        || value.toString().size() > maximumCharacters) {
        return false;
    }
    for (const QChar character : value.toString()) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

bool validPageTitle(const QJsonValue &value)
{
    if (!value.isString()) return false;
    const QString title = value.toString();
    if (title.isEmpty() || title.size() > maximumPageTitleCodeUnits) return false;
    for (qsizetype index = 0; index < title.size(); ++index) {
        const QChar character = title.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= title.size() || !title.at(index + 1).isLowSurrogate()) {
                return false;
            }
            ++index;
            continue;
        }
        if (character.isLowSurrogate()
            || character.category() == QChar::Other_Control
            || isBidiControl(character.unicode()) || character == u'<'
            || character == u'>') {
            return false;
        }
    }
    return true;
}

bool validPageStatus(const QJsonValue &value)
{
    if (!value.isString()) return false;
    const QString status = value.toString();
    if (status.isEmpty() || status.size() > maximumPageStatusBytes) return false;
    for (const QChar character : status) {
        if (character.unicode() > 0x7f) return false;
    }
    return status == QStringLiteral("loading")
        || status == QStringLiteral("ready");
}

bool validPayload(const ProtocolType type, const QJsonObject &payload)
{
    switch (type) {
    case ProtocolType::Handshake:
        return hasExactKeys(payload, {QStringLiteral("nonce")})
               && validToken(payload.value(QStringLiteral("nonce")));
    case ProtocolType::HandshakeAck:
        return hasExactKeys(payload,
                            {QStringLiteral("nonce"), QStringLiteral("appIdentity")})
               && validToken(payload.value(QStringLiteral("nonce")))
               && validToken(payload.value(QStringLiteral("appIdentity")));
    case ProtocolType::SurfaceReady: {
        if (!hasExactKeys(payload, {QStringLiteral("windowHandle")})) {
            return false;
        }
        const QString handle = payload.value(QStringLiteral("windowHandle")).toString();
        bool converted = false;
        const qulonglong value = handle.toULongLong(&converted, 10);
        return converted && value != 0 && QString::number(value) == handle;
    }
    case ProtocolType::Ready:
    case ProtocolType::Heartbeat:
        return payload.isEmpty();
    case ProtocolType::RouteLoad:
    case ProtocolType::NavigationRequest: {
        if (!hasExactKeys(payload, {QStringLiteral("route")})) {
            return false;
        }
        const QJsonValue route = payload.value(QStringLiteral("route"));
        const QString value = route.toString();
        return validText(route, 2048) && value.startsWith(u'/')
               && !value.startsWith(QStringLiteral("//")) && !value.contains(u'#');
    }
    case ProtocolType::PageMetadata: {
        const bool titleOnly = hasExactKeys(payload, {QStringLiteral("title")});
        const bool withStatus = hasExactKeys(
            payload, {QStringLiteral("title"), QStringLiteral("status")});
        return (titleOnly || withStatus)
            && validPageTitle(payload.value(QStringLiteral("title")))
            && (!withStatus
                || validPageStatus(payload.value(QStringLiteral("status"))));
    }
    case ProtocolType::Request:
        return hasExactKeys(payload,
                            {QStringLiteral("capability"),
                             QStringLiteral("operation"),
                             QStringLiteral("payload")})
               && validToken(payload.value(QStringLiteral("capability")))
               && validToken(payload.value(QStringLiteral("operation")))
               && payload.value(QStringLiteral("payload")).isObject();
    case ProtocolType::Response: {
        const QJsonValue ok = payload.value(QStringLiteral("ok"));
        if (!ok.isBool()) {
            return false;
        }
        if (ok.toBool()) {
            return hasExactKeys(payload,
                                {QStringLiteral("ok"), QStringLiteral("result")})
                   && payload.value(QStringLiteral("result")).isObject();
        }
        if (!hasExactKeys(payload, {QStringLiteral("ok"), QStringLiteral("error")})) {
            return false;
        }
        const QJsonValue error = payload.value(QStringLiteral("error"));
        if (!error.isObject()) {
            return false;
        }
        const QJsonObject errorObject = error.toObject();
        return hasExactKeys(errorObject,
                            {QStringLiteral("code"), QStringLiteral("message")})
               && validToken(errorObject.value(QStringLiteral("code")))
               && errorObject.value(QStringLiteral("message")).isString()
               && errorObject.value(QStringLiteral("message")).toString().size()
                      <= maximumTokenCharacters;
    }
    case ProtocolType::Shutdown:
        return hasExactKeys(payload, {QStringLiteral("reason")})
               && validToken(payload.value(QStringLiteral("reason")));
    case ProtocolType::StructuredLog: {
        if (!hasExactKeys(payload,
                          {QStringLiteral("level"),
                           QStringLiteral("category"),
                           QStringLiteral("message")})) {
            return false;
        }
        const QString level = payload.value(QStringLiteral("level")).toString();
        return (level == QStringLiteral("debug") || level == QStringLiteral("info")
                || level == QStringLiteral("warning") || level == QStringLiteral("error"))
               && validToken(payload.value(QStringLiteral("category")))
               && validText(payload.value(QStringLiteral("message")), 4096);
    }
    }
    return false;
}

ProtocolParseResult error(const ProtocolError value, const QString &code)
{
    return {std::nullopt, value, code};
}

} // namespace

ProtocolParseResult ProtocolMessage::parse(const QJsonObject &object)
{
    const QJsonValue versionValue = object.value(QStringLiteral("protocolVersion"));
    if (!versionValue.isDouble() || !std::isfinite(versionValue.toDouble())
        || std::floor(versionValue.toDouble()) != versionValue.toDouble()) {
        return error(ProtocolError::InvalidEnvelope,
                     QStringLiteral("ipc.protocol.invalid_envelope"));
    }
    if (versionValue.toInt() != currentVersion()) {
        return error(ProtocolError::UnsupportedVersion,
                     QStringLiteral("ipc.protocol.unsupported_version"));
    }

    const QJsonValue typeValue = object.value(QStringLiteral("type"));
    if (!typeValue.isString()) {
        return error(ProtocolError::InvalidEnvelope,
                     QStringLiteral("ipc.protocol.invalid_envelope"));
    }
    const auto type = parseType(typeValue.toString());
    if (!type.has_value()) {
        return error(ProtocolError::UnknownType, QStringLiteral("ipc.protocol.unknown_type"));
    }

    const bool needsRequestId = *type == ProtocolType::Request
        || *type == ProtocolType::Response || *type == ProtocolType::RouteLoad
        || *type == ProtocolType::NavigationRequest;
    if (needsRequestId && !object.contains(QStringLiteral("requestId"))) {
        return error(ProtocolError::InvalidRequestId,
                     QStringLiteral("ipc.protocol.invalid_request_id"));
    }
    const QSet<QString> envelopeKeys = needsRequestId
                                           ? QSet<QString>{QStringLiteral("protocolVersion"),
                                                           QStringLiteral("type"),
                                                           QStringLiteral("requestId"),
                                                           QStringLiteral("payload")}
                                           : QSet<QString>{QStringLiteral("protocolVersion"),
                                                           QStringLiteral("type"),
                                                           QStringLiteral("payload")};
    if (!hasExactKeys(object, envelopeKeys)) {
        if (!needsRequestId && object.contains(QStringLiteral("requestId"))) {
            return error(ProtocolError::UnexpectedRequestId,
                         QStringLiteral("ipc.protocol.unexpected_request_id"));
        }
        return error(ProtocolError::InvalidEnvelope,
                     QStringLiteral("ipc.protocol.invalid_envelope"));
    }

    QString requestId;
    if (needsRequestId) {
        const QJsonValue requestIdValue = object.value(QStringLiteral("requestId"));
        if (!validToken(requestIdValue)) {
            return error(ProtocolError::InvalidRequestId,
                         QStringLiteral("ipc.protocol.invalid_request_id"));
        }
        requestId = requestIdValue.toString();
    }

    const QJsonValue payloadValue = object.value(QStringLiteral("payload"));
    if (!payloadValue.isObject() || !validPayload(*type, payloadValue.toObject())) {
        return error(ProtocolError::InvalidPayload,
                     QStringLiteral("ipc.protocol.invalid_payload"));
    }

    return {ProtocolMessage(*type, requestId, payloadValue.toObject()),
            ProtocolError::None,
            {}};
}

std::optional<ProtocolMessage> ProtocolMessage::handshake(const QString &nonce)
{
    return validated(ProtocolType::Handshake,
                     {},
                     QJsonObject{{QStringLiteral("nonce"), nonce}});
}

std::optional<ProtocolMessage> ProtocolMessage::handshakeAck(const QString &nonce,
                                                             const QString &appIdentity)
{
    return validated(ProtocolType::HandshakeAck,
                     {},
                     QJsonObject{{QStringLiteral("nonce"), nonce},
                                 {QStringLiteral("appIdentity"), appIdentity}});
}

std::optional<ProtocolMessage> ProtocolMessage::surfaceReady(const QString &windowHandle)
{
    return validated(ProtocolType::SurfaceReady,
                     {},
                     QJsonObject{{QStringLiteral("windowHandle"), windowHandle}});
}

std::optional<ProtocolMessage> ProtocolMessage::routeLoad(const QString &requestId,
                                                          const QString &route)
{
    return validated(ProtocolType::RouteLoad,
                     requestId,
                     QJsonObject{{QStringLiteral("route"), route}});
}

std::optional<ProtocolMessage> ProtocolMessage::navigationRequest(const QString &requestId,
                                                                  const QString &route)
{
    return validated(ProtocolType::NavigationRequest,
                     requestId,
                     QJsonObject{{QStringLiteral("route"), route}});
}

std::optional<ProtocolMessage> ProtocolMessage::pageMetadata(const QString &title,
                                                             const QString &status)
{
    QJsonObject payload{{QStringLiteral("title"), title}};
    if (!status.isEmpty()) payload.insert(QStringLiteral("status"), status);
    return validated(ProtocolType::PageMetadata, {}, std::move(payload));
}

ProtocolMessage ProtocolMessage::ready()
{
    return {ProtocolType::Ready, {}, {}};
}

std::optional<ProtocolMessage> ProtocolMessage::request(const QString &requestId,
                                                        const QString &capability,
                                                        const QString &operation,
                                                        const QJsonObject &payload)
{
    return validated(ProtocolType::Request,
                     requestId,
                     QJsonObject{{QStringLiteral("capability"), capability},
                                 {QStringLiteral("operation"), operation},
                                 {QStringLiteral("payload"), payload}});
}

std::optional<ProtocolMessage> ProtocolMessage::successResponse(const QString &requestId,
                                                                const QJsonObject &result)
{
    return validated(ProtocolType::Response,
                     requestId,
                     QJsonObject{{QStringLiteral("ok"), true},
                                 {QStringLiteral("result"), result}});
}

std::optional<ProtocolMessage> ProtocolMessage::errorResponse(const QString &requestId,
                                                              const QString &code,
                                                              const QString &message)
{
    return validated(ProtocolType::Response,
                     requestId,
                     QJsonObject{{QStringLiteral("ok"), false},
                                 {QStringLiteral("error"),
                                  QJsonObject{{QStringLiteral("code"), code},
                                              {QStringLiteral("message"), message}}}});
}

ProtocolMessage ProtocolMessage::heartbeat()
{
    return {ProtocolType::Heartbeat, {}, {}};
}

std::optional<ProtocolMessage> ProtocolMessage::structuredLog(const QString &level,
                                                              const QString &category,
                                                              const QString &message)
{
    return validated(ProtocolType::StructuredLog,
                     {},
                     QJsonObject{{QStringLiteral("level"), level},
                                 {QStringLiteral("category"), category},
                                 {QStringLiteral("message"), message}});
}

std::optional<ProtocolMessage> ProtocolMessage::shutdown(const QString &reason)
{
    return validated(ProtocolType::Shutdown,
                     {},
                     QJsonObject{{QStringLiteral("reason"), reason}});
}

int ProtocolMessage::protocolVersion() const noexcept
{
    return currentVersion();
}

ProtocolType ProtocolMessage::type() const noexcept
{
    return type_;
}

QString ProtocolMessage::requestId() const
{
    return requestId_;
}

QJsonObject ProtocolMessage::payload() const
{
    return payload_;
}

QJsonObject ProtocolMessage::toJson() const
{
    QJsonObject object{{QStringLiteral("protocolVersion"), currentVersion()},
                       {QStringLiteral("type"), typeName(type_)},
                       {QStringLiteral("payload"), payload_}};
    if (!requestId_.isEmpty()) {
        object.insert(QStringLiteral("requestId"), requestId_);
    }
    return object;
}

ProtocolMessage::ProtocolMessage(const ProtocolType type,
                                 QString requestId,
                                 QJsonObject payload)
    : type_(type), requestId_(std::move(requestId)), payload_(std::move(payload))
{
}

std::optional<ProtocolMessage> ProtocolMessage::validated(const ProtocolType type,
                                                          QString requestId,
                                                          QJsonObject payload)
{
    ProtocolMessage candidate(type, std::move(requestId), std::move(payload));
    return parse(candidate.toJson()).message;
}
