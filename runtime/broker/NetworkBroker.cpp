#include "NetworkBroker.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace {

class QtNetworkAddressResolver final : public NetworkAddressResolver
{
public:
    QList<QHostAddress> resolve(const QString &host,
                               const int timeoutMs,
                               bool &timedOut) override
    {
        timedOut = false;
        QHostAddress literal;
        if (literal.setAddress(host)) {
            return {literal};
        }
        QList<QHostAddress> addresses;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        const int lookupId = QHostInfo::lookupHost(host, &loop, [&](const QHostInfo &info) {
            if (info.error() == QHostInfo::NoError) {
                addresses = info.addresses();
            }
            loop.quit();
        });
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            QHostInfo::abortHostLookup(lookupId);
            loop.quit();
        });
        timer.start(timeoutMs);
        loop.exec();
        return addresses;
    }
};

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
    return path == prefix
        || (prefix.endsWith(u'/') ? path.startsWith(prefix)
                                  : path.startsWith(prefix + u'/'));
}

bool hasDotSegment(const QString &path)
{
    const QStringList segments = path.split(u'/', Qt::KeepEmptyParts);
    return std::ranges::any_of(segments, [](const QString &segment) {
        return segment == QStringLiteral(".") || segment == QStringLiteral("..");
    });
}

NetworkAddressClass classify(const QHostAddress &address)
{
    if (address.isLoopback()) {
        return NetworkAddressClass::Loopback;
    }
    if (address.isLinkLocal()) {
        return NetworkAddressClass::LinkLocal;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 value = address.toIPv4Address();
        if ((value & 0xff000000U) == 0x0a000000U
            || (value & 0xfff00000U) == 0xac100000U
            || (value & 0xffff0000U) == 0xc0a80000U
            || (value & 0xffc00000U) == 0x64400000U) {
            return NetworkAddressClass::Private;
        }
    } else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR bytes = address.toIPv6Address();
        if ((bytes[0] & 0xfeU) == 0xfcU) {
            return NetworkAddressClass::Private;
        }
    }
    return NetworkAddressClass::Public;
}

int remaining(const QElapsedTimer &timer, const int timeoutMs)
{
    return std::max(0, timeoutMs - static_cast<int>(timer.elapsed()));
}

bool connectPinned(QAbstractSocket &socket,
                   const QHostAddress &address,
                   const quint16 port,
                   const QString &peerName,
                   const bool encrypted,
                   const int timeoutMs,
                   bool &timedOut)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool connected = false;
    if (encrypted) {
        auto &ssl = static_cast<QSslSocket &>(socket);
        ssl.setPeerVerifyName(peerName);
        QObject::connect(&ssl, &QSslSocket::encrypted, &loop, [&] {
            connected = true;
            loop.quit();
        });
        QObject::connect(&ssl,
                         &QSslSocket::sslErrors,
                         &loop,
                         [&](const QList<QSslError> &) { loop.quit(); });
        QObject::connect(&socket, &QAbstractSocket::connected, &loop, [&] {
            ssl.startClientEncryption();
        });
    } else {
        QObject::connect(&socket, &QAbstractSocket::connected, &loop, [&] {
            connected = true;
            loop.quit();
        });
    }
    QObject::connect(&socket, &QAbstractSocket::errorOccurred, &loop, [&] { loop.quit(); });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        socket.abort();
        loop.quit();
    });
    timer.start(timeoutMs);
    socket.connectToHost(address, port);
    loop.exec();
    return connected;
}

struct HttpResponse {
    bool complete = false;
    bool timedOut = false;
    bool tooLarge = false;
    int status = 0;
    QByteArray body;
};

HttpResponse exchangeHttp(QAbstractSocket &socket,
                          const QByteArray &request,
                          const qint64 maximumBodyBytes,
                          const int timeoutMs)
{
    HttpResponse response;
    QByteArray received;
    constexpr qsizetype maximumHeaderBytes = 32 * 1024;
    qsizetype expectedTotal = -1;
    qsizetype bodyStart = -1;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    const auto process = [&] {
        received.append(socket.readAll());
        if (received.size() > maximumHeaderBytes + maximumBodyBytes) {
            response.tooLarge = true;
            socket.abort();
            loop.quit();
            return;
        }
        if (bodyStart < 0) {
            const qsizetype separator = received.indexOf("\r\n\r\n");
            if (separator < 0) {
                if (received.size() > maximumHeaderBytes) {
                    response.tooLarge = true;
                    socket.abort();
                    loop.quit();
                }
                return;
            }
            bodyStart = separator + 4;
            const QList<QByteArray> lines = received.first(separator).split('\n');
            static const QRegularExpression statusPattern(
                QStringLiteral(R"(^HTTP/1\.[01] ([1-5][0-9]{2}) .+\r?$)"));
            const auto match = statusPattern.match(QString::fromLatin1(lines.value(0)));
            if (!match.hasMatch()) {
                loop.quit();
                return;
            }
            response.status = match.captured(1).toInt();
            std::optional<qint64> contentLength;
            for (qsizetype index = 1; index < lines.size(); ++index) {
                QByteArray line = lines.at(index);
                if (line.endsWith('\r')) {
                    line.chop(1);
                }
                const qsizetype colon = line.indexOf(':');
                if (colon <= 0) {
                    loop.quit();
                    return;
                }
                const QByteArray name = line.first(colon).trimmed().toLower();
                const QByteArray value = line.sliced(colon + 1).trimmed();
                if (name == "transfer-encoding") {
                    loop.quit();
                    return;
                }
                if (name == "content-length") {
                    bool ok = false;
                    const qint64 parsed = value.toLongLong(&ok);
                    if (!ok || parsed < 0 || contentLength.has_value()) {
                        loop.quit();
                        return;
                    }
                    contentLength = parsed;
                }
            }
            if (!contentLength.has_value()) {
                loop.quit();
                return;
            }
            if (*contentLength > maximumBodyBytes) {
                response.tooLarge = true;
                socket.abort();
                loop.quit();
                return;
            }
            expectedTotal = bodyStart + static_cast<qsizetype>(*contentLength);
        }
        if (expectedTotal >= 0 && received.size() >= expectedTotal) {
            if (received.size() != expectedTotal) {
                loop.quit();
                return;
            }
            response.body = received.sliced(bodyStart, expectedTotal - bodyStart);
            response.complete = true;
            loop.quit();
        }
    };
    QObject::connect(&socket, &QIODevice::readyRead, &loop, process);
    QObject::connect(&socket, &QAbstractSocket::disconnected, &loop, process);
    QObject::connect(&socket, &QAbstractSocket::errorOccurred, &loop, [&] { loop.quit(); });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        response.timedOut = true;
        socket.abort();
        loop.quit();
    });
    if (socket.write(request) != request.size()) {
        return response;
    }
    timer.start(timeoutMs);
    loop.exec();
    return response;
}

} // namespace

NetworkBroker::NetworkBroker(EffectiveNetworkPolicy policy)
    : policy_(std::move(policy)), ownedResolver_(std::make_unique<QtNetworkAddressResolver>()),
      resolver_(ownedResolver_.get())
{
}

NetworkBroker::NetworkBroker(EffectiveNetworkPolicy policy, NetworkAddressResolver &resolver)
    : policy_(std::move(policy)), resolver_(&resolver)
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
    QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    const QString host = url.host(QUrl::FullyDecoded).toLower();
    const int port = url.port(url.scheme() == QStringLiteral("https") ? 443 : 80);
    const NetworkScheme scheme = url.scheme() == QStringLiteral("https")
        ? NetworkScheme::Https
        : NetworkScheme::Http;
    if (!path.startsWith(u'/') || hasDotSegment(path) || port <= 0 || port > 65535) {
        return failure(QStringLiteral("network.host_denied"),
                       QStringLiteral("Network destination is not permitted."));
    }
    const NetworkAllowRule *allowedRule = nullptr;
    for (const NetworkAllowRule &rule : policy_.rules) {
        if (rule.scheme == scheme && rule.host == host && rule.port == port
            && rule.methods.contains(*method) && pathMatches(path, rule.pathPrefix)) {
            allowedRule = &rule;
            break;
        }
    }
    if (allowedRule == nullptr) {
        return failure(QStringLiteral("network.host_denied"),
                       QStringLiteral("Network destination is not permitted."));
    }
    QElapsedTimer operationTimer;
    operationTimer.start();
    bool resolutionTimedOut = false;
    const QList<QHostAddress> addresses =
        resolver_->resolve(host, policy_.timeoutMs, resolutionTimedOut);
    if (resolutionTimedOut) {
        return failure(QStringLiteral("network.timeout"),
                       QStringLiteral("Network request timed out."));
    }
    if (addresses.isEmpty()) {
        return failure(QStringLiteral("network.failed"),
                       QStringLiteral("Network request failed."));
    }
    for (const QHostAddress &address : addresses) {
        if (address.isNull() || address.isMulticast()
            || !allowedRule->addressClasses.contains(classify(address))) {
            return failure(QStringLiteral("network.host_denied"),
                           QStringLiteral("Network destination is not permitted."));
        }
    }
    const int connectTimeout = remaining(operationTimer, policy_.timeoutMs);
    if (connectTimeout <= 0) {
        return failure(QStringLiteral("network.timeout"),
                       QStringLiteral("Network request timed out."));
    }
    std::unique_ptr<QAbstractSocket> socket;
    if (scheme == NetworkScheme::Https) {
        socket = std::make_unique<QSslSocket>();
    } else {
        socket = std::make_unique<QTcpSocket>();
    }
    bool connectionTimedOut = false;
    if (!connectPinned(*socket,
                       addresses.first(),
                       static_cast<quint16>(port),
                       host,
                       scheme == NetworkScheme::Https,
                       connectTimeout,
                       connectionTimedOut)) {
        return failure(connectionTimedOut ? QStringLiteral("network.timeout")
                                         : QStringLiteral("network.failed"),
                       connectionTimedOut ? QStringLiteral("Network request timed out.")
                                          : QStringLiteral("Network request failed."));
    }
    QByteArray target = url.path(QUrl::FullyEncoded).toLatin1();
    if (target.isEmpty()) {
        target = "/";
    }
    if (url.hasQuery()) {
        target += '?' + url.query(QUrl::FullyEncoded).toLatin1();
    }
    const bool defaultPort = (scheme == NetworkScheme::Http && port == 80)
        || (scheme == NetworkScheme::Https && port == 443);
    const QByteArray hostHeader = host.toLatin1()
        + (defaultPort ? QByteArray{} : ':' + QByteArray::number(port));
    const QByteArray request = httpMethodName(*method).toLatin1() + ' ' + target
        + " HTTP/1.1\r\nHost: " + hostHeader
        + "\r\nConnection: close\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "
        + QByteArray::number(decodedBody.decoded.size()) + "\r\n\r\n" + decodedBody.decoded;
    const int exchangeTimeout = remaining(operationTimer, policy_.timeoutMs);
    if (exchangeTimeout <= 0) {
        return failure(QStringLiteral("network.timeout"),
                       QStringLiteral("Network request timed out."));
    }
    const HttpResponse response = exchangeHttp(
        *socket,
        request,
        std::min(policy_.maximumResponseBytes, maximumIpcBinaryResultBytes()),
        exchangeTimeout);
    if (response.tooLarge) {
        return failure(QStringLiteral("network.response_too_large"),
                       QStringLiteral("Network response is too large."));
    }
    if (response.timedOut) {
        return failure(QStringLiteral("network.timeout"),
                       QStringLiteral("Network request timed out."));
    }
    if (!response.complete) {
        return failure(QStringLiteral("network.failed"),
                       QStringLiteral("Network request failed."));
    }
    return BrokerResult::success(
        QJsonObject{{QStringLiteral("status"), response.status},
                    {QStringLiteral("bodyBase64"),
                     QString::fromLatin1(response.body.toBase64(QByteArray::Base64Encoding))}});
}
