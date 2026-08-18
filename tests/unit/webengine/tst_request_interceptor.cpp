#include "PilotRequestInterceptor.h"
#include "WebSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>

#include <functional>
#include <optional>

namespace {

class HttpServer final : public QObject
{
public:
    using Responder = std::function<QByteArray(const QByteArray &)>;

    explicit HttpServer(Responder responder = {}, QObject *parent = nullptr)
        : QObject(parent), responder_(std::move(responder))
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket] { respond(socket); });
            }
        });
    }

    bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server_.serverPort()));
    }

    QUrl url(const QString &path) const
    {
        return origin().resolved(QUrl(path));
    }

    QList<QByteArray> requests() const { return requests_; }
    void close() { server_.close(); }

private:
    void respond(QTcpSocket *socket)
    {
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
        const QList<QByteArray> requestLine = request.first(lineEnd).split(' ');
        if (requestLine.size() < 2) {
            socket->disconnectFromHost();
            return;
        }
        const QByteArray target = requestLine.at(1);
        requests_.append(target);
        const QByteArray body = responder_ ? responder_(target)
                                            : QByteArrayLiteral("<!doctype html><title>ok</title>");
        const QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
            + QByteArrayLiteral("Content-Type: text/html; charset=utf-8\r\n")
            + QByteArrayLiteral("Cache-Control: no-store\r\nConnection: close\r\n")
            + QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    Responder responder_;
    QList<QByteArray> requests_;
};

bool waitForLoad(WebSurface &surface,
                 const QUrl &expected,
                 QSignalSpy &spy,
                 const int timeoutMs = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (!spy.wait(remaining)) {
            return false;
        }
        if (surface.currentUrl() == expected && !surface.page()->isLoading()) {
            return true;
        }
    }
    return false;
}

bool navigateAndWait(WebSurface &surface,
                     const QUrl &requested,
                     const QUrl &expected,
                     const bool expectedAccepted = true,
                     const int timeoutMs = 10000)
{
    QSignalSpy spy(&surface, &WebSurface::navigationFinished);
    if (surface.navigate(requested) != expectedAccepted) {
        return false;
    }
    return waitForLoad(surface, expected, spy, timeoutMs);
}

std::optional<QJsonValue> evaluateJavaScript(QWebEnginePage *page,
                                             const QString &script,
                                             const int timeoutMs = 5000)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::optional<QJsonValue> result;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->runJavaScript(script, [&result, &loop](const QVariant &value) {
        result.emplace(QJsonValue::fromVariant(value));
        loop.quit();
    });
    timeout.start(timeoutMs);
    loop.exec();
    return result;
}

} // namespace

class RequestInterceptorTest final : public QObject
{
    Q_OBJECT

private slots:
    void policyAllowsOnlyExactLoopbackOriginAndTrustedError();
    void rejectsUnsafeSandboxConfiguration();
    void invalidOriginDoesNotInitializeWebEngine();
    void createsDedicatedEphemeralProfileWithRestrictiveSettings();
    void actualWebEngineBlocksCrossOriginSubresources();
    void actualWebEngineIsolatesLocalStorageBetweenSurfaces();
    void actualWebEngineDeniesPopupsDownloadsAndPermissions();
    void blockedAndFailedLoadsRenderTrustedQrcError();
    void rendererTerminationRendersTrustedQrcError();
};

void RequestInterceptorTest::policyAllowsOnlyExactLoopbackOriginAndTrustedError()
{
    const QUrl origin(QStringLiteral("http://127.0.0.1:43123/"));
    PilotRequestInterceptor interceptor(origin);

    QVERIFY(interceptor.isConfigurationValid());
    QVERIFY(interceptor.isAllowed(QUrl(QStringLiteral("http://127.0.0.1:43123/help?q=1"))));
    QVERIFY(interceptor.isAllowed(WebSurface::trustedErrorUrl()));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("http://127.0.0.1:43124/help"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("http://localhost:43123/help"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("https://127.0.0.1:43123/help"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("https://example.com/"))));
    QVERIFY(!interceptor.isAllowed(QUrl::fromLocalFile(QStringLiteral("C:/Windows/win.ini"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("qrc:/web/other.html"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("data:text/html,unsafe"))));
    QVERIFY(!interceptor.isAllowed(QUrl(QStringLiteral("javascript:alert(1)"))));

    PilotRequestInterceptor hostnameOrigin(
        QUrl(QStringLiteral("http://localhost:43123/")));
    QVERIFY(!hostnameOrigin.isConfigurationValid());
    QVERIFY(!hostnameOrigin.isAllowed(origin));
}

void RequestInterceptorTest::rejectsUnsafeSandboxConfiguration()
{
    QVERIFY(WebSurface::isChromiumSandboxConfigurationSafe({}, {}, {}));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {QStringLiteral("qbrowser-host"), QStringLiteral("--no-sandbox")}, {}, {}));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {}, QByteArrayLiteral("1"), {}));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {}, {}, QByteArrayLiteral("--disable-gpu --no-sandbox")));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {}, {}, QByteArrayLiteral("--disable-setuid-sandbox")));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {}, {}, QByteArrayLiteral("\"--no-sandbox\"")));
    QVERIFY(!WebSurface::isChromiumSandboxConfigurationSafe(
        {}, {}, QByteArrayLiteral("--disable-gpu \"--disable-setuid-sandbox\"")));
}

void RequestInterceptorTest::invalidOriginDoesNotInitializeWebEngine()
{
    WebSurface surface(QUrl(QStringLiteral("https://example.com/")));

    QVERIFY(!surface.isConfigurationValid());
    QVERIFY(surface.profile() == nullptr);
    QVERIFY(surface.page() == nullptr);
    QVERIFY(surface.view() == nullptr);
    QVERIFY(!surface.navigate(QUrl(QStringLiteral("https://example.com/help"))));
    QVERIFY(surface.currentUrl().isEmpty());
}

void RequestInterceptorTest::createsDedicatedEphemeralProfileWithRestrictiveSettings()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSurface surface(server.origin());

    QVERIFY(surface.isConfigurationValid());
    QVERIFY(surface.profile() != QWebEngineProfile::defaultProfile());
    QVERIFY(surface.profile()->isOffTheRecord());
    QCOMPARE(surface.profile()->httpCacheType(), QWebEngineProfile::NoCache);
    QCOMPARE(surface.profile()->persistentCookiesPolicy(),
             QWebEngineProfile::NoPersistentCookies);
    QCOMPARE(surface.profile()->persistentPermissionsPolicy(),
             QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    QVERIFY(surface.profile()->storageName().isEmpty());
    QCOMPARE(surface.page()->profile(), surface.profile());
    QVERIFY(!surface.page()->settings()->testAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows));
    QVERIFY(!surface.page()->settings()->testAttribute(
        QWebEngineSettings::JavascriptCanAccessClipboard));
    QVERIFY(!surface.page()->settings()->testAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls));
    QVERIFY(!surface.page()->settings()->testAttribute(
        QWebEngineSettings::LocalContentCanAccessFileUrls));
    QVERIFY(surface.page()->settings()->testAttribute(
        QWebEngineSettings::LocalStorageEnabled));
    QCOMPARE(surface.page()->settings()->unknownUrlSchemePolicy(),
             QWebEngineSettings::DisallowUnknownUrlSchemes);
}

void RequestInterceptorTest::actualWebEngineBlocksCrossOriginSubresources()
{
    HttpServer blocked;
    QVERIFY(blocked.listen());
    const QByteArray blockedUrl = blocked.url(QStringLiteral("/leak")).toString().toUtf8();
    HttpServer allowed([blockedUrl](const QByteArray &) {
        return QByteArrayLiteral("<!doctype html><title>loading</title><img src=\"")
            + blockedUrl
            + QByteArrayLiteral("\" onerror=\"document.title='blocked'\" ")
            + QByteArrayLiteral("onload=\"document.title='leaked'\">");
    });
    QVERIFY(allowed.listen());
    WebSurface surface(allowed.origin());
    QSignalSpy blockedSpy(surface.requestInterceptor(),
                          &PilotRequestInterceptor::requestBlocked);
    QSignalSpy titleSpy(surface.page(), &QWebEnginePage::titleChanged);

    QVERIFY(navigateAndWait(surface,
                            allowed.url(QStringLiteral("/index")),
                            allowed.url(QStringLiteral("/index"))));
    QTRY_COMPARE_WITH_TIMEOUT(surface.page()->title(), QStringLiteral("blocked"), 10000);
    QVERIFY(blockedSpy.count() >= 1);
    QVERIFY(blocked.requests().isEmpty());
    QVERIFY(allowed.requests().contains(QByteArrayLiteral("/index")));
    QVERIFY(!titleSpy.isEmpty());
}

void RequestInterceptorTest::actualWebEngineIsolatesLocalStorageBetweenSurfaces()
{
    HttpServer server([](const QByteArray &) {
        return QByteArrayLiteral("<!doctype html><title>storage</title>");
    });
    QVERIFY(server.listen());
    WebSurface first(server.origin());
    WebSurface second(server.origin());

    QVERIFY(navigateAndWait(first,
                            server.url(QStringLiteral("/first")),
                            server.url(QStringLiteral("/first"))));
    const auto stored = evaluateJavaScript(
        first.page(), QStringLiteral("localStorage.setItem('pilot-secret', 'one'); "
                                     "localStorage.getItem('pilot-secret')"));
    QVERIFY(stored.has_value());
    QCOMPARE(*stored, QJsonValue(QStringLiteral("one")));

    QVERIFY(navigateAndWait(second,
                            server.url(QStringLiteral("/second")),
                            server.url(QStringLiteral("/second"))));
    const auto isolated = evaluateJavaScript(
        second.page(), QStringLiteral("localStorage.getItem('pilot-secret')"));
    QVERIFY(isolated.has_value());
    QVERIFY(isolated->isNull());
}

void RequestInterceptorTest::actualWebEngineDeniesPopupsDownloadsAndPermissions()
{
    HttpServer server([](const QByteArray &target) {
        if (target == QByteArrayLiteral("/download")) {
            return QByteArrayLiteral("download-body");
        }
        return QByteArrayLiteral(
            "<!doctype html><title>actions</title>"
            "<a id='popup' href='/popup' target='_blank'>popup</a>"
            "<a id='download' href='/download' download='blocked.txt'>download</a>");
    });
    QVERIFY(server.listen());
    WebSurface surface(server.origin());
    QSignalSpy downloadSpy(&surface, &WebSurface::downloadDenied);
    QSignalSpy permissionSpy(&surface, &WebSurface::permissionDenied);

    QVERIFY(navigateAndWait(surface,
                            server.url(QStringLiteral("/actions")),
                            server.url(QStringLiteral("/actions"))));
    const auto popupResult = evaluateJavaScript(
        surface.page(), QStringLiteral("window.open('/popup') === null"));
    QVERIFY(popupResult.has_value());
    QCOMPARE(*popupResult, QJsonValue(true));
    const auto popupClick = evaluateJavaScript(
        surface.page(), QStringLiteral("document.getElementById('popup').click()"));
    QVERIFY(popupClick.has_value());
    QTest::qWait(250);
    QCOMPARE(surface.currentUrl(), server.url(QStringLiteral("/actions")));
    QVERIFY(!server.requests().contains(QByteArrayLiteral("/popup")));

    const auto downloadClick = evaluateJavaScript(
        surface.page(), QStringLiteral("document.getElementById('download').click()"));
    QVERIFY(downloadClick.has_value());
    QTRY_VERIFY_WITH_TIMEOUT(!downloadSpy.isEmpty(), 5000);

    const auto permissionRequest = evaluateJavaScript(
        surface.page(), QStringLiteral(
            "Notification.requestPermission().then(result => result);"));
    QVERIFY(permissionRequest.has_value());
    QTRY_VERIFY_WITH_TIMEOUT(!permissionSpy.isEmpty(), 5000);
    const QUrl deniedOrigin = permissionSpy.takeFirst().at(0).toUrl();
    QCOMPARE(deniedOrigin.scheme(), QStringLiteral("http"));
    QCOMPARE(deniedOrigin.host(), QStringLiteral("127.0.0.1"));
}

void RequestInterceptorTest::blockedAndFailedLoadsRenderTrustedQrcError()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSurface surface(server.origin());

    QVERIFY(navigateAndWait(surface,
                            QUrl(QStringLiteral("https://example.com/")),
                            WebSurface::trustedErrorUrl(), false));
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());
    QCOMPARE(surface.page()->title(), QStringLiteral("Q-Browser navigation unavailable"));

    const QUrl offlineUrl = server.url(QStringLiteral("/offline"));
    server.close();
    QVERIFY(navigateAndWait(surface, offlineUrl, WebSurface::trustedErrorUrl(), true, 15000));
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());
}

void RequestInterceptorTest::rendererTerminationRendersTrustedQrcError()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSurface surface(server.origin());
    QVERIFY(navigateAndWait(surface,
                            server.url(QStringLiteral("/before-crash")),
                            server.url(QStringLiteral("/before-crash"))));
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 77)));

    QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty(), 10000);
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());
    QVERIFY(!surface.page()->isLoading());
    QCOMPARE(surface.page()->title(), QStringLiteral("Q-Browser navigation unavailable"));
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-webengine-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    RequestInterceptorTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_request_interceptor.moc"
