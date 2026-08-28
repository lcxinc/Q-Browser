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

#include <chrono>
#include <future>
#include <type_traits>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

static_assert(!std::is_default_constructible_v<UserGestureSession>);
static_assert(!std::is_copy_constructible_v<UserGestureSession>);
static_assert(std::is_nothrow_move_constructible_v<UserGestureSession>);
static_assert(!std::is_default_constructible_v<UserGestureGrant>);
static_assert(!std::is_copy_constructible_v<UserGestureGrant>);
static_assert(std::is_nothrow_move_constructible_v<UserGestureGrant>);
static_assert(!std::is_default_constructible_v<PreparedFileRequest>);
static_assert(!std::is_copy_constructible_v<PreparedFileRequest>);
static_assert(std::is_nothrow_move_constructible_v<PreparedFileRequest>);
static_assert(!std::is_copy_constructible_v<PreparedFileRequestResult>);
static_assert(std::is_nothrow_move_constructible_v<PreparedFileRequestResult>);
static_assert(std::is_copy_constructible_v<FileDialogSelection>);
static_assert(std::is_copy_assignable_v<FileDialogSelection>);

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

class FailingReadDevice final : public QIODevice
{
public:
    FailingReadDevice()
    {
        open(QIODevice::ReadOnly);
    }

    [[nodiscard]] bool isSequential() const override { return true; }

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *, qint64) override { return -1; }
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
        std::unique_ptr<QIODevice> stream;
        if (failRead) {
            stream = std::make_unique<FailingReadDevice>();
        } else {
            auto buffer = std::make_unique<QBuffer>();
            buffer->setData(content);
            buffer->open(QIODevice::ReadOnly);
            stream = std::move(buffer);
        }
        return FileDialogResult::opened(name,
                                        content.size(),
                                        std::move(stream),
                                        identityBeforeRead,
                                        identityAfterRead);
    }

    int calls = 0;
    qint64 lastMaximumBytes = 0;
    FileDialogStatus status = FileDialogStatus::Cancelled;
    QString name = QStringLiteral("report.txt");
    QByteArray content;
    QByteArray identityBeforeRead = QByteArray("recording-file:1");
    QByteArray identityAfterRead = identityBeforeRead;
    bool failRead = false;
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
    void gestureGrantIsOpaqueBoundExpiringAndSingleUse();
    void gestureStoreBoundsRecordsAndPurgesClosedSessions();
    void gestureSessionRaiiInvalidatesOnDestruction();
    void revokingOneGestureSessionLeavesSiblingActive();
    void clipboardEnforcesNativeByteLimitAndStableStatuses();
    void enforcesExactIpcRequestAndResponseBudget();
    void filePreparationIsMoveOnlyBoundedAndPure();
    void filePreparationRejectsInvalidRequestsWithoutUi();
    void fileCompletionValidatesValueBoundary();
    void fileCompletionMapsStableTerminalStatuses();
    void fileLegacyReadDistinguishesEmptyFromFailure();
    void fileCancellationAndSizeAreStable();
    void fileInvalidRequestsNeverOpenDialog();
    void fileCompletionRejectsUnsafeLeafNames();
    void fileBackendStatusAndConcurrencyAreFailClosed();
    void nativeFileDialogUsesStableShellStream();
    void nativeFileDialogReturnsBoundStreamAndChecksSizeBeforeRead();
    void nativeFileDialogRejectsReparseSelection();
    void nativeFileDialogRejectsReplacementAfterStreamBind();
    void nativeFileDialogRejectedSelectionStaysOpenUntilCancel();
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
    auto session = grants.openSession(QStringLiteral("host.identity"));
    QVERIFY(session.has_value());

    BrokerResult result = service.invoke(
        QStringLiteral("read"),
        {},
        {QStringLiteral("host.identity"), QStringLiteral("read-1")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(backend.reads, 0);

    auto grant = grants.issue(*session, QStringLiteral("read-1"), 1000);
    QVERIFY(grant.has_value());
    result = service.invoke(QStringLiteral("read"),
                            {},
                            {QStringLiteral("host.identity"),
                             QStringLiteral("read-1"),
                             &*grant});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("text")).toString(),
             QStringLiteral("clipboard-value"));
    QCOMPARE(backend.reads, 1);
}

void CapabilityBrokerTest::gestureGrantIsOpaqueBoundExpiringAndSingleUse()
{
    RecordingClipboard backend;
    UserGestureGrantStore grants;
    ClipboardBroker service(EffectiveClipboardPolicy{false, true}, backend, grants);
    auto session = grants.openSession(QStringLiteral("app.one"));
    QVERIFY(session.has_value());
    const auto read = [&](const QString &app,
                          const QString &requestId,
                          UserGestureGrant *grant) {
        return service.invoke(QStringLiteral("read"),
                              {},
                              {app, requestId, grant});
    };

    auto grant = grants.issue(*session, QStringLiteral("request.one"), 1000);
    QVERIFY(grant.has_value());
    QCOMPARE(read(QStringLiteral("app.two"), QStringLiteral("request.one"), &*grant).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("request.two"), &*grant).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QVERIFY(read(QStringLiteral("app.one"), QStringLiteral("request.one"), &*grant).ok);
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("request.one"), &*grant).errorCode,
             QStringLiteral("clipboard.gesture_required"));

    auto stale = grants.issue(*session, QStringLiteral("stale"), 1);
    QVERIFY(stale.has_value());
    QTest::qWait(5);
    QCOMPARE(read(QStringLiteral("app.one"), QStringLiteral("stale"), &*stale).errorCode,
             QStringLiteral("clipboard.gesture_required"));
    QCOMPARE(backend.reads, 1);
}

void CapabilityBrokerTest::gestureStoreBoundsRecordsAndPurgesClosedSessions()
{
    UserGestureGrantStore grants;
    auto session = grants.openSession(QStringLiteral("app.one"));
    QVERIFY(session.has_value());

    for (int index = 0; index < 8192; ++index) {
        const QString requestId = QStringLiteral("request-%1").arg(index);
        auto grant = grants.issue(*session, requestId, 60000);
        QVERIFY2(grant.has_value(),
                 qPrintable(QStringLiteral("issue failed at %1").arg(index)));
        QVERIFY2(grants.consume(*grant, QStringLiteral("app.one"), requestId),
                 qPrintable(QStringLiteral("consume failed at %1").arg(index)));
    }

    QVERIFY(!grants.issue(*session, QStringLiteral("beyond-bound"), 60000).has_value());
    session.reset();
    auto replacement = grants.openSession(QStringLiteral("app.one"));
    QVERIFY(replacement.has_value());
    QVERIFY(grants.issue(*replacement, QStringLiteral("after-close"), 60000).has_value());
}

void CapabilityBrokerTest::gestureSessionRaiiInvalidatesOnDestruction()
{
    UserGestureGrantStore grants;
    std::optional<UserGestureGrant> grant;
    {
        auto session = grants.openSession(QStringLiteral("app.one"));
        QVERIFY(session.has_value());
        grant = grants.issue(*session, QStringLiteral("request.one"), 60000);
        QVERIFY(grant.has_value());
    }
    QVERIFY(!grants.consume(*grant,
                            QStringLiteral("app.one"),
                            QStringLiteral("request.one")));

    std::vector<UserGestureSession> sessions;
    for (int index = 0; index < 128; ++index) {
        auto session = grants.openSession(QStringLiteral("app-%1").arg(index));
        QVERIFY(session.has_value());
        sessions.push_back(std::move(*session));
    }
    QVERIFY(!grants.openSession(QStringLiteral("overflow")).has_value());
    sessions.pop_back();
    QVERIFY(grants.openSession(QStringLiteral("replacement")).has_value());
}

void CapabilityBrokerTest::revokingOneGestureSessionLeavesSiblingActive()
{
    UserGestureGrantStore grants;
    auto first = grants.openSession(QStringLiteral("app.same"));
    auto sibling = grants.openSession(QStringLiteral("app.same"));
    QVERIFY(first.has_value());
    QVERIFY(sibling.has_value());
    auto firstGrant = grants.issue(*first, QStringLiteral("same-request"), 1000);
    auto siblingGrant = grants.issue(*sibling, QStringLiteral("same-request"), 1000);
    QVERIFY(firstGrant.has_value());
    QVERIFY(siblingGrant.has_value());

    QVERIFY(grants.revokeOutstanding(*first));

    QVERIFY(!grants.consume(*firstGrant,
                            QStringLiteral("app.same"),
                            QStringLiteral("same-request")));
    QVERIFY(grants.consume(*siblingGrant,
                           QStringLiteral("app.same"),
                           QStringLiteral("same-request")));
}

void CapabilityBrokerTest::clipboardEnforcesNativeByteLimitAndStableStatuses()
{
    RecordingClipboard backend;
    UserGestureGrantStore grants;
    ClipboardBroker service(EffectiveClipboardPolicy{true, true}, backend, grants);
    auto session = grants.openSession(QStringLiteral("app.one"));
    QVERIFY(session.has_value());
    constexpr qint64 contentCharacters = maximumClipboardBytes() / 2 - 1;

    backend.readResult.text = QString(contentCharacters, u'x');
    auto exactGrant = grants.issue(*session, QStringLiteral("read-exact"), 1000);
    QVERIFY(exactGrant.has_value());
    QVERIFY(service.invoke(QStringLiteral("read"), {},
                           {QStringLiteral("app.one"),
                            QStringLiteral("read-exact"),
                            &*exactGrant})
                .ok);
    QCOMPARE(backend.lastMaximumBytes, maximumClipboardBytes());

    backend.readResult.text.append(u'x');
    auto largeGrant = grants.issue(*session, QStringLiteral("read-large"), 1000);
    QVERIFY(largeGrant.has_value());
    QCOMPARE(service.invoke(QStringLiteral("read"), {},
                            {QStringLiteral("app.one"),
                             QStringLiteral("read-large"),
                             &*largeGrant})
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
    auto failedGrant = grants.issue(*session, QStringLiteral("read-failed"), 1000);
    QVERIFY(failedGrant.has_value());
    QCOMPARE(service.invoke(QStringLiteral("read"), {},
                            {QStringLiteral("app.one"),
                             QStringLiteral("read-failed"),
                             &*failedGrant})
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

void CapabilityBrokerTest::filePreparationIsMoveOnlyBoundedAndPure()
{
    RecordingFileDialog backend;
    FileBroker service(
        EffectiveFilePolicy{true, maximumIpcBinaryResultBytes() + 4096},
        backend);
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("file-prepare")};

    PreparedFileRequestResult preparation = service.prepareFileRequest(
        QStringLiteral("open"), {}, context);

    QVERIFY(preparation.request.has_value());
    QVERIFY(preparation.rejection.errorCode.isEmpty());
    QCOMPARE(preparation.request->operation(), QStringLiteral("open"));
    QCOMPARE(preparation.request->payload(), QJsonObject{});
    QCOMPARE(preparation.request->appIdentity(), context.appIdentity);
    QCOMPARE(preparation.request->requestId(), context.requestId);
    QCOMPARE(preparation.request->maximumBytes(),
             maximumIpcBinaryResultBytes());
    QCOMPARE(backend.calls, 0);
}

void CapabilityBrokerTest::filePreparationRejectsInvalidRequestsWithoutUi()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);

    PreparedFileRequestResult preparation = service.prepareFileRequest(
        QStringLiteral("open"), {},
        {QString{}, QStringLiteral("file-invalid-identity")});
    QVERIFY(!preparation.request.has_value());
    QCOMPARE(preparation.rejection.errorCode,
             QStringLiteral("capability.denied"));

    preparation = service.prepareFileRequest(
        QStringLiteral("save"), {},
        {QStringLiteral("host.identity"), QStringLiteral("file-save")});
    QVERIFY(!preparation.request.has_value());
    QCOMPARE(preparation.rejection.errorCode,
             QStringLiteral("file.invalid_request"));

    preparation = service.prepareFileRequest(
        QStringLiteral("open"),
        {{QStringLiteral("path"), QStringLiteral("C:/untrusted.txt")}},
        {QStringLiteral("host.identity"), QStringLiteral("file-path")});
    QVERIFY(!preparation.request.has_value());
    QCOMPARE(preparation.rejection.errorCode,
             QStringLiteral("file.invalid_request"));

    preparation = service.prepareFileRequest(
        QStringLiteral("open"), {},
        {QStringLiteral("host.identity"),
         QString(static_cast<qsizetype>(FrameCodec::maximumPayloadBytes()),
                 u'r')});
    QVERIFY(!preparation.request.has_value());
    QCOMPARE(preparation.rejection.errorCode,
             QStringLiteral("capability.payload_too_large"));

    FileBroker denied(EffectiveFilePolicy{false, 16}, backend);
    preparation = denied.prepareFileRequest(
        QStringLiteral("open"), {},
        {QStringLiteral("host.identity"), QStringLiteral("file-denied")});
    QVERIFY(!preparation.request.has_value());
    QCOMPARE(preparation.rejection.errorCode,
             QStringLiteral("capability.denied"));
    QCOMPARE(backend.calls, 0);
}

void CapabilityBrokerTest::fileCompletionValidatesValueBoundary()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 4}, backend);
    PreparedFileRequestResult preparation = service.prepareFileRequest(
        QStringLiteral("open"), {},
        {QStringLiteral("host.identity"), QStringLiteral("file-complete")});
    QVERIFY(preparation.request.has_value());

    FileDialogSelection selection;
    selection.status = FileDialogStatus::Opened;
    selection.name = QStringLiteral("report.txt");
    selection.declaredSize = 4;
    selection.contentBase64 = QByteArray("safe").toBase64();
    selection.approvedMaximumBytes = 4;
    selection.identityBeforeRead = QByteArray("stable-file:1");
    selection.identityAfterRead = selection.identityBeforeRead;

    BrokerResult result = service.completeFileRequest(*preparation.request,
                                                       selection);
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("name")).toString(),
             selection.name);
    QCOMPARE(result.value.value(QStringLiteral("size")).toInteger(),
             selection.declaredSize);
    QCOMPARE(result.value.value(QStringLiteral("contentBase64")).toString(),
             QString::fromLatin1(selection.contentBase64));

    FileDialogSelection malformed = selection;
    malformed.identityAfterRead = QByteArray("stable-file:2");
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.failed"));

    malformed = selection;
    malformed.contentBase64 = QByteArray("***");
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.failed"));

    malformed = selection;
    malformed.contentBase64.chop(1);
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.failed"));

    malformed = selection;
    malformed.declaredSize = 3;
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.failed"));

    malformed = selection;
    malformed.approvedMaximumBytes = 5;
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.failed"));

    malformed = selection;
    malformed.declaredSize = 5;
    malformed.contentBase64 = QByteArray("large").toBase64();
    result = service.completeFileRequest(*preparation.request, malformed);
    QCOMPARE(result.errorCode, QStringLiteral("file.too_large"));
    QCOMPARE(backend.calls, 0);
}

void CapabilityBrokerTest::fileCompletionMapsStableTerminalStatuses()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);
    PreparedFileRequestResult preparation = service.prepareFileRequest(
        QStringLiteral("open"), {},
        {QStringLiteral("host.identity"), QStringLiteral("file-terminal")});
    QVERIFY(preparation.request.has_value());

    FileDialogSelection selection;
    selection.approvedMaximumBytes = 16;

    selection.status = FileDialogStatus::Cancelled;
    QCOMPARE(service.completeFileRequest(*preparation.request, selection).errorCode,
             QStringLiteral("file.cancelled"));

    selection.status = FileDialogStatus::TooLarge;
    QCOMPARE(service.completeFileRequest(*preparation.request, selection).errorCode,
             QStringLiteral("file.too_large"));

    selection.status = FileDialogStatus::Busy;
    QCOMPARE(service.completeFileRequest(*preparation.request, selection).errorCode,
             QStringLiteral("file.busy"));

    selection.status = FileDialogStatus::Failed;
    QCOMPARE(service.completeFileRequest(*preparation.request, selection).errorCode,
             QStringLiteral("file.failed"));

    selection.status = static_cast<FileDialogStatus>(-1);
    selection.name = QStringLiteral("empty.txt");
    selection.declaredSize = 0;
    selection.contentBase64.clear();
    selection.identityBeforeRead = QByteArray("stable-file:1");
    selection.identityAfterRead = selection.identityBeforeRead;
    QCOMPARE(service.completeFileRequest(*preparation.request, selection).errorCode,
             QStringLiteral("file.failed"));
    QCOMPARE(backend.calls, 0);
}

void CapabilityBrokerTest::fileLegacyReadDistinguishesEmptyFromFailure()
{
    RecordingFileDialog backend;
    backend.status = FileDialogStatus::Opened;
    backend.name = QStringLiteral("empty.txt");
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("file-read")};

    const BrokerResult empty = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(empty.ok);
    QCOMPARE(empty.value.value(QStringLiteral("size")).toInteger(), 0);
    QCOMPARE(empty.value.value(QStringLiteral("contentBase64")).toString(),
             QString());

    backend.failRead = true;
    const BrokerResult failed = service.invoke(QStringLiteral("open"), {}, context);
    QVERIFY(!failed.ok);
    QCOMPARE(failed.errorCode, QStringLiteral("file.failed"));
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

void CapabilityBrokerTest::fileInvalidRequestsNeverOpenDialog()
{
    RecordingFileDialog backend;
    backend.status = FileDialogStatus::Opened;
    backend.content = QByteArray("stable");
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("file-invalid")};
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);

    QCOMPARE(service.invoke(QStringLiteral("save"), {}, context).errorCode,
             QStringLiteral("file.invalid_request"));
    QCOMPARE(service.invoke(QStringLiteral("open"),
                            {{QStringLiteral("path"),
                              QStringLiteral("C:/untrusted.txt")}},
                            context)
                 .errorCode,
             QStringLiteral("file.invalid_request"));
    QCOMPARE(backend.calls, 0);

    FileBroker denied(EffectiveFilePolicy{false, 16}, backend);
    QCOMPARE(denied.invoke(QStringLiteral("open"), {}, context).errorCode,
             QStringLiteral("capability.denied"));
    QCOMPARE(backend.calls, 0);
}

void CapabilityBrokerTest::fileCompletionRejectsUnsafeLeafNames()
{
    RecordingFileDialog backend;
    FileBroker service(EffectiveFilePolicy{true, 16}, backend);
    const HostRequestContext context{QStringLiteral("host.identity"),
                                     QStringLiteral("file-name")};
    PreparedFileRequestResult preparation = service.prepareFileRequest(
        QStringLiteral("open"), {}, context);
    QVERIFY(preparation.request.has_value());

    FileDialogSelection selection;
    selection.status = FileDialogStatus::Opened;
    selection.declaredSize = 6;
    selection.contentBase64 = QByteArray("stable").toBase64();
    selection.approvedMaximumBytes = 16;
    selection.identityBeforeRead = QByteArray("stable-file:1");
    selection.identityAfterRead = selection.identityBeforeRead;
    const QStringList unsafeNames{
        QStringLiteral("../secret.txt"),
        QStringLiteral("folder/report.txt"),
        QStringLiteral("folder\\report.txt"),
        QStringLiteral("C:secret.txt"),
        QStringLiteral("report.txt."),
        QStringLiteral("report.txt "),
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral("control") + QChar(1) + QStringLiteral(".txt"),
    };

    for (const QString &name : unsafeNames) {
        selection.name = name;
        const BrokerResult result = service.completeFileRequest(
            *preparation.request, selection);
        QVERIFY2(!result.ok,
                 qPrintable(QStringLiteral("unsafe file leaf accepted: %1")
                                .arg(name)));
        QCOMPARE(result.errorCode, QStringLiteral("file.failed"));
    }
    QCOMPARE(backend.calls, 0);
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
    auto unblock = qScopeGuard([&] { backend.proceed.release(); });
    QVERIFY(backend.entered.tryAcquire(1, 1000));
    const BrokerResult concurrent = service.invoke(QStringLiteral("open"), {}, context);
    backend.proceed.release();
    unblock.dismiss();
    QCOMPARE(first.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    const BrokerResult owner = first.get();

    QCOMPARE(concurrent.errorCode, QStringLiteral("file.busy"));
    QCOMPARE(backend.calls, 2); // failed status call plus one active dialog
    QVERIFY(owner.ok);
}

void CapabilityBrokerTest::nativeFileDialogUsesStableShellStream()
{
    QFile source(QStringLiteral(Q_BROWSER_FILE_BROKER_SOURCE_FILE));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();
#ifdef Q_OS_WIN
    QByteArray normalizedImplementation = implementation;
    normalizedImplementation.replace("\r\n", "\n");
    QVERIFY(implementation.contains("IFileOpenDialog"));
    QVERIFY(implementation.contains("IFileDialogEvents"));
    QVERIFY(implementation.contains("OnFileOk"));
    QVERIFY(normalizedImplementation.contains(
        "if (dialog == nullptr || FAILED(dialog->GetResult(item.put()))) {\n"
        "            return S_FALSE;"));
    QVERIFY(implementation.contains("CreateFileW"));
    QVERIFY(implementation.contains("FILE_ID_INFO"));
    QVERIFY(implementation.contains("FileIdInfo"));
    QVERIFY(implementation.contains("FILE_ID_128"));
    QVERIFY(!implementation.contains("BY_HANDLE_FILE_INFORMATION"));
    QVERIFY(!implementation.contains("BindToHandler"));
    const qsizetype windowsBranch = implementation.indexOf("IFileOpenDialog");
    const qsizetype fallback = implementation.indexOf("QFileDialog::getOpenFileName");
    QVERIFY(windowsBranch >= 0);
    QVERIFY(fallback > windowsBranch);
    QVERIFY(implementation.indexOf("OnFileOk") < fallback);
    QVERIFY(implementation.contains("_open_osfhandle"));
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
    QVERIFY(result.identityBeforeRead.startsWith("win-id128:"));
    QCOMPARE(result.stream->readAll(), QByteArray("safe"));

    result = backend.openFile(3);
    QCOMPARE(result.status, FileDialogStatus::TooLarge);
    QVERIFY(result.stream == nullptr);

    FileBroker service(EffectiveFilePolicy{true, 4}, backend);
    const BrokerResult invoked = service.invoke(
        QStringLiteral("open"), {},
        {QStringLiteral("host.identity"), QStringLiteral("native-file")});
    QVERIFY(invoked.ok);
    QCOMPARE(QByteArray::fromBase64(
                 invoked.value.value(QStringLiteral("contentBase64"))
                     .toString().toLatin1()),
             QByteArray("safe"));
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
         .afterNativeHandleOpened = [&](const QString &path) {
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
    if (replaced) {
        QCOMPARE(result.status, FileDialogStatus::Failed);
    } else {
        QCOMPARE(result.status, FileDialogStatus::Opened);
        QVERIFY(result.stream != nullptr);
        QCOMPARE(result.stream->readAll(), QByteArray("safe"));
    }
#endif
}

void CapabilityBrokerTest::nativeFileDialogRejectedSelectionStaysOpenUntilCancel()
{
#ifndef Q_OS_WIN
    QSKIP("Windows native file dialog coverage");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(QStringLiteral("large.txt"));
    QFile file(selected);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("oversized"), 9);
    file.close();

    qbrowser_broker_testing::setFileDialogTestHooks(
        {.selectedPath = [selected] { return selected; },
         .afterNativeHandleOpened = {},
         .cancelAfterRejectedSelection = true});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend backend;

    QCOMPARE(backend.openFile(4).status, FileDialogStatus::Cancelled);
#endif
}

QTEST_GUILESS_MAIN(CapabilityBrokerTest)
#include "tst_capability_broker.moc"
