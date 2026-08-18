#include "WorkerSurface.h"
#include "WorkerTestEnvironment.h"

#include <QApplication>
#include <QTest>

#include <qt_windows.h>

class WorkerSurfaceTest final : public QObject
{
    Q_OBJECT
private slots:
    void embedsOnlyTheLaunchedWorkersWindow();
};

void WorkerSurfaceTest::embedsOnlyTheLaunchedWorkersWindow()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("surface-nonce"),
                                     QStringLiteral("surface-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceMessage = receiveUntil(launch->hostSession,
                                             ProtocolType::SurfaceReady);
    QCOMPARE(surfaceMessage.status, SessionStatus::MessageReady);
    QCOMPARE(surfaceMessage.message->type(), ProtocolType::SurfaceReady);
    const auto ready = receiveUntil(launch->hostSession, ProtocolType::Ready);
    QCOMPARE(ready.status, SessionStatus::MessageReady);
    QCOMPARE(ready.message->type(), ProtocolType::Ready);

    QWidget host;
    host.resize(640, 480);
    host.show();
    WorkerSurface *surface = WorkerSurface::create(
        surfaceMessage.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{42}, &host);
    QVERIFY(surface != nullptr);
    surface->setGeometry(host.rect());
    surface->show();
    QTest::qWait(100);
    QVERIFY(surface->isValid());
    QVERIFY(WorkerSurface::create(QStringLiteral("1"),
                                  launch->process.nativeProcessHandle(),
                                  WorkerAttemptId{42}, &host)
            == nullptr);
    QCOMPARE(surface->attemptId(), WorkerAttemptId{42});

    host.resize(800, 600);
    surface->setGeometry(host.rect());
    QTest::qWait(100);
    RECT nativeRect{};
    QVERIFY(GetClientRect(reinterpret_cast<HWND>(surface->nativeWindowId()), &nativeRect));
    QCOMPARE(nativeRect.right - nativeRect.left, 800L);
    QCOMPARE(nativeRect.bottom - nativeRect.top, 600L);
    surface->setFocus(Qt::OtherFocusReason);
    const HWND workerWindow = reinterpret_cast<HWND>(surface->nativeWindowId());
    DWORD workerPid = 0;
    const DWORD workerThread = GetWindowThreadProcessId(workerWindow, &workerPid);
    QCOMPARE(workerPid, launch->process.processId());
    QVERIFY(workerThread != 0);
    GUITHREADINFO guiInfo{sizeof(GUITHREADINFO)};
    QTRY_VERIFY_WITH_TIMEOUT(GetGUIThreadInfo(workerThread, &guiInfo)
                                 && guiInfo.hwndFocus == workerWindow,
                             2000);

    delete surface;
    QVERIFY(!launch->process.waitForFinished(0));
    WorkerSurface *lifetimeBound = WorkerSurface::create(
        surfaceMessage.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{43}, &host);
    QVERIFY(lifetimeBound != nullptr);
    launch->hostSession.close();
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    QVERIFY(!lifetimeBound->isValid());
    lifetimeBound->resize(320, 240);
    lifetimeBound->setFocus(Qt::OtherFocusReason);
    QVERIFY(!lifetimeBound->isValid());
    delete lifetimeBound;
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    WorkerSurfaceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_worker_surface.moc"
