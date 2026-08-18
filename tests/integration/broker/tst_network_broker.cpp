#include "NetworkBroker.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>

class LocalHttpServer final : public QTcpServer
{
    Q_OBJECT

public:
    LocalHttpServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const QByteArray request = socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        return;
                    }
                    ++requestCount;
                    lastRequest = request;
                    const auto respond = [this, socket] {
                        if (socket->state() == QAbstractSocket::ConnectedState) {
                            const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ")
                                + QByteArray::number(responseBody.size())
                                + QByteArray("\r\nConnection: close\r\n\r\n") + responseBody;
                            socket->write(response);
                            socket->disconnectFromHost();
                        }
                    };
                    if (responseDelayMs > 0) {
                        QTimer::singleShot(responseDelayMs, socket, respond);
                    } else {
                        respond();
                    }
                });
            }
        });
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(serverPort())
                        .arg(path));
    }

    int requestCount = 0;
    int responseDelayMs = 0;
    QByteArray responseBody = "ok";
    QByteArray lastRequest;
};

class FixedResolver final : public NetworkAddressResolver
{
public:
    QList<QHostAddress> resolve(const QString &host, int, bool &timedOut) override
    {
        lastHost = host;
        timedOut = false;
        return addresses;
    }

    QList<QHostAddress> addresses;
    QString lastHost;
};

namespace {

EffectiveNetworkPolicy policyFor(const LocalHttpServer &server)
{
    return EffectiveNetworkPolicy{{NetworkAllowRule{NetworkScheme::Http,
                                                     QStringLiteral("127.0.0.1"),
                                                     server.serverPort(),
                                                     QStringLiteral("/api"),
                                                     {HttpMethod::Get, HttpMethod::Post},
                                                     {NetworkAddressClass::Loopback}}},
                                  8,
                                  32,
                                  1000};
}

QJsonObject requestPayload(const QString &method, const QUrl &url, const QByteArray &body = {})
{
    return {{QStringLiteral("method"), method},
            {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("bodyBase64"),
             QString::fromLatin1(body.toBase64(QByteArray::Base64Encoding))}};
}

} // namespace

class NetworkBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void performsAllowedRequestAgainstRealServer();
    void deniedPathAndMethodNeverReachServer();
    void boundsRequestAndResponsePayloads();
    void returnsStableTimeout();
    void rejectsWrongSchemePortAndResolvedAddressClass();
    void classifiesOnlyGloballyRoutableAddressesAsPublic_data();
    void classifiesOnlyGloballyRoutableAddressesAsPublic();
    void canonicalizesUnicodeIdnForPolicyDnsAndHostHeader();
};

void NetworkBrokerTest::performsAllowedRequestAgainstRealServer()
{
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkBroker broker(policyFor(server));

    const BrokerResult result = broker.invoke(QStringLiteral("request"),
                                              requestPayload(QStringLiteral("GET"),
                                                             server.url(QStringLiteral("/api/orders"))),
                                              {QStringLiteral("host.identity"), QStringLiteral("request")});

    QVERIFY2(result.ok, qPrintable(result.errorCode));
    QCOMPARE(result.value.value(QStringLiteral("status")).toInt(), 200);
    QCOMPARE(QByteArray::fromBase64(
                 result.value.value(QStringLiteral("bodyBase64")).toString().toLatin1()),
             QByteArray("ok"));
    QCOMPARE(server.requestCount, 1);
}

void NetworkBrokerTest::deniedPathAndMethodNeverReachServer()
{
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkBroker broker(policyFor(server));

    BrokerResult result = broker.invoke(QStringLiteral("request"),
                                        requestPayload(QStringLiteral("GET"),
                                                       server.url(QStringLiteral("/private"))),
                                        {QStringLiteral("host.identity"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));

    result = broker.invoke(QStringLiteral("request"),
                           requestPayload(QStringLiteral("PUT"),
                                          server.url(QStringLiteral("/api"))),
                           {QStringLiteral("host.identity"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));
    QCOMPARE(server.requestCount, 0);
}

void NetworkBrokerTest::boundsRequestAndResponsePayloads()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral("QIODevice::read.*device not open")));
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkBroker broker(policyFor(server));

    BrokerResult result = broker.invoke(QStringLiteral("request"),
                                        requestPayload(QStringLiteral("POST"),
                                                       server.url(QStringLiteral("/api")),
                                                       QByteArray(9, 'x')),
                                        {QStringLiteral("host.identity"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.payload_too_large"));
    QCOMPARE(server.requestCount, 0);

    server.responseBody = QByteArray(33, 'y');
    result = broker.invoke(QStringLiteral("request"),
                           requestPayload(QStringLiteral("GET"),
                                          server.url(QStringLiteral("/api"))),
                           {QStringLiteral("host.identity"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.response_too_large"));
    QCOMPARE(server.requestCount, 1);
}

void NetworkBrokerTest::returnsStableTimeout()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral("QIODevice::read.*device not open")));
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.responseDelayMs = 250;
    EffectiveNetworkPolicy policy = policyFor(server);
    policy.timeoutMs = 100;
    NetworkBroker broker(policy);

    const BrokerResult result = broker.invoke(QStringLiteral("request"),
                                              requestPayload(QStringLiteral("GET"),
                                                             server.url(QStringLiteral("/api"))),
                                              {QStringLiteral("host.identity"), QStringLiteral("request")});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.timeout"));
    QCOMPARE(server.requestCount, 1);
}

void NetworkBrokerTest::rejectsWrongSchemePortAndResolvedAddressClass()
{
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkBroker broker(policyFor(server));
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("request")};

    QUrl wrongScheme = server.url(QStringLiteral("/api"));
    wrongScheme.setScheme(QStringLiteral("https"));
    BrokerResult result = broker.invoke(QStringLiteral("request"),
                                        requestPayload(QStringLiteral("GET"), wrongScheme),
                                        context);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));

    QUrl wrongPort = server.url(QStringLiteral("/api"));
    wrongPort.setPort(static_cast<int>(server.serverPort()) + 1);
    result = broker.invoke(QStringLiteral("request"),
                           requestPayload(QStringLiteral("GET"), wrongPort),
                           context);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));
    QCOMPARE(server.requestCount, 0);

    EffectiveNetworkPolicy rebound = policyFor(server);
    rebound.rules[0].host = QStringLiteral("api.example.com");
    rebound.rules[0].addressClasses = {NetworkAddressClass::Public};
    FixedResolver resolver;
    resolver.addresses = {QHostAddress::LocalHost};
    NetworkBroker reboundBroker(rebound, resolver);
    QUrl reboundUrl(QStringLiteral("http://api.example.com:%1/api").arg(server.serverPort()));
    result = reboundBroker.invoke(QStringLiteral("request"),
                                  requestPayload(QStringLiteral("GET"), reboundUrl),
                                  context);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));
    QCOMPARE(server.requestCount, 0);
}

void NetworkBrokerTest::classifiesOnlyGloballyRoutableAddressesAsPublic_data()
{
    QTest::addColumn<QString>("address");
    QTest::addColumn<int>("expected");
    const int denied = -1;
    QTest::newRow("public-v4") << QStringLiteral("8.8.8.8")
                               << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("zero-net") << QStringLiteral("0.1.2.3") << denied;
    QTest::newRow("private-10") << QStringLiteral("10.1.2.3")
                                << static_cast<int>(NetworkAddressClass::Private);
    QTest::newRow("carrier-grade") << QStringLiteral("100.64.0.1") << denied;
    QTest::newRow("loopback") << QStringLiteral("127.0.0.1")
                              << static_cast<int>(NetworkAddressClass::Loopback);
    QTest::newRow("link-local") << QStringLiteral("169.254.1.1")
                               << static_cast<int>(NetworkAddressClass::LinkLocal);
    QTest::newRow("documentation") << QStringLiteral("192.0.2.1") << denied;
    QTest::newRow("benchmark") << QStringLiteral("198.18.0.1") << denied;
    QTest::newRow("broadcast") << QStringLiteral("255.255.255.255") << denied;
    QTest::newRow("mapped-private") << QStringLiteral("::ffff:192.168.1.1")
                                    << static_cast<int>(NetworkAddressClass::Private);
    QTest::newRow("mapped-loopback") << QStringLiteral("::ffff:127.0.0.1")
                                     << static_cast<int>(NetworkAddressClass::Loopback);
    QTest::newRow("public-v6") << QStringLiteral("2606:4700:4700::1111")
                               << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-apnic-v6") << QStringLiteral("2404:6800:4001::1")
                                         << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-ripe-v6") << QStringLiteral("2a02:6b8::1")
                                         << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-legacy-2001") << QStringLiteral("2001:4860::1")
                                             << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4800-explicit") << QStringLiteral("2001:4800::1")
                                                    << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0200") << QStringLiteral("2001:200::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0400") << QStringLiteral("2001:400::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0600") << QStringLiteral("2001:600::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0800") << QStringLiteral("2001:800::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0c00") << QStringLiteral("2001:c00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-0e00") << QStringLiteral("2001:e00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-1200") << QStringLiteral("2001:1200::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-1400") << QStringLiteral("2001:1400::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-1800") << QStringLiteral("2001:1800::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-1a00") << QStringLiteral("2001:1a00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-1c00") << QStringLiteral("2001:1c00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-2000") << QStringLiteral("2001:2000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4000") << QStringLiteral("2001:4000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4200") << QStringLiteral("2001:4200::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4400") << QStringLiteral("2001:4400::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4600") << QStringLiteral("2001:4600::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4a00") << QStringLiteral("2001:4a00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-4c00") << QStringLiteral("2001:4c00::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-5000") << QStringLiteral("2001:5000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-8000") << QStringLiteral("2001:8000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-a000") << QStringLiteral("2001:a000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2001-b000") << QStringLiteral("2001:b000::1")
                                           << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2003") << QStringLiteral("2003::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2410") << QStringLiteral("2410::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2610") << QStringLiteral("2610::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2620") << QStringLiteral("2620::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2630") << QStringLiteral("2630::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2a10") << QStringLiteral("2a10::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2800") << QStringLiteral("2800::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("allocated-2c00") << QStringLiteral("2c00::1")
                                      << static_cast<int>(NetworkAddressClass::Public);
    QTest::newRow("unallocated-3000-v6") << QStringLiteral("3000::1") << denied;
    QTest::newRow("unallocated-before-2001-0200") << QStringLiteral("2001:1ff::1")
                                                   << denied;
    QTest::newRow("unallocated-2001-1000-gap") << QStringLiteral("2001:1000::1")
                                                << denied;
    QTest::newRow("unallocated-2001-4e00-gap") << QStringLiteral("2001:4e00::1")
                                                << denied;
    QTest::newRow("unallocated-2001-6000-gap") << QStringLiteral("2001:6000::1")
                                                << denied;
    QTest::newRow("unallocated-2001-c000-gap") << QStringLiteral("2001:c000::1")
                                                << denied;
    QTest::newRow("unallocated-after-2003-18") << QStringLiteral("2003:4000::1")
                                                << denied;
    QTest::newRow("unallocated-2500-gap") << QStringLiteral("2500::1") << denied;
    QTest::newRow("unallocated-after-2610-23") << QStringLiteral("2610:200::1")
                                                << denied;
    QTest::newRow("unallocated-2700-gap") << QStringLiteral("2700::1") << denied;
    QTest::newRow("unallocated-2b00-gap") << QStringLiteral("2b00::1") << denied;
    QTest::newRow("former-6bone-v6") << QStringLiteral("3ffe::1") << denied;
    QTest::newRow("unallocated-2d00-v6") << QStringLiteral("2d00::1") << denied;
    QTest::newRow("unspecified-v6") << QStringLiteral("::") << denied;
    QTest::newRow("documentation-v6") << QStringLiteral("2001:db8::1") << denied;
    QTest::newRow("special-6to4-v6") << QStringLiteral("2002::1") << denied;
    QTest::newRow("special-as112-v6") << QStringLiteral("2620:4f:8000::1") << denied;
    QTest::newRow("documentation-3fff-v6") << QStringLiteral("3fff::1") << denied;
    QTest::newRow("private-v6") << QStringLiteral("fd00::1")
                                << static_cast<int>(NetworkAddressClass::Private);
    QTest::newRow("link-local-v6") << QStringLiteral("fe80::1")
                                   << static_cast<int>(NetworkAddressClass::LinkLocal);
}

void NetworkBrokerTest::canonicalizesUnicodeIdnForPolicyDnsAndHostHeader()
{
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    EffectiveNetworkPolicy policy{{NetworkAllowRule{NetworkScheme::Http,
                                                     QStringLiteral("xn--bcher-kva.example"),
                                                     server.serverPort(),
                                                     QStringLiteral("/api"),
                                                     {HttpMethod::Get},
                                                     {NetworkAddressClass::Loopback}}},
                                  8,
                                  32,
                                  1000};
    FixedResolver resolver;
    resolver.addresses = {QHostAddress::LocalHost};
    NetworkBroker broker(policy, resolver);
    const QJsonObject payload{{QStringLiteral("method"), QStringLiteral("GET")},
                              {QStringLiteral("url"),
                               QStringLiteral("http://b\u00fccher.example:%1/api")
                                   .arg(server.serverPort())},
                              {QStringLiteral("bodyBase64"), QString{}}};

    const BrokerResult result = broker.invoke(
        QStringLiteral("request"),
        payload,
        {QStringLiteral("host.identity"), QStringLiteral("request")});

    QVERIFY2(result.ok, qPrintable(result.errorCode));
    QCOMPARE(resolver.lastHost, QStringLiteral("xn--bcher-kva.example"));
    QVERIFY(server.lastRequest.contains(
        QByteArray("\r\nHost: xn--bcher-kva.example:")
        + QByteArray::number(server.serverPort()) + QByteArray("\r\n")));

    QJsonObject trailing = payload;
    trailing.insert(QStringLiteral("url"),
                    QStringLiteral("http://b\u00fccher.example.:%1/api")
                        .arg(server.serverPort()));
    QCOMPARE(broker.invoke(QStringLiteral("request"),
                           trailing,
                           {QStringLiteral("host.identity"), QStringLiteral("request-2")})
                 .errorCode,
             QStringLiteral("network.invalid_request"));
}

void NetworkBrokerTest::classifiesOnlyGloballyRoutableAddressesAsPublic()
{
    QFETCH(QString, address);
    QFETCH(int, expected);
    const auto classification = classifyNetworkAddress(QHostAddress(address));
    QCOMPARE(classification.has_value() ? static_cast<int>(*classification) : -1, expected);
}

QTEST_GUILESS_MAIN(NetworkBrokerTest)
#include "tst_network_broker.moc"
