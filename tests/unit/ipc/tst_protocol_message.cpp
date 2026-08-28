#include "FrameCodec.h"
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
    void versionOneUnknownAdditionsStillFailClosed();
    void frozenLegacyVersionOneDecoderRejectsActualPageMetadataFrame();
    void parsesVisibilityChangedWithExactActivePayload();
    void rejectsVisibilityChangedPayloadViolations_data();
    void rejectsVisibilityChangedPayloadViolations();
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
    QTest::newRow("navigation-request")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("navigationRequest")},
                       {QStringLiteral("requestId"), QStringLiteral("navigate-1")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("route"), QStringLiteral("/orders/42")}}}}
        << ProtocolType::NavigationRequest;
    QTest::newRow("structured-log")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("structuredLog")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("level"), QStringLiteral("warning")},
                                    {QStringLiteral("category"), QStringLiteral("qml")},
                                    {QStringLiteral("message"), QStringLiteral("binding failed")}}}}
        << ProtocolType::StructuredLog;
    QTest::newRow("page-metadata-title")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("pageMetadata")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("title"),
                                     QStringLiteral("Orders")}}}}
        << ProtocolType::PageMetadata;
    QTest::newRow("page-metadata-status")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("pageMetadata")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("title"),
                                     QStringLiteral("Order details")},
                                    {QStringLiteral("status"),
                                     QStringLiteral("ready")}}}}
        << ProtocolType::PageMetadata;
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
    QTest::newRow("page-metadata-with-request-id")
        << QJsonObject{{QStringLiteral("protocolVersion"), 1},
                       {QStringLiteral("type"), QStringLiteral("pageMetadata")},
                       {QStringLiteral("requestId"), QStringLiteral("forbidden")},
                       {QStringLiteral("payload"),
                        QJsonObject{{QStringLiteral("title"), QStringLiteral("Orders")}}}}
        << ProtocolError::UnexpectedRequestId;
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

    QString loneHighSurrogate;
    loneHighSurrogate.append(QChar(0xd800));

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
    const auto pageMetadata = [](const QJsonObject &payload) {
        return QJsonObject{{QStringLiteral("protocolVersion"), 1},
                           {QStringLiteral("type"), QStringLiteral("pageMetadata")},
                           {QStringLiteral("payload"), payload}};
    };
    QTest::newRow("page-metadata-missing-title") << pageMetadata({});
    QTest::newRow("page-metadata-unknown-key")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("route"), QStringLiteral("/orders")}});
    QTest::newRow("page-metadata-non-string-title")
        << pageMetadata({{QStringLiteral("title"), 7}});
    QTest::newRow("page-metadata-overlong-title")
        << pageMetadata({{QStringLiteral("title"), QString(257, u'x')}});
    QTest::newRow("page-metadata-control-title")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders\n")}});
    QTest::newRow("page-metadata-bidi-title")
        << pageMetadata({{QStringLiteral("title"),
                          QStringLiteral("Orders\u202e")}});
    QTest::newRow("page-metadata-deprecated-bidi-title")
        << pageMetadata({{QStringLiteral("title"),
                          QStringLiteral("Orders\u206f")}});
    QTest::newRow("page-metadata-html-title")
        << pageMetadata({{QStringLiteral("title"),
                          QStringLiteral("<b>Orders</b>")}});
    QTest::newRow("page-metadata-lone-surrogate")
        << pageMetadata({{QStringLiteral("title"), loneHighSurrogate}});
    QTest::newRow("page-metadata-empty-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QString()}});
    QTest::newRow("page-metadata-overlong-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QString(33, u'a')}});
    QTest::newRow("page-metadata-non-ascii-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("r\u00e9ady")}});
    QTest::newRow("page-metadata-non-token-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("not ready")}});
    QTest::newRow("page-metadata-admin-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("admin")}});
    QTest::newRow("page-metadata-trusted-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("trusted")}});
    QTest::newRow("page-metadata-loading-suffix-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("loading-1")}});
    QTest::newRow("page-metadata-uppercase-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("READY")}});
    QTest::newRow("page-metadata-case-variant-status")
        << pageMetadata({{QStringLiteral("title"), QStringLiteral("Orders")},
                         {QStringLiteral("status"), QStringLiteral("Loading")}});
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
    const ProtocolMessage navigationRequest = *ProtocolMessage::navigationRequest(
        QStringLiteral("navigate-1"), QStringLiteral("/orders/42"));
    const ProtocolMessage structuredLog = *ProtocolMessage::structuredLog(
        QStringLiteral("info"), QStringLiteral("worker"), QStringLiteral("ready"));
    const ProtocolMessage shutdown = *ProtocolMessage::shutdown(QStringLiteral("host.request"));
    const ProtocolMessage pageMetadata = *ProtocolMessage::pageMetadata(
        QString(254, u'x') + QString::fromUcs4(U"\U0001f680"),
        QStringLiteral("loading"));
    const ProtocolMessage pageMetadataWithoutStatus = *ProtocolMessage::pageMetadata(
        QStringLiteral("Orders"));

    for (const auto &message : {handshake,
                                request,
                                response,
                                heartbeat,
                                surfaceReady,
                                routeLoad,
                                navigationRequest,
                                structuredLog,
                                pageMetadata,
                                pageMetadataWithoutStatus,
                                shutdown}) {
        const auto reparsed = ProtocolMessage::parse(message.toJson());
        QVERIFY(reparsed.message.has_value());
        QCOMPARE(reparsed.message->toJson(), message.toJson());
    }
    QVERIFY(!ProtocolMessage::surfaceReady(QStringLiteral("0")).has_value());
}

void ProtocolMessageTest::factoriesRejectInvalidArguments()
{
    QString loneHighSurrogate;
    loneHighSurrogate.append(QChar(0xd800));
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
    QVERIFY(!ProtocolMessage::navigationRequest(QStringLiteral("req"),
                                                 QStringLiteral("https://evil.test"))
                 .has_value());
    QVERIFY(!ProtocolMessage::navigationRequest(QStringLiteral("req"),
                                                 QStringLiteral("//other-app/orders"))
                 .has_value());
    QVERIFY(!ProtocolMessage::structuredLog(QStringLiteral("fatal"), QStringLiteral("qml"),
                                            QStringLiteral("message"))
                 .has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QString()).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QString(257, u'x')).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders\n")).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders\u202e")).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders\u206f")).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("<b>Orders</b>")).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(loneHighSurrogate).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders"),
                                           QString(33, u'a')).has_value());
    QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders"),
                                           QStringLiteral("not ready")).has_value());
    for (const QString &status : {QStringLiteral("admin"),
                                  QStringLiteral("trusted"),
                                  QStringLiteral("loading-1"),
                                  QStringLiteral("READY"),
                                  QStringLiteral("Loading")}) {
        QVERIFY(!ProtocolMessage::pageMetadata(QStringLiteral("Orders"), status)
                     .has_value());
    }
}

void ProtocolMessageTest::versionOneUnknownAdditionsStillFailClosed()
{
    QCOMPARE(ProtocolMessage::currentVersion(), 1);
    const ProtocolParseResult result = ProtocolMessage::parse(
        QJsonObject{{QStringLiteral("protocolVersion"), 1},
                    {QStringLiteral("type"), QStringLiteral("futurePageMetadata")},
                    {QStringLiteral("payload"), QJsonObject{}}});
    QVERIFY(!result.message.has_value());
    QCOMPARE(result.error, ProtocolError::UnknownType);
    QCOMPARE(result.errorCode, QStringLiteral("ipc.protocol.unknown_type"));
}

void ProtocolMessageTest::frozenLegacyVersionOneDecoderRejectsActualPageMetadataFrame()
{
    enum class LegacyDecodeResult {
        AcceptedType,
        UnknownType,
        InvalidEnvelope,
    };
    const QStringList frozenLegacyTypes{
        QStringLiteral("handshake"),
        QStringLiteral("handshakeAck"),
        QStringLiteral("surfaceReady"),
        QStringLiteral("routeLoad"),
        QStringLiteral("navigationRequest"),
        QStringLiteral("ready"),
        QStringLiteral("request"),
        QStringLiteral("response"),
        QStringLiteral("heartbeat"),
        QStringLiteral("structuredLog"),
        QStringLiteral("shutdown"),
    };
    const auto legacyDecode = [&frozenLegacyTypes](const QJsonObject &object) {
        const QJsonValue version = object.value(QStringLiteral("protocolVersion"));
        const QJsonValue type = object.value(QStringLiteral("type"));
        const QJsonValue payload = object.value(QStringLiteral("payload"));
        if (!version.isDouble() || version.toInt(-1) != 1 || !type.isString()
            || !payload.isObject()) {
            return LegacyDecodeResult::InvalidEnvelope;
        }
        return frozenLegacyTypes.contains(type.toString())
            ? LegacyDecodeResult::AcceptedType
            : LegacyDecodeResult::UnknownType;
    };

    for (const QString &legacyType : frozenLegacyTypes) {
        QCOMPARE(legacyDecode(
                     QJsonObject{{QStringLiteral("protocolVersion"), 1},
                                 {QStringLiteral("type"), legacyType},
                                 {QStringLiteral("payload"), QJsonObject{}}}),
                 LegacyDecodeResult::AcceptedType);
    }

    const auto metadata = ProtocolMessage::pageMetadata(
        QStringLiteral("Orders"), QStringLiteral("ready"));
    QVERIFY(metadata.has_value());
    const QByteArray actualFrame = FrameCodec::encode(metadata->toJson());
    QVERIFY(!actualFrame.isEmpty());
    FrameCodec legacyFraming;
    const FrameFeedResult framed = legacyFraming.feed(actualFrame);
    QCOMPARE(framed.status, FrameStatus::FramesReady);
    QCOMPARE(framed.frames.size(), 1);
    QCOMPARE(framed.frames.constFirst(), metadata->toJson());
    QCOMPARE(legacyDecode(framed.frames.constFirst()),
             LegacyDecodeResult::UnknownType);
}

void ProtocolMessageTest::parsesVisibilityChangedWithExactActivePayload()
{
    const auto message = ProtocolMessage::visibilityChanged(true);
    QVERIFY(message.has_value());
    QCOMPARE(message->type(), ProtocolType::VisibilityChanged);
    const QJsonObject expected{{QStringLiteral("active"), true}};
    QCOMPARE(message->payload(), expected);

    const ProtocolParseResult parsed = ProtocolMessage::parse(message->toJson());
    QVERIFY(parsed.message.has_value());
    QCOMPARE(parsed.message->toJson(), message->toJson());
}

void ProtocolMessageTest::rejectsVisibilityChangedPayloadViolations_data()
{
    QTest::addColumn<QJsonObject>("object");

    const auto frame = [](const QJsonObject &payload) {
        return QJsonObject{{QStringLiteral("protocolVersion"), 1},
                           {QStringLiteral("type"), QStringLiteral("visibilityChanged")},
                           {QStringLiteral("payload"), payload}};
    };
    QTest::newRow("missing-active") << frame({});
    QTest::newRow("non-bool-active")
        << frame({{QStringLiteral("active"), QStringLiteral("true")}});
    QTest::newRow("extra-key")
        << frame({{QStringLiteral("active"), true}, {QStringLiteral("tab"), 1}});
}

void ProtocolMessageTest::rejectsVisibilityChangedPayloadViolations()
{
    QFETCH(QJsonObject, object);
    const ProtocolParseResult result = ProtocolMessage::parse(object);
    QVERIFY(!result.message.has_value());
    QCOMPARE(result.error, ProtocolError::InvalidPayload);
    QCOMPARE(result.errorCode, QStringLiteral("ipc.protocol.invalid_payload"));
}

QTEST_MAIN(ProtocolMessageTest)

#include "tst_protocol_message.moc"
