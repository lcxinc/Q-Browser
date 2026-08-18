#include "CapabilityBroker.h"
#include "ClipboardBroker.h"
#include "FileBroker.h"

#include <QTest>

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
        return BrokerResult::success(QJsonObject{{QStringLiteral("called"), true}});
    }

    int calls = 0;
    QString lastOperation;
    QJsonObject lastPayload;
    HostRequestContext lastContext;
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
    std::optional<SelectedFile> openFile(qint64 maximumBytes) override
    {
        ++calls;
        lastMaximumBytes = maximumBytes;
        return selection;
    }

    int calls = 0;
    qint64 lastMaximumBytes = 0;
    std::optional<SelectedFile> selection;
};

class CapabilityBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void deniedCapabilityNeverTouchesService();
    void deniedOperationNeverTouchesService();
    void dispatchesAllowedCapabilityWithHostIdentity();
    void clipboardReadRequiresHostGesture();
    void fileCancellationAndSizeAreStable();
};

void CapabilityBrokerTest::deniedCapabilityNeverTouchesService()
{
    RecordingService network;
    CapabilityBroker broker(EffectivePolicy{}, CapabilityServices{&network, nullptr, nullptr, nullptr});

    const BrokerResult result = broker.dispatch(QStringLiteral("network"),
                                                QStringLiteral("request"),
                                                {},
                                                {QStringLiteral("host.identity"), true});

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
                                                {QStringLiteral("host.identity"), true});

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
                                                {QStringLiteral("host.identity"), false});

    QVERIFY(result.ok);
    QCOMPARE(storage.calls, 1);
    QCOMPARE(storage.lastOperation, QStringLiteral("get"));
    QCOMPARE(storage.lastPayload, payload);
    QCOMPARE(storage.lastContext.appIdentity, QStringLiteral("host.identity"));
}

void CapabilityBrokerTest::clipboardReadRequiresHostGesture()
{
    RecordingClipboard backend;
    ClipboardBroker service(EffectiveClipboardPolicy{true, true}, backend);

    BrokerResult result = service.invoke(QStringLiteral("read"),
                                         {},
                                         {QStringLiteral("host.identity"), false});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(backend.reads, 0);

    result = service.invoke(QStringLiteral("read"),
                            {},
                            {QStringLiteral("host.identity"), true});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("text")).toString(),
             QStringLiteral("clipboard-value"));
    QCOMPARE(backend.reads, 1);
}

void CapabilityBrokerTest::fileCancellationAndSizeAreStable()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 4}, backend);
    const HostRequestContext context{QStringLiteral("host.identity"), true};

    BrokerResult result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("file.cancelled"));
    QCOMPARE(backend.calls, 1);
    QCOMPARE(backend.lastMaximumBytes, 4);

    backend.selection = SelectedFile{QStringLiteral("report.txt"), QByteArray("12345")};
    result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("file.too_large"));

    backend.selection = SelectedFile{QStringLiteral("report.txt"), QByteArray("1234")};
    result = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("name")).toString(),
             QStringLiteral("report.txt"));
    QCOMPARE(QByteArray::fromBase64(
                 result.value.value(QStringLiteral("contentBase64")).toString().toLatin1()),
             QByteArray("1234"));
}

QTEST_APPLESS_MAIN(CapabilityBrokerTest)
#include "tst_capability_broker.moc"
