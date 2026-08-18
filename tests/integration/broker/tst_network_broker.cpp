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
};

class FixedResolver final : public NetworkAddressResolver
{
public:
    QList<QHostAddress> resolve(const QString &, int, bool &timedOut) override
    {
        timedOut = false;
        return addresses;
    }

    QList<QHostAddress> addresses;
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

QTEST_GUILESS_MAIN(NetworkBrokerTest)
#include "tst_network_broker.moc"
