#include "FrameCodec.h"

#include <QJsonDocument>
#include <QTest>

namespace {

QByteArray frame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray encoded(4, Qt::Uninitialized);
    const auto size = static_cast<quint32>(payload.size());
    encoded[0] = static_cast<char>((size >> 24U) & 0xffU);
    encoded[1] = static_cast<char>((size >> 16U) & 0xffU);
    encoded[2] = static_cast<char>((size >> 8U) & 0xffU);
    encoded[3] = static_cast<char>(size & 0xffU);
    return encoded + payload;
}

QByteArray rawFrame(const QByteArray &payload)
{
    QByteArray encoded(4, Qt::Uninitialized);
    const auto size = static_cast<quint32>(payload.size());
    encoded[0] = static_cast<char>((size >> 24U) & 0xffU);
    encoded[1] = static_cast<char>((size >> 16U) & 0xffU);
    encoded[2] = static_cast<char>((size >> 8U) & 0xffU);
    encoded[3] = static_cast<char>(size & 0xffU);
    return encoded + payload;
}

} // namespace

class FrameCodecTest final : public QObject
{
    Q_OBJECT

private slots:
    void encodesBigEndianLengthAndCompactUtf8Json();
    void acceptsEveryFragmentBoundary();
    void returnsAllCoalescedFrames();
    void rejectsInvalidLengths_data();
    void rejectsInvalidLengths();
    void rejectsInvalidPayload_data();
    void rejectsInvalidPayload();
    void failsClosedAndBoundsQueuedBytes();
    void boundsDecodedFrameCount();
    void consumesCompletedMaximumFrameBeforeAppendingCoalescedFrame();
    void enforcesJsonNestingBeforeDom_data();
    void enforcesJsonNestingBeforeDom();
    void enforcesAggregateJsonEntriesBeforeDom_data();
    void enforcesAggregateJsonEntriesBeforeDom();
};

void FrameCodecTest::encodesBigEndianLengthAndCompactUtf8Json()
{
    const QJsonObject object{{QStringLiteral("text"), QString::fromUtf8("â")}};
    const QByteArray encoded = FrameCodec::encode(object);
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QCOMPARE(encoded.size(), payload.size() + 4);
    QCOMPARE(static_cast<quint8>(encoded[0]), quint8(0));
    QCOMPARE(static_cast<quint8>(encoded[1]), quint8(0));
    QCOMPARE(static_cast<quint8>(encoded[2]), quint8(0));
    QCOMPARE(static_cast<quint8>(encoded[3]), static_cast<quint8>(payload.size()));
    QCOMPARE(encoded.sliced(4), payload);
}

void FrameCodecTest::acceptsEveryFragmentBoundary()
{
    const QJsonObject object{{QStringLiteral("protocolVersion"), 1},
                             {QStringLiteral("type"), QStringLiteral("heartbeat")},
                             {QStringLiteral("payload"), QJsonObject{}}};
    const QByteArray encoded = frame(object);

    for (qsizetype split = 0; split < encoded.size(); ++split) {
        FrameCodec codec;
        QCOMPARE(codec.feed(encoded.first(split)).status, FrameStatus::NeedMoreData);
        const FrameFeedResult result = codec.feed(encoded.sliced(split));
        QCOMPARE(result.status, FrameStatus::FramesReady);
        QCOMPARE(result.frames, QList<QJsonObject>{object});
        QCOMPARE(codec.queuedBytes(), qsizetype(0));
    }
}

void FrameCodecTest::returnsAllCoalescedFrames()
{
    const QJsonObject first{{QStringLiteral("value"), 1}};
    const QJsonObject second{{QStringLiteral("value"), 2}};
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(frame(first) + frame(second));

    QCOMPARE(result.status, FrameStatus::FramesReady);
    QCOMPARE(result.frames, (QList<QJsonObject>{first, second}));
}

void FrameCodecTest::rejectsInvalidLengths_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<FrameError>("error");

    QTest::newRow("zero") << QByteArray(4, '\0') << FrameError::ZeroLength;
    QByteArray oversized(4, '\0');
    const quint32 size = FrameCodec::maximumPayloadBytes() + 1U;
    oversized[0] = static_cast<char>((size >> 24U) & 0xffU);
    oversized[1] = static_cast<char>((size >> 16U) & 0xffU);
    oversized[2] = static_cast<char>((size >> 8U) & 0xffU);
    oversized[3] = static_cast<char>(size & 0xffU);
    QTest::newRow("oversized") << oversized << FrameError::PayloadTooLarge;
}

void FrameCodecTest::rejectsInvalidLengths()
{
    QFETCH(QByteArray, bytes);
    QFETCH(FrameError, error);
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(bytes);

    QCOMPARE(result.status, FrameStatus::Failed);
    QCOMPARE(result.error, error);
    QVERIFY(codec.isFailed());
    QCOMPARE(codec.queuedBytes(), qsizetype(0));
}

void FrameCodecTest::rejectsInvalidPayload_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<FrameError>("error");

    QTest::newRow("invalid-utf8") << QByteArray("{\"x\":\"\xC3\x28\"}")
                                   << FrameError::InvalidUtf8;
    QTest::newRow("invalid-json") << QByteArray("{\"x\":") << FrameError::InvalidJson;
    QTest::newRow("json-array") << QByteArray("[]") << FrameError::RootNotObject;
    QTest::newRow("duplicate-member") << QByteArray("{\"x\":1,\"\\u0078\":2}")
                                        << FrameError::DuplicateMember;
}

void FrameCodecTest::rejectsInvalidPayload()
{
    QFETCH(QByteArray, payload);
    QFETCH(FrameError, error);
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(rawFrame(payload));

    QCOMPARE(result.status, FrameStatus::Failed);
    QCOMPARE(result.error, error);
}

void FrameCodecTest::failsClosedAndBoundsQueuedBytes()
{
    FrameCodec codec;
    QByteArray header(4, '\0');
    const quint32 size = FrameCodec::maximumPayloadBytes();
    header[0] = static_cast<char>((size >> 24U) & 0xffU);
    header[1] = static_cast<char>((size >> 16U) & 0xffU);
    header[2] = static_cast<char>((size >> 8U) & 0xffU);
    header[3] = static_cast<char>(size & 0xffU);

    QCOMPARE(codec.feed(header + QByteArray(static_cast<qsizetype>(size - 1U), 'x')).status,
             FrameStatus::NeedMoreData);
    QVERIFY(codec.queuedBytes() <= FrameCodec::maximumQueuedBytes());
    QCOMPARE(codec.feed(QByteArray(2, 'x')).status, FrameStatus::Failed);
    QCOMPARE(codec.lastError(), FrameError::InvalidJson);
    QCOMPARE(codec.queuedBytes(), qsizetype(0));
    QCOMPARE(codec.feed(frame(QJsonObject{{QStringLiteral("ok"), true}})).status,
             FrameStatus::Failed);
    QCOMPARE(codec.lastErrorCode(), QStringLiteral("ipc.frame.invalid_json"));
}

void FrameCodecTest::boundsDecodedFrameCount()
{
    const QByteArray heartbeat = frame(QJsonObject{{QStringLiteral("value"), true}});
    QByteArray coalesced;
    for (qsizetype index = 0; index <= FrameCodec::maximumFramesPerFeed(); ++index) {
        coalesced.append(heartbeat);
    }
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(coalesced);

    QCOMPARE(result.status, FrameStatus::Failed);
    QCOMPARE(result.error, FrameError::QueueLimitExceeded);
    QCOMPARE(result.errorCode, QStringLiteral("ipc.frame.queue_limit"));
}

void FrameCodecTest::consumesCompletedMaximumFrameBeforeAppendingCoalescedFrame()
{
    const QByteArray maximumPayload = QByteArrayLiteral("{\"padding\":\"")
        + QByteArray(static_cast<qsizetype>(FrameCodec::maximumPayloadBytes()) - 14, 'x')
        + QByteArrayLiteral("\"}");
    QCOMPARE(maximumPayload.size(),
             static_cast<qsizetype>(FrameCodec::maximumPayloadBytes()));
    const QByteArray maximumFrame = rawFrame(maximumPayload);
    const QJsonObject next{{QStringLiteral("next"), true}};
    FrameCodec codec;

    QCOMPARE(codec.feed(maximumFrame.first(maximumFrame.size() - 1)).status,
             FrameStatus::NeedMoreData);
    const FrameFeedResult result = codec.feed(maximumFrame.last(1) + frame(next));

    QCOMPARE(result.status, FrameStatus::FramesReady);
    QCOMPARE(result.frames.size(), 2);
    QCOMPARE(result.frames.at(1), next);
    QCOMPARE(codec.queuedBytes(), qsizetype(0));
}

void FrameCodecTest::enforcesJsonNestingBeforeDom_data()
{
    QTest::addColumn<qsizetype>("arrayDepth");
    QTest::addColumn<FrameStatus>("status");
    QTest::addColumn<FrameError>("error");

    QTest::newRow("exact-limit") << FrameCodec::maximumJsonNesting() - 1
                                  << FrameStatus::FramesReady << FrameError::None;
    QTest::newRow("limit-plus-one") << FrameCodec::maximumJsonNesting()
                                     << FrameStatus::Failed
                                     << FrameError::JsonResourceLimit;
}

void FrameCodecTest::enforcesJsonNestingBeforeDom()
{
    QFETCH(qsizetype, arrayDepth);
    QFETCH(FrameStatus, status);
    QFETCH(FrameError, error);
    const QByteArray payload = QByteArrayLiteral("{\"value\":")
        + QByteArray(arrayDepth, '[') + QByteArrayLiteral("0")
        + QByteArray(arrayDepth, ']') + QByteArrayLiteral("}");
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(rawFrame(payload));

    QCOMPARE(result.status, status);
    QCOMPARE(result.error, error);
}

void FrameCodecTest::enforcesAggregateJsonEntriesBeforeDom_data()
{
    QTest::addColumn<qsizetype>("elementCount");
    QTest::addColumn<FrameStatus>("status");
    QTest::addColumn<FrameError>("error");

    QTest::newRow("exact-limit") << FrameCodec::maximumJsonAggregateEntries() - 1
                                  << FrameStatus::FramesReady << FrameError::None;
    QTest::newRow("limit-plus-one") << FrameCodec::maximumJsonAggregateEntries()
                                     << FrameStatus::Failed
                                     << FrameError::JsonResourceLimit;
}

void FrameCodecTest::enforcesAggregateJsonEntriesBeforeDom()
{
    QFETCH(qsizetype, elementCount);
    QFETCH(FrameStatus, status);
    QFETCH(FrameError, error);
    QByteArray payload = QByteArrayLiteral("{\"items\":[");
    for (qsizetype index = 0; index < elementCount; ++index) {
        if (index != 0) {
            payload.append(',');
        }
        payload.append('0');
    }
    payload.append(QByteArrayLiteral("]}"));
    FrameCodec codec;

    const FrameFeedResult result = codec.feed(rawFrame(payload));

    QCOMPARE(result.status, status);
    QCOMPARE(result.error, error);
}

QTEST_MAIN(FrameCodecTest)

#include "tst_frame_codec.moc"
