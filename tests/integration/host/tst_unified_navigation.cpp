#include "HostApplication.h"
#include "MainWindow.h"
#include "NavigationBar.h"
#include "HostWorkerSessionController.h"
#include "HostWorkerSessionTestHooks.h"
#include "ProtocolMessage.h"
#include "RouteRegistry.h"
#include "WebSurface.h"
#include "WorkerSurface.h"
#include "WorkerTestEnvironment.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLineEdit>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWebEnginePage>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <semaphore>
#include <thread>

namespace {

#ifdef Q_OS_WIN
DWORD processHandleCount()
{
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
}
#endif

class HelpServer final : public QObject
{
public:
    explicit HelpServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("responded").toBool()) {
                        return;
                    }
                    QByteArray request = socket->property("requestBuffer").toByteArray();
                    request += socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("requestBuffer", request);
                        return;
                    }
                    socket->setProperty("responded", true);
                    const qsizetype lineEnd = request.indexOf("\r\n");
                    if (lineEnd < 0) {
                        return;
                    }
                    const auto parts = request.first(lineEnd).split(' ');
                    if (parts.size() >= 2) {
                        requests_.append(parts.at(1));
                    }
                    const QByteArray body = QByteArrayLiteral(
                        "<!doctype html><title>Pilot help</title><h1>Help</h1>");
                    const QByteArray response = QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ")
                        + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server_.serverPort()));
    }
    QUrl helpUrl() const { return origin().resolved(QUrl(QStringLiteral("help"))); }
    QList<QByteArray> requests() const { return requests_; }

private:
    QTcpServer server_;
    QList<QByteArray> requests_;
};

RouteRegistry routes(const QUrl &helpUrl,
                     const QString &workerPackageId = QStringLiteral("com.qbrowser.pilot"))
{
    RouteRegistry registry;
    const RouteRecord workerRoute{QStringLiteral("/web-shaped-worker/:id"),
                                  Engine::QmlWorker,
                                  workerPackageId,
                                  QStringLiteral("qml/Main.qml")};
    const RouteRecord webRoute{QStringLiteral("/worker-shaped-web"),
                               Engine::WebEngine,
                               QStringLiteral("com.qbrowser.web"),
                               helpUrl.toString(QUrl::FullyEncoded)};
    const RouteRecord workerListRoute{QStringLiteral("/orders"),
                                      Engine::QmlWorker,
                                      workerPackageId,
                                      QStringLiteral("qml/Main.qml")};
    if (registry.add(workerRoute) != RouteAddResult::Added
        || registry.add(webRoute) != RouteAddResult::Added
        || registry.add(workerListRoute) != RouteAddResult::Added) {
        return {};
    }
    return registry;
}

bool waitForWebNavigation(WebSurface *surface,
                          const QUrl &expected,
                          QSignalSpy &spy)
{
    if (spy.count() > 0 && surface->currentUrl() == expected
        && !surface->page()->isLoading()) {
        return true;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000) {
        if (!spy.wait(10000 - static_cast<int>(elapsed.elapsed()))) {
            return false;
        }
        if (surface->currentUrl() == expected && !surface->page()->isLoading()) {
            return true;
        }
    }
    return false;
}

struct AuthenticatedSessions final {
    std::unique_ptr<IpcSession> host;
    std::unique_ptr<IpcSession> worker;
};

std::optional<AuthenticatedSessions> authenticatedSessions(const QString &appIdentity)
{
    WinPipePair pair = WinPipeTransport::createHostPair();
    if (!pair.isValid()) return std::nullopt;
    auto host = std::make_unique<IpcSession>(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("host-controller-nonce"), appIdentity});
    auto worker = std::make_unique<IpcSession>(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()), IpcRole::Worker);
    const auto handshake = ProtocolMessage::handshake(
        QStringLiteral("host-controller-nonce"));
    if (!handshake.has_value() || !worker->send(*handshake, 1000)
        || host->receive(1000).status != SessionStatus::MessageReady
        || worker->receive(1000).status != SessionStatus::MessageReady) {
        return std::nullopt;
    }
    return AuthenticatedSessions{std::move(host), std::move(worker)};
}

} // namespace

class UnifiedNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void routeRegistryAloneSelectsOneActiveSurfaceAndStableHistory();
    void workerNavigationIsSameAppAndHistoryAware();
    void hostApplicationOwnsAttachableWorkerSessionController();
    void hostApplicationBindsWorkerContextLifecycle();
    void hostWorkerRoutesAreTrackedWithoutDuplicateWorkerNavigation();
    void stalledWorkerReaderNeverBlocksTheGuiThread();
    void gracefulShutdownCleansIoBeforeReattach();
    void failedSessionCanReattachBeforeOldCallbacksDrain();
    void reattachAfterIoThreadFinishedBeforeGuiCleanup();
    void destroyAfterIoThreadFinishedBeforeGuiCleanup();
    void navigationTransactionsRejectReentrantCommands();
};

void UnifiedNavigationTest::routeRegistryAloneSelectsOneActiveSurfaceAndStableHistory()
{
    HelpServer server;
    QVERIFY(server.listen());

    WorkerTestEnvironment workerEnvironment;
    QVERIFY2(workerEnvironment.isValid(), qPrintable(workerEnvironment.error()));
    auto launch = workerEnvironment.launch(QStringLiteral("host-nav-nonce"),
                                           QStringLiteral("host-nav-nonce"), 100);
    QVERIFY2(launch.has_value(), qPrintable(workerEnvironment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);

    WorkerSurface *workerSurface = WorkerSurface::create(
        surfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{101});
    QVERIFY(workerSurface != nullptr);

    MainWindow window(routes(server.helpUrl()), server.origin(), workerSurface);
    window.resize(900, 600);
    window.show();
    QSignalSpy workerRouteSpy(&window, &MainWindow::workerRouteRequested);
    QSignalSpy currentUrlSpy(&window, &MainWindow::currentUrlChanged);
    QLineEdit *address = window.navigationBar()->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QToolButton *backButton = window.navigationBar()->findChild<QToolButton *>(
        QStringLiteral("navigation-back"));
    QToolButton *forwardButton = window.navigationBar()->findChild<QToolButton *>(
        QStringLiteral("navigation-forward"));
    QVERIFY(address != nullptr);
    QVERIFY(backButton != nullptr);
    QVERIFY(forwardButton != nullptr);

    const QString workerAppUrl = QStringLiteral(
        "app://pilot/web-shaped-worker/order%2042?tab=summary");
    QVERIFY(window.navigate(workerAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.activeSurfaceCount(), 1);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyCount(), 1);
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(workerRouteSpy.count(), 1);
    QCOMPARE(workerRouteSpy.first().at(1).toString(), QStringLiteral("qml/Main.qml"));
    QCOMPARE(workerRouteSpy.first().at(2).toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("order 42"));
    QCOMPARE(window.surfaceStack()->currentWidget(), workerSurface);
    QVERIFY(workerSurface->isVisible());
    QVERIFY(!window.webSurface()->page()->isVisible());
    QCOMPARE(window.webSurface()->page()->renderProcessPid(), 0);

    const int workerCurrentUrlSignals = currentUrlSpy.count();
    address->setText(QStringLiteral("https://example.com/not-an-app-route"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyCount(), 1);
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(address->text(), workerAppUrl);
    QVERIFY(!backButton->isEnabled());
    QVERIFY(!forwardButton->isEnabled());
    QCOMPARE(currentUrlSpy.count(), workerCurrentUrlSignals);

    QVERIFY(window.navigate(workerAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.historyCount(), 1);
    QCOMPARE(window.historyIndex(), 0);

    const QString webAppUrl = QStringLiteral("app://pilot/worker-shaped-web");
    QSignalSpy webNavigationSpy(window.webSurface(), &WebSurface::navigationFinished);
    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.activeSurfaceCount(), 1);
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(window.surfaceStack()->currentWidget(), window.webSurface());
    QVERIFY(!workerSurface->isVisible());
    QCOMPARE(window.webSurface()->page()->lifecycleState(),
             QWebEnginePage::LifecycleState::Active);
    QVERIFY(waitForWebNavigation(window.webSurface(), server.helpUrl(), webNavigationSpy));
    QCOMPARE(window.webSurface()->page()->title(), QStringLiteral("Pilot help"));
    QVERIFY(server.requests().contains(QByteArrayLiteral("/help")));

    const QString missingAppUrl = QStringLiteral("app://pilot/not-registered");
    QVERIFY(!window.navigate(missingAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.activeSurfaceCount(), 1);
    QCOMPARE(window.currentAppUrl(), missingAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
    QVERIFY(window.trustedErrorText().contains(QStringLiteral("not found"),
                                               Qt::CaseInsensitive));
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QCOMPARE(window.webSurface()->page()->lifecycleState(),
             QWebEnginePage::LifecycleState::Frozen);

    QVERIFY(window.goBack());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 1);
    QVERIFY(window.goBack());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(window.webSurface()->page()->lifecycleState(),
             QWebEnginePage::LifecycleState::Frozen);
    QVERIFY(!window.goBack());
    QVERIFY(window.goForward());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyIndex(), 1);

    address->setText(missingAppUrl);
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.currentAppUrl(), missingAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
    QCOMPARE(address->text(), missingAppUrl);

    const int stableHistoryCount = window.historyCount();
    address->setText(QStringLiteral("https://example.com/"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.historyCount(), stableHistoryCount);
    QCOMPARE(window.currentAppUrl(), missingAppUrl);
    QCOMPARE(address->text(), missingAppUrl);
    QVERIFY(currentUrlSpy.count() >= 6);

    window.close();
    launch->hostSession.close();
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void UnifiedNavigationTest::workerNavigationIsSameAppAndHistoryAware()
{
    HelpServer server;
    QVERIFY(server.listen());
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Rectangle {
    width: 320; height: 200
    Timer { interval: 100; running: true; onTriggered: Runtime.navigate("/worker-shaped-web") }
    Timer { interval: 5000; running: true; onTriggered: Runtime.navigate("/orders") }
})QML");
    WorkerTestEnvironment workerEnvironment(qml);
    QVERIFY2(workerEnvironment.isValid(), qPrintable(workerEnvironment.error()));
    auto launch = workerEnvironment.launch(QStringLiteral("worker-request-nonce"),
                                           QStringLiteral("worker-request-nonce"), 100);
    QVERIFY2(launch.has_value(), qPrintable(workerEnvironment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    WorkerSurface *workerSurface = WorkerSurface::create(
        surfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{102});
    QVERIFY(workerSurface != nullptr);
    MainWindow window(routes(server.helpUrl(), workerEnvironment.appId()),
                      server.origin(), workerSurface);
    window.resize(900, 600);
    window.show();
    const QString initial = QStringLiteral("app://pilot/web-shaped-worker/42");
    QVERIFY(window.navigate(initial));

    HostWorkerSessionController controller(&window);
    QVERIFY(controller.attach(std::make_unique<IpcSession>(
        std::move(launch->hostSession))));
    QSignalSpy routeLoadSpy(&controller,
                            &HostWorkerSessionController::routeLoadAcknowledged);
    QTest::qWait(250);
    QCOMPARE(window.currentAppUrl(), initial);
    QCOMPARE(window.historyCount(), 1);
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QTRY_COMPARE_WITH_TIMEOUT(window.currentAppUrl(), QStringLiteral("app://pilot/orders"),
                              5000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.pendingRouteLoadCount(), qsizetype(0), 5000);
    QCOMPARE(routeLoadSpy.count(), 1);
    QCOMPARE(routeLoadSpy.at(0).at(0).toString(), QStringLiteral("/orders"));
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/orders"));
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);
    QVERIFY(!window.navigateFromWorker(QStringLiteral("com.qbrowser.other"),
                                       QStringLiteral("/orders")));
    QVERIFY(!window.navigateFromWorker(workerEnvironment.appId(),
                                       QStringLiteral("https://evil.test/orders")));
    QVERIFY(!window.navigateFromWorker(workerEnvironment.appId(),
                                       QStringLiteral("/worker-shaped-web")));
    QCOMPARE(window.historyCount(), 2);
    QVERIFY(window.goBack());
    QTRY_COMPARE_WITH_TIMEOUT(routeLoadSpy.count(), 2, 5000);
    QCOMPARE(routeLoadSpy.at(1).at(0).toString(),
             QStringLiteral("/web-shaped-worker/42"));
    QCOMPARE(window.currentAppUrl(), initial);
    QVERIFY(window.goForward());
    QTRY_COMPARE_WITH_TIMEOUT(routeLoadSpy.count(), 3, 5000);
    QCOMPARE(routeLoadSpy.at(2).at(0).toString(), QStringLiteral("/orders"));
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/orders"));

    window.close();
    QVERIFY(controller.shutdown(QStringLiteral("navigation.complete")));
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void UnifiedNavigationTest::hostWorkerRoutesAreTrackedWithoutDuplicateWorkerNavigation()
{
    HelpServer server;
    QVERIFY(server.listen());
    WorkerTestEnvironment workerEnvironment;
    QVERIFY2(workerEnvironment.isValid(), qPrintable(workerEnvironment.error()));
    auto launch = workerEnvironment.launch(QStringLiteral("host-route-sync"),
                                           QStringLiteral("host-route-sync"), 100);
    QVERIFY2(launch.has_value(), qPrintable(workerEnvironment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    WorkerSurface *surface = WorkerSurface::create(
        surfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{103});
    QVERIFY(surface != nullptr);

    MainWindow window(routes(server.helpUrl(), workerEnvironment.appId()),
                      server.origin(), surface);
    window.resize(900, 600);
    window.show();
    HostWorkerSessionController controller(&window);
    QVERIFY(controller.attach(std::make_unique<IpcSession>(
        std::move(launch->hostSession))));
    QSignalSpy acknowledged(&controller,
                            &HostWorkerSessionController::routeLoadAcknowledged);

    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/7")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 1, 5000);
    QCOMPARE(acknowledged.at(0).at(0).toString(),
             QStringLiteral("/web-shaped-worker/7"));
    QVERIFY(window.navigate(QStringLiteral("app://pilot/orders")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 2, 5000);
    QCOMPARE(acknowledged.at(1).at(0).toString(), QStringLiteral("/orders"));
    QVERIFY(window.goBack());
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 3, 5000);
    QCOMPARE(acknowledged.at(2).at(0).toString(),
             QStringLiteral("/web-shaped-worker/7"));
    QVERIFY(window.goForward());
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 4, 5000);
    QCOMPARE(acknowledged.at(3).at(0).toString(), QStringLiteral("/orders"));

    bool reentered = false;
    const QMetaObject::Connection reentry = connect(
        &controller, &HostWorkerSessionController::routeLoadAcknowledged,
        &window, [&](const QString &route) {
            if (!reentered && route == QStringLiteral("/web-shaped-worker/9")) {
                reentered = true;
                QVERIFY(window.goBack());
            }
        }, Qt::DirectConnection);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/9")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 6, 5000);
    QVERIFY(reentered);
    QCOMPARE(acknowledged.at(4).at(0).toString(),
             QStringLiteral("/web-shaped-worker/9"));
    QCOMPARE(acknowledged.at(5).at(0).toString(), QStringLiteral("/orders"));
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/orders"));
    disconnect(reentry);

    const int stableCount = acknowledged.count();
    QVERIFY(!window.navigate(QStringLiteral("app://pilot/missing")));
    QTest::qWait(100);
    QCOMPARE(acknowledged.count(), stableCount);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/worker-shaped-web")));
    QTest::qWait(100);
    QCOMPARE(acknowledged.count(), stableCount);

    QVERIFY(controller.shutdown(QStringLiteral("route-sync.complete")));
    window.close();
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void UnifiedNavigationTest::stalledWorkerReaderNeverBlocksTheGuiThread()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    auto sessions = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(sessions.has_value());
    QVERIFY(controller.attach(std::move(sessions->host)));

    std::atomic_bool writerFinished = false;
    std::thread writer([worker = std::move(sessions->worker), &writerFinished]() mutable {
        for (int index = 0; index < 2000; ++index) {
            const auto request = ProtocolMessage::request(
                QStringLiteral("flood-%1").arg(index),
                QStringLiteral("test"), QStringLiteral("stall"), QJsonObject{});
            if (!request.has_value() || !worker->send(*request, 100)) {
                break;
            }
        }
        writerFinished = true;
        worker->close();
    });

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 previousTick = elapsed.elapsed();
    qint64 maximumGap = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, &window, [&] {
        const qint64 now = elapsed.elapsed();
        maximumGap = std::max(maximumGap, now - previousTick);
        previousTick = now;
    });
    heartbeat.start();
    QTest::qWait(1500);
    heartbeat.stop();
    writer.join();

    QVERIFY2(maximumGap < 100,
             qPrintable(QStringLiteral("GUI heartbeat stalled for %1 ms")
                            .arg(maximumGap)));
    QVERIFY(writerFinished.load());
}

void UnifiedNavigationTest::gracefulShutdownCleansIoBeforeReattach()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    QElapsedTimer responsiveness;
    responsiveness.start();
    qint64 previousHeartbeat = responsiveness.elapsed();
    qint64 maximumHeartbeatGap = 0;
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, &window, [&] {
        const qint64 now = responsiveness.elapsed();
        maximumHeartbeatGap = std::max(maximumHeartbeatGap, now - previousHeartbeat);
        previousHeartbeat = now;
        ++heartbeatCount;
    });
    heartbeat.start();
    auto first = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(first.has_value());
    QVERIFY(controller.attach(std::move(first->host)));
    QVERIFY(controller.hasIoThread());

    std::thread firstPeer([worker = std::move(first->worker)]() mutable {
        const SessionReceiveResult request = worker->receive(5000);
        if (request.status == SessionStatus::MessageReady
            && request.message->type() == ProtocolType::Shutdown) {
            const auto acknowledgement = ProtocolMessage::shutdown(
                QStringLiteral("worker.ack"));
            if (acknowledgement.has_value())
                (void)worker->send(*acknowledgement, 1000);
        }
        worker->close();
    });
    QVERIFY(controller.shutdown(QStringLiteral("lifecycle.first")));
    QCOMPARE(controller.state(), HostWorkerSessionState::ShuttingDown);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Detached, 6000);
    QVERIFY(!controller.hasIoThread());
    firstPeer.join();
#ifdef Q_OS_WIN
    QTest::qWait(500);
    const DWORD handlesAfterFirstCycle = processHandleCount();
    QVERIFY(handlesAfterFirstCycle > 0);
#endif

    auto second = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(second.has_value());
    QVERIFY(controller.attach(std::move(second->host)));
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QVERIFY(controller.hasIoThread());
    std::thread secondPeer([worker = std::move(second->worker)]() mutable {
        const SessionReceiveResult request = worker->receive(5000);
        if (request.status == SessionStatus::MessageReady
            && request.message->type() == ProtocolType::Shutdown) {
            const auto acknowledgement = ProtocolMessage::shutdown(
                QStringLiteral("worker.ack"));
            if (acknowledgement.has_value())
                (void)worker->send(*acknowledgement, 1000);
        }
        worker->close();
    });
    QVERIFY(controller.shutdown(QStringLiteral("lifecycle.second")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Detached, 6000);
    QVERIFY(!controller.hasIoThread());
    secondPeer.join();
    heartbeat.stop();
    QVERIFY(heartbeatCount > 0);
    QVERIFY2(maximumHeartbeatGap < 100,
             qPrintable(QStringLiteral("GUI lifecycle heartbeat stalled for %1 ms")
                            .arg(maximumHeartbeatGap)));
#ifdef Q_OS_WIN
    QTest::qWait(500);
    const DWORD handlesAfter = processHandleCount();
    QVERIFY2(handlesAfter <= handlesAfterFirstCycle + 2,
             qPrintable(QStringLiteral("reattached worker session leaked process handles: %1 -> %2")
                            .arg(handlesAfterFirstCycle).arg(handlesAfter)));
#endif
}

void UnifiedNavigationTest::failedSessionCanReattachBeforeOldCallbacksDrain()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    auto first = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    auto replacement = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(first.has_value());
    QVERIFY(replacement.has_value());
    std::unique_ptr<IpcSession> replacementHost = std::move(replacement->host);
    bool replacementAccepted = false;
    connect(&controller, &HostWorkerSessionController::failed, &window,
            [&](const QString &) {
                if (!replacementAccepted && replacementHost != nullptr)
                    replacementAccepted = controller.attach(std::move(replacementHost));
            }, Qt::DirectConnection);

    QVERIFY(controller.attach(std::move(first->host)));
    first->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(replacementAccepted, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Running, 6000);
    QVERIFY(controller.hasIoThread());
    QTest::qWait(100);
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QVERIFY(controller.hasIoThread());

    replacement->worker->close();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Failed, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasIoThread(), 6000);
}

void UnifiedNavigationTest::reattachAfterIoThreadFinishedBeforeGuiCleanup()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    std::atomic_bool stoppingEntered = false;
    std::binary_semaphore allowThreadQuit(0);
    bool threadQuitAllowed = false;
    [[maybe_unused]] const auto resetHooks = qScopeGuard([&] {
        qbrowser_host_testing::resetHostWorkerSessionTestHooks();
        if (!threadQuitAllowed) allowThreadQuit.release();
    });
    qbrowser_host_testing::HostWorkerSessionTestHooks hooks;
    hooks.beforeIoThreadQuit = [&](quint64) {
        stoppingEntered.store(true);
        allowThreadQuit.acquire();
    };
    qbrowser_host_testing::setHostWorkerSessionTestHooks(std::move(hooks));

    auto first = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    auto replacement = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(first.has_value());
    QVERIFY(replacement.has_value());
    QVERIFY(controller.attach(std::move(first->host)));
    std::thread firstPeer([worker = std::move(first->worker)]() mutable {
        const SessionReceiveResult request = worker->receive(5000);
        if (request.status == SessionStatus::MessageReady
            && request.message->type() == ProtocolType::Shutdown) {
            const auto acknowledgement = ProtocolMessage::shutdown(
                QStringLiteral("worker.ack"));
            if (acknowledgement.has_value())
                (void)worker->send(*acknowledgement, 1000);
        }
        worker->close();
    });
    QVERIFY(controller.shutdown(QStringLiteral("barrier.reattach")));
    QTRY_VERIFY_WITH_TIMEOUT(stoppingEntered.load(), 5000);
    allowThreadQuit.release();
    threadQuitAllowed = true;
    QThread::msleep(100);
    QVERIFY(controller.hasIoThread());
    QVERIFY(!controller.ioThreadRunning());
    qbrowser_host_testing::resetHostWorkerSessionTestHooks();

    QVERIFY(controller.attach(std::move(replacement->host)));
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Running, 5000);
    QVERIFY(controller.ioThreadRunning());
    firstPeer.join();

    std::thread replacementPeer([worker = std::move(replacement->worker)]() mutable {
        const SessionReceiveResult request = worker->receive(5000);
        if (request.status == SessionStatus::MessageReady
            && request.message->type() == ProtocolType::Shutdown) {
            const auto acknowledgement = ProtocolMessage::shutdown(
                QStringLiteral("worker.ack"));
            if (acknowledgement.has_value())
                (void)worker->send(*acknowledgement, 1000);
        }
        worker->close();
    });
    QVERIFY(controller.shutdown(QStringLiteral("barrier.replacement")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Detached, 5000);
    replacementPeer.join();
}

void UnifiedNavigationTest::destroyAfterIoThreadFinishedBeforeGuiCleanup()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    auto controller = std::make_unique<HostWorkerSessionController>(&window);
    std::atomic_bool stoppingEntered = false;
    std::binary_semaphore allowThreadQuit(0);
    bool threadQuitAllowed = false;
    [[maybe_unused]] const auto resetHooks = qScopeGuard([&] {
        qbrowser_host_testing::resetHostWorkerSessionTestHooks();
        if (!threadQuitAllowed) allowThreadQuit.release();
    });
    qbrowser_host_testing::HostWorkerSessionTestHooks hooks;
    hooks.beforeIoThreadQuit = [&](quint64) {
        stoppingEntered.store(true);
        allowThreadQuit.acquire();
    };
    qbrowser_host_testing::setHostWorkerSessionTestHooks(std::move(hooks));

    auto sessions = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(sessions.has_value());
    QVERIFY(controller->attach(std::move(sessions->host)));
    std::thread peer([worker = std::move(sessions->worker)]() mutable {
        const SessionReceiveResult request = worker->receive(5000);
        if (request.status == SessionStatus::MessageReady
            && request.message->type() == ProtocolType::Shutdown) {
            const auto acknowledgement = ProtocolMessage::shutdown(
                QStringLiteral("worker.ack"));
            if (acknowledgement.has_value())
                (void)worker->send(*acknowledgement, 1000);
        }
        worker->close();
    });
    QVERIFY(controller->shutdown(QStringLiteral("barrier.destroy")));
    QTRY_VERIFY_WITH_TIMEOUT(stoppingEntered.load(), 5000);
    allowThreadQuit.release();
    threadQuitAllowed = true;
    QThread::msleep(100);
    QVERIFY(controller->hasIoThread());
    QVERIFY(!controller->ioThreadRunning());
    qbrowser_host_testing::resetHostWorkerSessionTestHooks();

    QElapsedTimer elapsed;
    elapsed.start();
    controller.reset();
    QVERIFY2(elapsed.elapsed() < 100,
             qPrintable(QStringLiteral("controller destruction blocked GUI for %1 ms")
                            .arg(elapsed.elapsed())));
    peer.join();
}

void UnifiedNavigationTest::hostApplicationOwnsAttachableWorkerSessionController()
{
    HelpServer server;
    QVERIFY(server.listen());
    HostApplication application(server.origin());
    QVERIFY(application.start());
    QVERIFY(application.mainWindow() != nullptr);
    QVERIFY(application.workerSessionController() != nullptr);

    auto sessions = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(sessions.has_value());
    QVERIFY(application.attachWorkerSession(std::move(sessions->host)));
    QCOMPARE(application.workerSessionController()->state(),
             HostWorkerSessionState::Running);
    sessions->worker->close();
    QTRY_COMPARE_WITH_TIMEOUT(application.workerSessionController()->state(),
                              HostWorkerSessionState::Failed, 2000);
    QCOMPARE(application.workerSessionController()->lastErrorCode(),
             QStringLiteral("ipc.session.peer_closed"));
    QCOMPARE(application.workerSessionController()->state(),
             HostWorkerSessionState::Failed);
    application.mainWindow()->close();
}

void UnifiedNavigationTest::hostApplicationBindsWorkerContextLifecycle()
{
    HelpServer server;
    QVERIFY(server.listen());
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("host-context"),
                                     QStringLiteral("host-context"), 100);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    auto process = std::make_shared<SandboxProcess>(std::move(launch->process));
    WorkerSurface *surface = WorkerSurface::create(
        surfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        process->nativeProcessHandle(), WorkerAttemptId{104});
    QVERIFY(surface != nullptr);

    HostApplication application(server.origin());
    QVERIFY(application.start());
    HostWorkerAttachContext context;
    context.session = std::make_unique<IpcSession>(std::move(launch->hostSession));
    context.surface = surface;
    context.processLifetime = process;
    context.stopProcess = [process] { process->terminate(ERROR_PROCESS_ABORTED); };
    QVERIFY(application.attachWorkerContext(std::move(context)));
    QVERIFY(application.hasWorkerContext());
    QCOMPARE(application.mainWindow()->workerSurface(), surface);
    QCOMPARE(application.workerSessionController()->state(),
             HostWorkerSessionState::Running);

    application.detachWorkerContext(QStringLiteral("context.test.complete"));
    QVERIFY(!application.hasWorkerContext());
    QCOMPARE(application.mainWindow()->workerSurface(), nullptr);
    QVERIFY(process->waitForFinished(5000));
    const auto closed = process->close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
    application.mainWindow()->close();
}

void UnifiedNavigationTest::navigationTransactionsRejectReentrantCommands()
{
    HelpServer server;
    QVERIFY(server.listen());

    MainWindow window(routes(server.helpUrl()), server.origin());
    const QString webAppUrl = QStringLiteral("app://pilot/worker-shaped-web");
    const QString unavailableWorkerAppUrl =
        QStringLiteral("app://pilot/web-shaped-worker/42");
    QLineEdit *address = window.navigationBar()->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QToolButton *backButton = window.navigationBar()->findChild<QToolButton *>(
        QStringLiteral("navigation-back"));
    QToolButton *forwardButton = window.navigationBar()->findChild<QToolButton *>(
        QStringLiteral("navigation-forward"));
    QVERIFY(address != nullptr);
    QVERIFY(backButton != nullptr);
    QVERIFY(forwardButton != nullptr);

    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.historyCount(), 1);

    bool navigateReceiverRan = false;
    bool navigateReceiverSawCommittedState = false;
    bool reentrantNavigateResult = true;
    const QMetaObject::Connection navigateConnection = connect(
        &window, &MainWindow::currentUrlChanged, &window,
        [&](const QString &url) {
            if (navigateReceiverRan || url != unavailableWorkerAppUrl) {
                return;
            }
            navigateReceiverRan = true;
            navigateReceiverSawCommittedState =
                window.currentAppUrl() == unavailableWorkerAppUrl
                && window.historyCount() == 2
                && window.historyIndex() == 1
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantNavigateResult = window.navigate(unavailableWorkerAppUrl);
        },
        Qt::DirectConnection);

    QVERIFY(!window.navigate(unavailableWorkerAppUrl));
    QVERIFY(navigateReceiverRan);
    QVERIFY(navigateReceiverSawCommittedState);
    QVERIFY(!reentrantNavigateResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    disconnect(navigateConnection);

    const int historyBeforeDuplicate = window.historyCount();
    QVERIFY(!window.navigate(unavailableWorkerAppUrl));
    QCOMPARE(window.historyCount(), historyBeforeDuplicate);
    QCOMPARE(window.historyIndex(), historyBeforeDuplicate - 1);

    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);

    bool backReceiverRan = false;
    bool backReceiverSawCommittedState = false;
    bool reentrantBackResult = true;
    const QMetaObject::Connection backConnection = connect(
        &window, &MainWindow::currentUrlChanged, &window,
        [&](const QString &url) {
            if (backReceiverRan || url != unavailableWorkerAppUrl) {
                return;
            }
            backReceiverRan = true;
            backReceiverSawCommittedState =
                window.currentAppUrl() == unavailableWorkerAppUrl
                && window.historyCount() == 3
                && window.historyIndex() == 1
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantBackResult = window.goBack();
        },
        Qt::DirectConnection);

    QVERIFY(window.goBack());
    QVERIFY(backReceiverRan);
    QVERIFY(backReceiverSawCommittedState);
    QVERIFY(!reentrantBackResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    disconnect(backConnection);

    QVERIFY(window.goBack());
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);

    QSignalSpy invalidInputUrlSpy(&window, &MainWindow::currentUrlChanged);
    address->setText(QStringLiteral("file:///C:/Windows/win.ini"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(address->text(), webAppUrl);
    QVERIFY(!backButton->isEnabled());
    QVERIFY(forwardButton->isEnabled());
    QCOMPARE(invalidInputUrlSpy.count(), 0);

    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 0);
    QVERIFY(!backButton->isEnabled());
    QVERIFY(forwardButton->isEnabled());

    bool forwardReceiverRan = false;
    bool forwardReceiverSawCommittedState = false;
    bool reentrantForwardResult = true;
    const QMetaObject::Connection forwardConnection = connect(
        &window, &MainWindow::currentUrlChanged, &window,
        [&](const QString &url) {
            if (forwardReceiverRan || url != unavailableWorkerAppUrl) {
                return;
            }
            forwardReceiverRan = true;
            forwardReceiverSawCommittedState =
                window.currentAppUrl() == unavailableWorkerAppUrl
                && window.historyCount() == 3
                && window.historyIndex() == 1
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantForwardResult = window.goForward();
        },
        Qt::DirectConnection);

    QVERIFY(window.goForward());
    QVERIFY(forwardReceiverRan);
    QVERIFY(forwardReceiverSawCommittedState);
    QVERIFY(!reentrantForwardResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    disconnect(forwardConnection);
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-host-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    UnifiedNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_unified_navigation.moc"
