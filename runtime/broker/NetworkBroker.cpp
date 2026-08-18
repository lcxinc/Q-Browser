#include "NetworkBroker.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace {

BrokerResult failure(const QString &code, const QString &message)
{
    return BrokerResult::failure(code, message);
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    if (object.size() != expected.size()) {
        return false;
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected.contains(iterator.key())) {
            return false;
        }
    }
    return true;
}

bool pathMatches(const QString &path, const QString &prefix)
{
    if (prefix == QStringLiteral("/")) {
        return path.startsWith(u'/');
    }
    if (path == prefix) {
        return true;
    }
    return prefix.endsWith(u'/') ? path.startsWith(prefix)
                                 : path.startsWith(prefix + u'/');
}

bool hasDotSegment(const QString &path)
{
    const QStringList segments = path.split(u'/', Qt::KeepEmptyParts);
    return std::ranges::any_of(segments, [](const QString &segment) {
        return segment == QStringLiteral(".") || segment == QStringLiteral("..");
    });
}

} // namespace

NetworkBroker::NetworkBroker(EffectiveNetworkPolicy policy)
    : policy_(std::move(policy))
{
}

BrokerResult NetworkBroker::invoke(const QString &operation,
                                   const QJsonObject &payload,
                                   const HostRequestContext &context)
{
    Q_UNUSED(context)
    if (operation != QStringLiteral("request")
        || !hasExactKeys(payload,
                         {QStringLiteral("method"),
                          QStringLiteral("url"),
                          QStringLiteral("bodyBase64")})
        || !payload.value(QStringLiteral("method")).isString()
        || !payload.value(QStringLiteral("url")).isString()
        || !payload.value(QStringLiteral("bodyBase64")).isString()) {
        return failure(QStringLiteral("network.invalid_request"),
                       QStringLiteral("Network request is invalid."));
    }

    const auto method = parseHttpMethod(payload.value(QStringLiteral("method")).toString());
    const QUrl url(payload.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    const auto decodedBody = QByteArray::fromBase64Encoding(
        payload.value(QStringLiteral("bodyBase64")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (!method.has_value() || !url.isValid() || url.isRelative()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || !url.userInfo().isEmpty() || url.hasFragment() || !decodedBody) {
        return failure(QStringLiteral("network.invalid_request"),
                       QStringLiteral("Network request is invalid."));
    }
    if (decodedBody.decoded.size() > policy_.maximumRequestBytes) {
        return failure(QStringLiteral("network.payload_too_large"),
                       QStringLiteral("Network request is too large."));
    }

    const QString path = url.path(QUrl::FullyDecoded);
    if (!path.startsWith(u'/') || hasDotSegment(path)) {
        return failure(QStringLiteral("network.host_denied"),
                       QStringLiteral("Network destination is not permitted."));
    }
    const QString host = url.host(QUrl::FullyDecoded).toLower();
    bool allowed = false;
    for (const NetworkAllowRule &rule : policy_.rules) {
        if (rule.host == host && rule.methods.contains(*method)
            && pathMatches(path, rule.pathPrefix)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        return failure(QStringLiteral("network.host_denied"),
                       QStringLiteral("Network destination is not permitted."));
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=utf-8"));

    QNetworkReply *reply = nullptr;
    switch (*method) {
    case HttpMethod::Get:
        reply = manager.get(request);
        break;
    case HttpMethod::Post:
        reply = manager.post(request, decodedBody.decoded);
        break;
    case HttpMethod::Put:
        reply = manager.put(request, decodedBody.decoded);
        break;
    }

    QByteArray responseBody;
    bool responseTooLarge = false;
    bool timedOut = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        responseBody.append(reply->readAll());
        if (responseBody.size() > policy_.maximumResponseBytes) {
            responseTooLarge = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    timeout.start(policy_.timeoutMs);
    loop.exec();
    timeout.stop();
    responseBody.append(reply->readAll());
    if (responseBody.size() > policy_.maximumResponseBytes) {
        responseTooLarge = true;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    delete reply;

    if (responseTooLarge) {
        return failure(QStringLiteral("network.response_too_large"),
                       QStringLiteral("Network response is too large."));
    }
    if (timedOut) {
        return failure(QStringLiteral("network.timeout"),
                       QStringLiteral("Network request timed out."));
    }
    if (networkError != QNetworkReply::NoError && status == 0) {
        return failure(QStringLiteral("network.failed"),
                       QStringLiteral("Network request failed."));
    }

    return BrokerResult::success(
        QJsonObject{{QStringLiteral("status"), status},
                    {QStringLiteral("bodyBase64"),
                     QString::fromLatin1(responseBody.toBase64(QByteArray::Base64Encoding))}});
}
