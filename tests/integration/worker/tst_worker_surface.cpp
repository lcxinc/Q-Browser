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
    QCOMPARE(launch->hostSession.receive(15000).status, SessionStatus::MessageReady);
    const auto surfaceMessage = launch->hostSession.receive(15000);
    QCOMPARE(surfaceMessage.status, SessionStatus::MessageReady);
    QCOMPARE(surfaceMessage.message->type(), ProtocolType::SurfaceReady);
    const auto ready = launch->hostSession.receive(15000);
    QCOMPARE(ready.status, SessionStatus::MessageReady);
    QCOMPARE(ready.message->type(), ProtocolType::Ready);

    QWidget host;
    host.resize(640, 480);
    host.show();
    WorkerSurface *surface = WorkerSurface::create(
        surfaceMessage.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.processId(), &host);
    QVERIFY(surface != nullptr);
    surface->setGeometry(host.rect());
    surface->show();
    QTest::qWait(100);
    QVERIFY(surface->isValid());
    QVERIFY(WorkerSurface::create(QStringLiteral("1"), launch->process.processId(), &host)
            == nullptr);

    host.resize(800, 600);
    surface->setGeometry(host.rect());
    QTest::qWait(100);
    RECT nativeRect{};
    QVERIFY(GetClientRect(reinterpret_cast<HWND>(surface->nativeWindowId()), &nativeRect));
    QCOMPARE(nativeRect.right - nativeRect.left, 800L);
    QCOMPARE(nativeRect.bottom - nativeRect.top, 600L);
    surface->setFocus(Qt::OtherFocusReason);
    QVERIFY(surface->hasFocus() || surface->containerHasFocus());

    delete surface;
    QVERIFY(!launch->process.waitForFinished(0));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("test.done")),
                                    5000));
    QCOMPARE(launch->hostSession.receive(5000).message->type(), ProtocolType::Shutdown);
    QVERIFY(launch->process.waitForFinished(5000));
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
