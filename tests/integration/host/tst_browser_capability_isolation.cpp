#include "FileBroker.h"
#include "TabCapabilityAuthority.h"

#include <QBuffer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTest>

#include <atomic>
#include <chrono>
#include <future>
#include <utility>

namespace {
TabCapabilityAuthority authority(const QString &tabId,
                                 const quint64 runtimeIncarnation,
                                 const quint32 workerProcessId,
                                 const quintptr workerWindowId,
                                 const quint64 sessionGeneration,
                                 const quint64 leaseAuthorityEpoch)
{
    return {tabId, runtimeIncarnation, QStringLiteral("com.qbrowser.same"),
            workerProcessId, workerWindowId, sessionGeneration,
            leaseAuthorityEpoch};
}

class BlockingFileDialog final : public FileDialogBackend
{
public:
    FileDialogResult openFile(const qint64 maximumBytes) override
    {
        lastMaximumBytes = maximumBytes;
        ++calls;
        if (block) {
            entered.release();
            proceed.acquire();
        }
        auto stream = std::make_unique<QBuffer>();
        stream->setData(content);
        stream->open(QIODevice::ReadOnly);
        return FileDialogResult::opened(
            QStringLiteral("report.txt"), content.size(), std::move(stream));
    }

    std::atomic<int> calls{0};
    qint64 lastMaximumBytes = 0;
    QByteArray content = QByteArray("stable");
    bool block = false;
    QSemaphore entered;
    QSemaphore proceed;
};

}

class BrowserCapabilityIsolationTest final : public QObject
{
    Q_OBJECT

private slots:
    void processWideDialogBusyRejectsSiblingBeforeBackendCall();
    void filePrepareAndCompleteValidationIsDeferred();
    void ownerBoundTerminalCompletionIsDeferred_data();
    void ownerBoundTerminalCompletionIsDeferred();
    void pendingDialogSiblingProgressIsDeferred();
    void fullRequestAndSessionIdentityRoutingIsDeferred();
};

void BrowserCapabilityIsolationTest::
    processWideDialogBusyRejectsSiblingBeforeBackendCall()
{
    BlockingFileDialog ownerBackend;
    ownerBackend.block = true;
    BlockingFileDialog siblingBackend;
    FileBroker ownerBroker(EffectiveFilePolicy{true, 16}, ownerBackend);
    FileBroker siblingBroker(EffectiveFilePolicy{true, 16}, siblingBackend);
    const TabCapabilityAuthority owner = authority(
        QStringLiteral("tab-owner"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority sibling = authority(
        QStringLiteral("tab-sibling"), 2, 42, 402, 9, 12);

    auto pending = std::async(std::launch::async, [&] {
        return ownerBroker.invoke(
            QStringLiteral("open"), {},
            {owner.appIdentity, QStringLiteral("file-owner")});
    });
    auto unblock = qScopeGuard([&] { ownerBackend.proceed.release(); });
    QVERIFY(ownerBackend.entered.tryAcquire(1, 1'000));
    const BrokerResult concurrent = siblingBroker.invoke(
        QStringLiteral("open"), {},
        {sibling.appIdentity, QStringLiteral("file-sibling")});
    ownerBackend.proceed.release();
    unblock.dismiss();
    QCOMPARE(pending.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    const BrokerResult ownerResult = pending.get();

    QVERIFY(ownerResult.ok);
    QVERIFY(!concurrent.ok);
    QCOMPARE(concurrent.errorCode, QStringLiteral("file.busy"));
    QCOMPARE(siblingBackend.calls.load(), 0);
}

void BrowserCapabilityIsolationTest::filePrepareAndCompleteValidationIsDeferred()
{
    QFAIL("TODO(Task16 Task2): production FileBroker has no prepare/complete seam; "
          "replace this RED placeholder with malformed prepare and completion cases");
}

void BrowserCapabilityIsolationTest::
    ownerBoundTerminalCompletionIsDeferred_data()
{
    QTest::addColumn<QString>("terminalStatus");
    QTest::newRow("success") << QStringLiteral("success");
    QTest::newRow("cancelled") << QStringLiteral("file.cancelled");
    QTest::newRow("failed") << QStringLiteral("file.failed");
}

void BrowserCapabilityIsolationTest::
    ownerBoundTerminalCompletionIsDeferred()
{
    QFETCH(QString, terminalStatus);
    Q_UNUSED(terminalStatus)
    QFAIL("TODO(Task16 Task2): production has no asynchronous operation-token "
          "adapter; cover switch, reopen, close/cancel, and late result discard");
}

void BrowserCapabilityIsolationTest::pendingDialogSiblingProgressIsDeferred()
{
    QFAIL("TODO(Task16 Task2): production file.open is synchronous; once the "
          "async coordinator exists, prove sibling heartbeat/network/storage/"
          "clipboard progress while its dialog is pending");
}

void BrowserCapabilityIsolationTest::
    fullRequestAndSessionIdentityRoutingIsDeferred()
{
    QFAIL("TODO(Task16 Task2): final delivery currently lacks a testable full "
          "authority/request/session gate; bind this to the production IO seam");
}

QTEST_APPLESS_MAIN(BrowserCapabilityIsolationTest)

#include "tst_browser_capability_isolation.moc"
