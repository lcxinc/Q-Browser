#include "ProtocolMessage.h"

#include <QJsonArray>
#include <QTest>

class ProtocolMessageTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesValidMessages_data();
    void parsesValidMessages();
    void rejectsEnvelopeViolations_data();
    void rejectsEnvelopeViolations();
    void rejectsTypedPayloadViolations_data();
    void rejectsTypedPayloadViolations();
    void factoriesAreValidByConstruction();
    void factoriesRejectInvalidArguments();
};

void ProtocolMessageTest::parsesValidMessages_data()
{
    QTest::addColumn<QJsonObject>("object");
    QTest::addColumn<ProtocolType>("type");

    QTest::newRow("handshake")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("handshake")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("nonce"), QStringLiteral("launch-token")}}}}
        << ProtocolType::Handshake;
    QTest::newRow("request")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("request")},
                       {QStringLiteral("requestId"), QStringLiteral("req-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("capability"), QStringLiteral("storage")},
                                    {QStringLiteral("operation"), QStringLiteral("get")},
                                    {QStringLiteral("payload"), QJsonObject{}}}}}
        << ProtocolType::Request;
    QTest::newRow("success-response")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("response")},
                       {QStringLiteral("requestId"), QStringLiteral("req-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("ok"), true},
                                    {QStringLiteral("result"), QJsonObject{}}}}}
        << ProtocolType::Response;
    QTest::newRow("heartbeat")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolType::Heartbeat;
    QTest::newRow("surface-ready")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("surfaceReady")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("windowHandle"),
                                     QStringLiteral("123456")}}}}
        << ProtocolType::SurfaceReady;
    QTest::newRow("route-load")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("routeLoad")},
                       {QStringLiteral("requestId"), QStringLiteral("route-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("route"), QStringLiteral("/orders/42")}}}}
        << ProtocolType::RouteLoad;
    QTest::newRow("structured-log")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("structuredLog")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("level"), QStringLiteral("warning")},
                                    {QStringLiteral("category"), QStringLiteral("qml")},
                                    {QStringLiteral("message"), QStringLiteral("binding failed")}}}}
        << ProtocolType::StructuredLog;
}

void ProtocolMessageTest::parsesValidMessages()
{
    QFETCH(QJsonObject, object);
    QFETCH(ProtocolType, type);

    const ProtocolParseResult result = ProtocolMessage::parse(object);

    QVERIFY(result.message.has_value());
    QCOMPARE(result.message->type(), type);
    QCOMPARE(result.message->toJson(), object);
    QCOMPARE(result.error, ProtocolError::None);
}

void ProtocolMessageTest::rejectsEnvelopeViolations_data()
{
    QTest::addColumn<QJsonObject>("object");
    QTest::addColumn<ProtocolError>("error");

    const QJsonObject payload{{QStringLiteral("payload"), QJsonObject{}}};
    QTest::newRow("missing-version")
        << QJsonObject{{QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolError::InvalidEnvelope;
    QTest::newRow("unknown-version")
        << QJsonObject{{QStringLiteral("protocolVersion"), 2},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolError::UnsupportedVersion;
    QTest::newRow("fractional-version")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1.5},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolError::InvalidEnvelope;
    QTest::newRow("unknown-type")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("surprise")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolError::UnknownType;
    QTest::newRow("request-missing-id")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("request")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("capability"), QStringLiteral("storage")},
                                    {QStringLiteral("operation"), QStringLiteral("get")},
                                    {QStringLiteral("payload"), QJsonObject{}}}}}
        << ProtocolError::InvalidRequestId;
    QTest::newRow("heartbeat-with-id")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("requestId"), QStringLiteral("req-1")},
                       {QStringLiteral("payload"), QJsonObject{}}}
        << ProtocolError::UnexpectedRequestId;
    QTest::newRow("unknown-envelope-field")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{}},
                       {QStringLiteral("extra"), true}}
        << ProtocolError::InvalidEnvelope;
}

void ProtocolMessageTest::rejectsEnvelopeViolations()
{
    QFETCH(QJsonObject, object);
    QFETCH(ProtocolError, error);

    const ProtocolParseResult result = ProtocolMessage::parse(object);

    QVERIFY(!result.message.has_value());
    QCOMPARE(result.error, error);
    QVERIFY(!result.errorCode.isEmpty());
}

void ProtocolMessageTest::rejectsTypedPayloadViolations_data()
{
    QTest::addColumn<QJsonObject>("object");

    QTest::newRow("handshake-empty-nonce")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("handshake")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("nonce"), QString()}}}};
    QTest::newRow("handshake-claimed-identity")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("handshake")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("nonce"), QStringLiteral("n")},
                                    {QStringLiteral("appId"), QStringLiteral("attacker")}}}};
    QTest::newRow("request-nonobject-payload")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("request")},
                       {QStringLiteral("requestId"), QStringLiteral("req-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("capability"), QStringLiteral("storage")},
                                    {QStringLiteral("operation"), QStringLiteral("get")},
                                    {QStringLiteral("payload"), QJsonArray{}}}}};
    QTest::newRow("response-both-result-error")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("response")},
                       {QStringLiteral("requestId"), QStringLiteral("req-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("ok"), true},
                                    {QStringLiteral("result"), QJsonObject{}},
                                    {QStringLiteral("error"), QJsonObject{}}}}};
    QTest::newRow("heartbeat-nonempty")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("heartbeat")},
                       {QStringLiteral("payload"), QJsonObject{{QStringLiteral("x"), 1}}}};
}

void ProtocolMessageTest::rejectsTypedPayloadViolations()
{
    QFETCH(QJsonObject, object);

    const ProtocolParseResult result = ProtocolMessage::parse(object);

    QVERIFY(!result.message.has_value());
    QCOMPARE(result.error, ProtocolError::InvalidPayload);
    QCOMPARE(result.errorCode, QStringLiteral("ipc.protocol.invalid_payload"));
}

void ProtocolMessageTest::factoriesAreValidByConstruction()
{
    const ProtocolMessage handshake = *ProtocolMessage::handshake(QStringLiteral("nonce"));
    const ProtocolMessage request = *ProtocolMessage::request(
        QStringLiteral("req-1"), QStringLiteral("network"), QStringLiteral("fetch"),
        QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.test")}});
    const ProtocolMessage response = *ProtocolMessage::successResponse(
        QStringLiteral("req-1"), QJsonObject{{QStringLiteral("status"), 200}});
    const ProtocolMessage heartbeat = ProtocolMessage::heartbeat();
    const ProtocolMessage surfaceReady = *ProtocolMessage::surfaceReady(QStringLiteral("123456"));
    const ProtocolMessage routeLoad = *ProtocolMessage::routeLoad(QStringLiteral("route-1"),
                                                                  QStringLiteral("/orders"));
    const ProtocolMessage structuredLog = *ProtocolMessage::structuredLog(
        QStringLiteral("info"), QStringLiteral("worker"), QStringLiteral("ready"));
    const ProtocolMessage shutdown = *ProtocolMessage::shutdown(QStringLiteral("host.request"));

    for (const auto &message : {handshake,
                                request,
                                response,
                                heartbeat,
                                surfaceReady,
                                routeLoad,
                                structuredLog,
                                shutdown}) {
        const auto reparsed = ProtocolMessage::parse(message.toJson());
        QVERIFY(reparsed.message.has_value());
        QCOMPARE(reparsed.message->toJson(), message.toJson());
    }
    QVERIFY(!ProtocolMessage::surfaceReady(QStringLiteral("0")).has_value());
}

void ProtocolMessageTest::factoriesRejectInvalidArguments()
{
    QVERIFY(!ProtocolMessage::handshake(QString()).has_value());
    QVERIFY(!ProtocolMessage::handshake(QString(257, u'x')).has_value());
    QVERIFY(!ProtocolMessage::request(QString(), QStringLiteral("storage"),
                                      QStringLiteral("get"), {}).has_value());
    QVERIFY(!ProtocolMessage::request(QStringLiteral("req"), QString(),
                                      QStringLiteral("get"), {}).has_value());
    QVERIFY(!ProtocolMessage::request(QStringLiteral("req"), QStringLiteral("storage"),
                                      QString(), {}).has_value());
    QVERIFY(!ProtocolMessage::request(QStringLiteral("bad\nrequest"),
                                      QStringLiteral("storage"), QStringLiteral("get"), {})
                 .has_value());
    QVERIFY(!ProtocolMessage::successResponse(QString(), {}).has_value());
    QVERIFY(!ProtocolMessage::errorResponse(QStringLiteral("req"), QString(), QString())
                 .has_value());
    QVERIFY(!ProtocolMessage::shutdown(QString()).has_value());
    QVERIFY(!ProtocolMessage::routeLoad(QStringLiteral("req"), QStringLiteral("relative"))
                 .has_value());
    QVERIFY(!ProtocolMessage::structuredLog(QStringLiteral("fatal"), QStringLiteral("qml"),
                                            QStringLiteral("message"))
                 .has_value());
}

QTEST_MAIN(ProtocolMessageTest)

#include "tst_protocol_message.moc"
