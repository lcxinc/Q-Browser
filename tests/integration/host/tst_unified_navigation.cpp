#include "MainWindow.h"
#include "NavigationBar.h"
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
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QWebEnginePage>

namespace {

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

RouteRegistry routes(const QUrl &helpUrl)
{
    RouteRegistry registry;
    const RouteRecord workerRoute{QStringLiteral("/web-shaped-worker/:id"),
                                  Engine::QmlWorker,
                                  QStringLiteral("com.qbrowser.pilot"),
                                  QStringLiteral("qml/Main.qml")};
    const RouteRecord webRoute{QStringLiteral("/worker-shaped-web"),
                               Engine::WebEngine,
                               QStringLiteral("com.qbrowser.web"),
                               helpUrl.toString(QUrl::FullyEncoded)};
    if (registry.add(workerRoute) != RouteAddResult::Added
        || registry.add(webRoute) != RouteAddResult::Added) {
        return {};
    }
    return registry;
}

bool waitForWebNavigation(WebSurface *surface,
                          const QUrl &expected,
                          QSignalSpy &spy)
{
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

} // namespace

class UnifiedNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void routeRegistryAloneSelectsOneActiveSurfaceAndStableHistory();
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

    QLineEdit *address = window.navigationBar()->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);
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

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-host-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    UnifiedNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_unified_navigation.moc"
