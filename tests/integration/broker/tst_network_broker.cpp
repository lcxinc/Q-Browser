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

namespace {

EffectiveNetworkPolicy policyFor(const LocalHttpServer &)
{
    return EffectiveNetworkPolicy{{NetworkAllowRule{QStringLiteral("127.0.0.1"),
                                                     QStringLiteral("/api"),
                                                     {HttpMethod::Get, HttpMethod::Post}}},
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
};

void NetworkBrokerTest::performsAllowedRequestAgainstRealServer()
{
    LocalHttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    NetworkBroker broker(policyFor(server));

    const BrokerResult result = broker.invoke(QStringLiteral("request"),
                                              requestPayload(QStringLiteral("GET"),
                                                             server.url(QStringLiteral("/api/orders"))),
                                              {QStringLiteral("host.identity"), false});

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
                                        {QStringLiteral("host.identity"), false});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.host_denied"));

    result = broker.invoke(QStringLiteral("request"),
                           requestPayload(QStringLiteral("PUT"),
                                          server.url(QStringLiteral("/api"))),
                           {QStringLiteral("host.identity"), false});
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
                                        {QStringLiteral("host.identity"), false});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.payload_too_large"));
    QCOMPARE(server.requestCount, 0);

    server.responseBody = QByteArray(33, 'y');
    result = broker.invoke(QStringLiteral("request"),
                           requestPayload(QStringLiteral("GET"),
                                          server.url(QStringLiteral("/api"))),
                           {QStringLiteral("host.identity"), false});
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
                                              {QStringLiteral("host.identity"), false});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("network.timeout"));
    QCOMPARE(server.requestCount, 1);
}

QTEST_GUILESS_MAIN(NetworkBrokerTest)
#include "tst_network_broker.moc"
