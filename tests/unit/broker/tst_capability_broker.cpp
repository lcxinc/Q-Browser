#include "CapabilityBroker.h"
#include "ClipboardBroker.h"
#include "FileBroker.h"
#include "FrameCodec.h"
#include "ProtocolMessage.h"
#include "UserGestureGrantStore.h"

#include <QTest>
#include <QBuffer>
#include <QSemaphore>

#include <future>

class RecordingService final : public CapabilityService
{
public:
    BrokerResult invoke(const QString &operation,
                        const QJsonObject &payload,
                        const HostRequestContext &context) override
    {
        ++calls;
        lastOperation = operation;
        lastPayload = payload;
        lastContext = context;
        return BrokerResult::success(responseValue.isEmpty()
                                         ? QJsonObject{{QStringLiteral("called"), true}}
                                         : responseValue);
    }

    int calls = 0;
    QString lastOperation;
    QJsonObject lastPayload;
    HostRequestContext lastContext;
    QJsonObject responseValue;
};

class RecordingClipboard final : public ClipboardBackend
{
public:
    QString readText() override
    {
        ++reads;
        return QStringLiteral("clipboard-value");
    }
    bool writeText(const QString &text) override
    {
        ++writes;
        lastText = text;
        return true;
    }

    int reads = 0;
    int writes = 0;
    QString lastText;
};

class RecordingFileDialog final : public FileDialogBackend
{
public:
    FileDialogResult openFile(qint64 maximumBytes) override
    {
        ++calls;
        lastMaximumBytes = maximumBytes;
        if (block) {
            entered.release();
            proceed.acquire();
        }
        if (status != FileDialogStatus::Opened) {
            return FileDialogResult::error(status);
        }
        auto stream = std::make_unique<QBuffer>();
        stream->setData(content);
        stream->open(QIODevice::ReadOnly);
        return FileDialogResult::opened(QStringLiteral("report.txt"),
                                        content.size(),
                                        std::move(stream));
    }

    int calls = 0;
    qint64 lastMaximumBytes = 0;
    FileDialogStatus status = FileDialogStatus::Cancelled;
    QByteArray content;
    bool block = false;
    QSemaphore entered;
    QSemaphore proceed;
};

class CapabilityBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void deniedCapabilityNeverTouchesService();
    void deniedOperationNeverTouchesService();
    void dispatchesAllowedCapabilityWithHostIdentity();
    void clipboardReadRequiresHostGesture();
    void gestureGrantIsBoundExpiringAndSingleUse();
    void enforcesExactIpcRequestAndResponseBudget();
    void fileCancellationAndSizeAreStable();
    void fileBackendStatusAndConcurrencyAreFailClosed();
};

void CapabilityBrokerTest::deniedCapabilityNeverTouchesService()
{
    RecordingService network;
    CapabilityBroker broker(EffectivePolicy{}, CapabilityServices{&network, nullptr, nullptr, nullptr});

    const BrokerResult result = broker.dispatch(QStringLiteral("network"),
                                                QStringLiteral("request"),
                                                {},
                                                {QStringLiteral("host.identity"), QStringLiteral("r1")});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
    QCOMPARE(network.calls, 0);
}

void CapabilityBrokerTest::deniedOperationNeverTouchesService()
{
    EffectivePolicy policy;
    policy.network = EffectiveNetworkPolicy{};
    RecordingService network;
    CapabilityBroker broker(policy, CapabilityServices{&network, nullptr, nullptr, nullptr});

    const BrokerResult result = broker.dispatch(QStringLiteral("network"),
                                                QStringLiteral("rawSocket"),
                                                {},
                                                {QStringLiteral("host.identity"), QStringLiteral("r1")});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
    QCOMPARE(network.calls, 0);
}

void CapabilityBrokerTest::dispatchesAllowedCapabilityWithHostIdentity()
{
    EffectivePolicy policy;
    policy.storage = EffectiveStoragePolicy{1024};
    RecordingService storage;
    CapabilityBroker broker(policy, CapabilityServices{nullptr, &storage, nullptr, nullptr});
    const QJsonObject payload{{QStringLiteral("key"), QStringLiteral("theme")}};

    const BrokerResult result = broker.dispatch(QStringLiteral("storage"),
                                                QStringLiteral("get"),
                                                payload,
                                                {QStringLiteral("host.identity"), QStringLiteral("r1")});

    QVERIFY(result.ok);
    QCOMPARE(storage.calls, 1);
    QCOMPARE(storage.lastOperation, QStringLiteral("get"));
    QCOMPARE(storage.lastPayload, payload);
    QCOMPARE(storage.lastContext.appIdentity, QStringLiteral("host.identity"));
    QCOMPARE(storage.lastContext.requestId, QStringLiteral("r1"));
}

void CapabilityBrokerTest::clipboardReadRequiresHostGesture()
{
    RecordingClipboard backend;
    UserGestureGrantStore grants;
    ClipboardBroker service(EffectiveClipboardPolicy{true, true}, backend, grants);

    BrokerResult result = service.invoke(QStringLiteral("read"),
                                         {},
                                         {QStringLiteral("host.identity"), QStringLiteral("read-1")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(backend.reads, 0);

    QVERIFY(grants.issue(QStringLiteral("host.identity"), QStringLiteral("read-1"), 1000));
    result = service.invoke(QStringLiteral("read"),
                            {},
                            {QStringLiteral("host.identity"), QStringLiteral("read-1")});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("text")).toString(),
             QStringLiteral("clipboard-value"));
    QCOMPARE(backend.reads, 1);
}

void CapabilityBrokerTest::gestureGrantIsBoundExpiringAndSingleUse()
{
    RecordingClipboard backend;
    UserGestureGrantStore grants;
    ClipboardBroker service(EffectiveClipboardPolicy{false, true}, backend, grants);
    const auto read = [&](const QString &app, const QString &requestId) {
        return service.invoke(QStringLiteral("read"), {}, {app, requestId});
    };

    QVERIFY(grants.issue(QStringLiteral("app.one"), QStringLiteral("request.one"), 1000));
    QCOMPARE(read(QStringLiteral("app.two"), QStringLiteral("request.one")).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("request.two")).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QVERIFY(read(QStringLiteral("app.one"), QStringLiteral("request.one")).ok);
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("request.one")).errorCode,
             QStringLiteral("clipboard.gesture_required"));

    QVERIFY(grants.issue(QStringLiteral("app.one"), QStringLiteral("stale"), 1));
    QTest::qWait(5);
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("stale")).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(backend.reads, 1);
}

void CapabilityBrokerTest::enforcesExactIpcRequestAndResponseBudget()
{
    EffectivePolicy policy;
    policy.storage = EffectiveStoragePolicy{16 * 1024 * 1024};
    RecordingService storage;
    CapabilityBroker broker(policy, CapabilityServices{nullptr, &storage, nullptr, nullptr});
    const HostRequestContext context{QStringLiteral("host.identity"), QString(256, u'r')};

    const QJsonObject emptyPayload{{QStringLiteral("blob"), QString{}}};
    const auto emptyRequest = ProtocolMessage::request(context.requestId,
                                                       QStringLiteral("storage"),
                                                       QStringLiteral("set"),
                                                       emptyPayload);
    QVERIFY(emptyRequest.has_value());
    const qsizetype emptyRequestBytes = FrameCodec::encode(emptyRequest->toJson()).size() - 4;
    const qsizetype exactRequestCharacters =
        static_cast<qsizetype>(FrameCodec::maximumPayloadBytes()) - emptyRequestBytes;
    QJsonObject exactRequest{{QStringLiteral("blob"), QString(exactRequestCharacters, u'x')}};
    QVERIFY(broker.dispatch(QStringLiteral("storage"),
                            QStringLiteral("set"),
                            exactRequest,
                            context)
                .ok);
    QCOMPARE(storage.calls, 1);

    exactRequest.insert(QStringLiteral("blob"), QString(exactRequestCharacters + 1, u'x'));
    BrokerResult result = broker.dispatch(QStringLiteral("storage"),
                                          QStringLiteral("set"),
                                          exactRequest,
                                          context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.payload_too_large"));
    QCOMPARE(storage.calls, 1);

    const auto emptyResponse = ProtocolMessage::successResponse(
        context.requestId,
        QJsonObject{{QStringLiteral("blob"), QString{}}});
    QVERIFY(emptyResponse.has_value());
    const qsizetype emptyResponseBytes = FrameCodec::encode(emptyResponse->toJson()).size() - 4;
    const qsizetype exactResponseCharacters =
        static_cast<qsizetype>(FrameCodec::maximumPayloadBytes()) - emptyResponseBytes;
    storage.responseValue =
        QJsonObject{{QStringLiteral("blob"), QString(exactResponseCharacters, u'y')}};
    result = broker.dispatch(QStringLiteral("storage"),
                             QStringLiteral("get"),
                             {},
                             context);
    QVERIFY(result.ok);

    storage.responseValue =
        QJsonObject{{QStringLiteral("blob"), QString(exactResponseCharacters + 1, u'y')}};
    result = broker.dispatch(QStringLiteral("storage"),
                             QStringLiteral("get"),
                             {},
                             context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.response_too_large"));
}

void CapabilityBrokerTest::fileCancellationAndSizeAreStable()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 4}, backend);
    const HostRequestContext context{QStringLiteral("host.identity"), QStringLiteral("file-1")};

    BrokerResult result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("file.cancelled"));
    QCOMPARE(backend.calls, 1);
    QCOMPARE(backend.lastMaximumBytes, 4);

    backend.status = FileDialogStatus::TooLarge;
    result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("file.too_large"));

    backend.status = FileDialogStatus::Opened;
    backend.content = QByteArray("1234");
    result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("name")).toString(),
             QStringLiteral("report.txt"));
    QCOMPARE(QByteArray::fromBase64(
                 result.value.value(QStringLiteral("contentBase64")).toString().toLatin1()),
             QByteArray("1234"));
}

void CapabilityBrokerTest::fileBackendStatusAndConcurrencyAreFailClosed()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("file-1")};

    backend.status = FileDialogStatus::Failed;
    QCOMPARE(service.invoke(QStringLiteral("open"), {}, context).errorCode,
             QStringLiteral("file.failed"));

    backend.status = FileDialogStatus::Opened;
    backend.content = QByteArray("stable");
    backend.block = true;
    auto first = std::async(std::launch::async, [&] {
        return service.invoke(QStringLiteral("open"), {}, context);
    });
    QVERIFY(backend.entered.tryAcquire(1, 1000));
    const BrokerResult concurrent = service.invoke(QStringLiteral("open"), {}, context);
    QCOMPARE(concurrent.errorCode, QStringLiteral("file.failed"));
    QCOMPARE(backend.calls, 2); // failed status call plus one active dialog
    backend.proceed.release();
    QVERIFY(first.get().ok);
}

QTEST_GUILESS_MAIN(CapabilityBrokerTest)
#include "tst_capability_broker.moc"
