#include "CapabilityBroker.h"
#include "ClipboardBroker.h"
#include "FileBroker.h"
#include "FileDialogTestHooks.h"
#include "FrameCodec.h"
#include "ProtocolMessage.h"
#include "UserGestureGrantStore.h"

#include <QTest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QSemaphore>

#include <future>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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
    ClipboardReadResult readText(const qint64 maximumBytes) override
    {
        ++reads;
        lastMaximumBytes = maximumBytes;
        return readResult;
    }
    ClipboardStatus writeText(const QString &text, const qint64 maximumBytes) override
    {
        ++writes;
        lastText = text;
        lastMaximumBytes = maximumBytes;
        return writeStatus;
    }

    int reads = 0;
    int writes = 0;
    QString lastText;
    qint64 lastMaximumBytes = 0;
    ClipboardReadResult readResult{ClipboardStatus::Success,
                                   QStringLiteral("clipboard-value")};
    ClipboardStatus writeStatus = ClipboardStatus::Success;
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
    void gestureGrantTombstonesDoNotExhaustActiveCapacity();
    void clipboardEnforcesNativeByteLimitAndStableStatuses();
    void enforcesExactIpcRequestAndResponseBudget();
    void fileCancellationAndSizeAreStable();
    void fileBackendStatusAndConcurrencyAreFailClosed();
    void nativeFileDialogUsesStableShellStream();
    void nativeFileDialogReturnsBoundStreamAndChecksSizeBeforeRead();
    void nativeFileDialogRejectsReparseSelection();
    void nativeFileDialogRejectsReplacementAfterStreamBind();
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

void CapabilityBrokerTest::gestureGrantTombstonesDoNotExhaustActiveCapacity()
{
    UserGestureGrantStore grants;

    for (int index = 0; index < 5000; ++index) {
        const QString requestId = QStringLiteral("request-%1").arg(index);
        QVERIFY2(grants.issue(QStringLiteral("app.one"), requestId, 60000),
                 qPrintable(QStringLiteral("issue failed at %1").arg(index)));
        QVERIFY2(grants.consume(QStringLiteral("app.one"), requestId),
                 qPrintable(QStringLiteral("consume failed at %1").arg(index)));
    }

    QVERIFY(!grants.issue(QStringLiteral("app.one"), QStringLiteral("request-4999"), 60000));
    QVERIFY(!grants.consume(QStringLiteral("app.one"), QStringLiteral("request-4999")));
}

void CapabilityBrokerTest::clipboardEnforcesNativeByteLimitAndStableStatuses()
{
    RecordingClipboard backend;
    UserGestureGrantStore grants;
    ClipboardBroker service(EffectiveClipboardPolicy{true, true}, backend, grants);
    constexpr qint64 contentCharacters = maximumClipboardBytes() / 2 - 1;

    backend.readResult.text = QString(contentCharacters, u'x');
    QVERIFY(grants.issue(QStringLiteral("app.one"), QStringLiteral("read-exact"), 1000));
    QVERIFY(service.invoke(QStringLiteral("read"), {},
                           {QStringLiteral("app.one"), QStringLiteral("read-exact")})
                .ok);
    QCOMPARE(backend.lastMaximumBytes, maximumClipboardBytes());

    backend.readResult.text.append(u'x');
    QVERIFY(grants.issue(QStringLiteral("app.one"), QStringLiteral("read-large"), 1000));
    QCOMPARE(service.invoke(QStringLiteral("read"), {},
                            {QStringLiteral("app.one"), QStringLiteral("read-large")})
                 .errorCode,
             QStringLiteral("clipboard.too_large"));

    const QJsonObject exactWrite{{QStringLiteral("text"),
                                  QString(contentCharacters, u'x')}};
    QVERIFY(service.invoke(QStringLiteral("write"), exactWrite,
                           {QStringLiteral("app.one"), QStringLiteral("write-exact")})
                .ok);
    const int writesAtLimit = backend.writes;
    const QJsonObject largeWrite{{QStringLiteral("text"),
                                  QString(contentCharacters + 1, u'x')}};
    QCOMPARE(service.invoke(QStringLiteral("write"), largeWrite,
                            {QStringLiteral("app.one"), QStringLiteral("write-large")})
                 .errorCode,
             QStringLiteral("clipboard.too_large"));
    QCOMPARE(backend.writes, writesAtLimit);

    backend.readResult = ClipboardReadResult::error(ClipboardStatus::Unavailable);
    QVERIFY(grants.issue(QStringLiteral("app.one"), QStringLiteral("read-failed"), 1000));
    QCOMPARE(service.invoke(QStringLiteral("read"), {},
                            {QStringLiteral("app.one"), QStringLiteral("read-failed")})
                 .errorCode,
             QStringLiteral("clipboard.failed"));
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

void CapabilityBrokerTest::nativeFileDialogUsesStableShellStream()
{
    QFile source(QStringLiteral(Q_BROWSER_FILE_BROKER_SOURCE_FILE));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();
#ifdef Q_OS_WIN
    QVERIFY(implementation.contains("IFileOpenDialog"));
    QVERIFY(implementation.contains("BindToHandler"));
    const qsizetype windowsBranch = implementation.indexOf("IFileOpenDialog");
    const qsizetype fallback = implementation.indexOf("QFileDialog::getOpenFileName");
    QVERIFY(windowsBranch >= 0);
    QVERIFY(fallback > windowsBranch);
    QVERIFY(implementation.mid(windowsBranch, fallback - windowsBranch)
                .contains("BindToHandler"));
    QVERIFY(!implementation.contains("_open_osfhandle"));
#endif
}

void CapabilityBrokerTest::nativeFileDialogReturnsBoundStreamAndChecksSizeBeforeRead()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable shell stream coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString selected = QDir(root.path()).filePath(QStringLiteral("selected.txt"));
    QFile selectedFile(selected);
    QVERIFY(selectedFile.open(QIODevice::WriteOnly));
    QCOMPARE(selectedFile.write("safe"), 4);
    selectedFile.close();
    qbrowser_broker_testing::setFileDialogTestHooks(
        {.selectedPath = [selected] { return selected; }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend backend;
    FileDialogResult result = backend.openFile(4);
    QCOMPARE(result.status, FileDialogStatus::Opened);
    QVERIFY(result.stream != nullptr);
    QCOMPARE(result.stream->readAll(), QByteArray("safe"));

    result = backend.openFile(3);
    QCOMPARE(result.status, FileDialogStatus::TooLarge);
    QVERIFY(result.stream == nullptr);
#endif
}

void CapabilityBrokerTest::nativeFileDialogRejectsReparseSelection()
{
#ifndef Q_OS_WIN
    QSKIP("Windows shell stream coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = QDir(root.path()).filePath(QStringLiteral("target.txt"));
    QFile targetFile(target);
    QVERIFY(targetFile.open(QIODevice::WriteOnly));
    QCOMPARE(targetFile.write("safe"), 4);
    targetFile.close();
    const QString link = QDir(root.path()).filePath(QStringLiteral("selected.txt"));
    if (CreateSymbolicLinkW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        == FALSE) {
        QSKIP("File symlink creation is unavailable");
    }
    qbrowser_broker_testing::setFileDialogTestHooks(
        {.selectedPath = [link] { return link; }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend backend;
    QCOMPARE(backend.openFile(1024).status, FileDialogStatus::Failed);
#endif
}

void CapabilityBrokerTest::nativeFileDialogRejectsReplacementAfterStreamBind()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable shell stream coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString selected = QDir(root.path()).filePath(QStringLiteral("selected.txt"));
    const QString replacement = QDir(root.path()).filePath(QStringLiteral("replacement.txt"));
    const QString backup = QDir(root.path()).filePath(QStringLiteral("backup.txt"));
    QFile selectedFile(selected);
    QVERIFY(selectedFile.open(QIODevice::WriteOnly));
    QCOMPARE(selectedFile.write("safe"), 4);
    selectedFile.close();
    QFile replacementFile(replacement);
    QVERIFY(replacementFile.open(QIODevice::WriteOnly));
    QCOMPARE(replacementFile.write("evil"), 4);
    replacementFile.close();

    bool replaced = false;
    qbrowser_broker_testing::setFileDialogTestHooks(
        {.selectedPath = [selected] { return selected; },
         .afterStreamBound = [&](const QString &path) {
             replaced = ReplaceFileW(
                            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                            reinterpret_cast<LPCWSTR>(
                                QDir::toNativeSeparators(replacement).utf16()),
                            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(backup).utf16()),
                            REPLACEFILE_IGNORE_MERGE_ERRORS,
                            nullptr,
                            nullptr)
                 != FALSE;
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend backend;
    const FileDialogResult result = backend.openFile(1024);
    QVERIFY(replaced);
    QCOMPARE(result.status, FileDialogStatus::Failed);
#endif
}

QTEST_GUILESS_MAIN(CapabilityBrokerTest)
#include "tst_capability_broker.moc"
