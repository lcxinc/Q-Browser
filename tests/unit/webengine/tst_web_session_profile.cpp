#include "HostApplication.h"
#include "MainWindow.h"
#include "NavigationBar.h"
#include "RouteRegistry.h"
#include "WebSessionProfile.h"
#include "WebSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonValue>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineLoadingInfo>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

struct HttpResponse final
{
    HttpResponse() = default;
    HttpResponse(QByteArray responseBody) : body(std::move(responseBody)) {}

    QByteArray status = QByteArrayLiteral("200 OK");
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
};

class HttpServer final : public QObject
{
public:
    using Responder = std::function<std::optional<HttpResponse>(const QByteArray &, int)>;

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

    ~HttpServer() override { abortHeldResponses(); }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }

    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/")
                        .arg(server_.serverPort()));
    }

    QUrl url(const QString &path) const { return origin().resolved(QUrl(path)); }

    QList<QByteArray> requests() const { return requests_; }

    void releaseHeldResponses(const QByteArray &body = QByteArrayLiteral("released"))
    {
        const QList<QPointer<QTcpSocket>> held = std::exchange(held_, {});
        for (const QPointer<QTcpSocket> &socket : held) {
            if (socket) sendResponse(socket, body);
        }
    }

    void abortHeldResponses()
    {
        const QList<QPointer<QTcpSocket>> held = std::exchange(held_, {});
        for (const QPointer<QTcpSocket> &socket : held) {
            if (socket) socket->abort();
        }
    }

private:
    static void sendResponse(QTcpSocket *socket, const HttpResponse &responseData)
    {
        QByteArray response = QByteArrayLiteral("HTTP/1.1 ") + responseData.status
            + QByteArrayLiteral("\r\nContent-Type: text/html; charset=utf-8\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n");
        for (const auto &[name, value] : responseData.headers) {
            response += name + QByteArrayLiteral(": ") + value
                + QByteArrayLiteral("\r\n");
        }
        response += QByteArrayLiteral("Content-Length: ")
            + QByteArray::number(responseData.body.size())
            + QByteArrayLiteral("\r\n\r\n") + responseData.body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    void respond(QTcpSocket *socket)
    {
        if (socket->property("responded").toBool()) return;
        QByteArray request = socket->property("requestBuffer").toByteArray();
        request += socket->readAll();
        if (!request.contains("\r\n\r\n")) {
            socket->setProperty("requestBuffer", request);
            return;
        }
        socket->setProperty("responded", true);
        const qsizetype lineEnd = request.indexOf("\r\n");
        if (lineEnd < 0) return;
        const QList<QByteArray> requestLine = request.first(lineEnd).split(' ');
        if (requestLine.size() < 2) {
            socket->disconnectFromHost();
            return;
        }
        const QByteArray target = requestLine.at(1);
        requests_.append(target);
        const int requestNumber = requests_.count(target);
        const std::optional<HttpResponse> response = responder_
            ? responder_(target, requestNumber)
            : std::optional<HttpResponse>(
                  HttpResponse{QByteArrayLiteral("<!doctype html><title>ok</title>")});
        if (!response.has_value()) {
            held_.append(socket);
            return;
        }
        sendResponse(socket, *response);
    }

    QTcpServer server_;
    Responder responder_;
    QList<QByteArray> requests_;
    QList<QPointer<QTcpSocket>> held_;
};

bool waitForLoad(WebSurface &surface,
                 const QUrl &expected,
                 QSignalSpy &spy,
                 const int timeoutMs = 10000)
{
    if (surface.currentUrl() == expected && !surface.page()->isLoading()
        && !spy.isEmpty()) {
        return true;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (!spy.wait(remaining)) return false;
        if (surface.currentUrl() == expected && !surface.page()->isLoading()) {
            return true;
        }
    }
    return false;
}

bool navigateAndWait(WebSurface &surface,
                     const QUrl &url,
                     const int timeoutMs = 10000)
{
    QSignalSpy spy(&surface, &WebSurface::navigationFinished);
    return surface.navigate(url) && waitForLoad(surface, url, spy, timeoutMs);
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

void drainQueuedEvents()
{
    for (int turn = 0; turn < 3; ++turn) {
        QEventLoop loop;
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
        loop.exec();
    }
}

QString deeplyPercentEncodedTitle(const QString &plainText, const int layers)
{
    Q_ASSERT(layers >= 1);
    const QByteArray bytes = plainText.toUtf8();
    QByteArray encoded;
    encoded.reserve(bytes.size() * 3);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : bytes) {
        encoded.append('%');
        encoded.append(hex[byte >> 4]);
        encoded.append(hex[byte & 0x0f]);
    }
    for (int layer = 1; layer < layers; ++layer) {
        encoded.replace("%", "%25");
    }
    return QString::fromLatin1(encoded);
}

RouteRegistry webRoutes(const QUrl &entry)
{
    RouteRegistry registry;
    const RouteRecord first{QStringLiteral("/first"), Engine::WebEngine,
                            QStringLiteral("com.qbrowser.web"),
                            entry.toString(QUrl::FullyEncoded)};
    const RouteRecord second{QStringLiteral("/second"), Engine::WebEngine,
                             QStringLiteral("com.qbrowser.web"),
                             entry.toString(QUrl::FullyEncoded)};
    if (registry.add(first) != RouteAddResult::Added
        || registry.add(second) != RouteAddResult::Added) {
        return {};
    }
    return registry;
}

class ObservableWebPage final : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

    [[nodiscard]] int destroyedReceiverCount() const
    {
        return receivers(SIGNAL(destroyed(QObject *)));
    }
};

} // namespace

class WebSessionProfileTest final : public QObject
{
    Q_OBJECT

private slots:
    void freshSessionIsEmptyAndEphemeral();
    void registrationIsBoundedAndObservable();
    void surfacesHaveIndependentPagesAndOneProfile();
    void sameOriginLocalStorageIsShared();
    void pageDomAndSessionStorageStayIndependentAcrossSwitches();
    void reloadFromAFreshSurfaceLoadsTheRegisteredEntry();
    void reloadFromTheTrustedErrorRetriesTheRegisteredEntry();
    void reloadAtTheRegisteredEntryPreservesSessionStorage();
    void currentLoadForwardsIntermediateProgress();
    void replacementRejectsQueuedPriorLoadProgress();
    void sameUrlReplacementRejectsQueuedOldCallbacks();
    void rendererDrivenReloadStartsAFreshIncarnation();
    void trustedTitlesAreBoundedAndUnicodeSafe();
    void maximumLengthNestedPercentTitleHasBoundedDecodeWork();
    void trustedTitlesRejectDeeplyEncodedPhysicalAddresses_data();
    void trustedTitlesRejectDeeplyEncodedPhysicalAddresses();
    void trustedTitlesRejectEncodedPhysicalAddresses();
    void mainFrameIsPinnedToItsRegisteredEntry();
    void redirectedMainFrameMustRemainTheRegisteredEntry();
    void surfaceSignalsAndTrustedTitlesAreIndependent();
    void stopDoesNotLoadTheTrustedErrorPage();
    void aNewNavigationIncarnationIgnoresTheStoppedLoad();
    void backgroundLifecycleWaitsUntilFreezeIsRecommended();
    void sessionRetirementSynchronouslyShutsDownItsSurfaces();
    void unregisterDisconnectsThePageDestructionObserver();
    void shutdownFailsUntilEveryPageIsUnregistered();
    void mainWindowShutdownCanBeRetriedAfterForeignSurfaceRetires();
    void reentrantZeroCountShutdownRetiresThePageBeforeTheProfile();
    void mainWindowRejectsCommandsAfterShutdown();
    void hostApplicationDoesNotRestartARetiredWindow();
};

void WebSessionProfileTest::freshSessionIsEmptyAndEphemeral()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());

    QVERIFY(session.isConfigurationValid());
    QCOMPARE(session.registeredPageCount(), 0);
    QVERIFY(session.profile() != nullptr);
    QVERIFY(session.profile() != QWebEngineProfile::defaultProfile());
    QVERIFY(session.profile()->isOffTheRecord());
    QVERIFY(session.profile()->storageName().isEmpty());
    QCOMPARE(session.profile()->httpCacheType(), QWebEngineProfile::NoCache);
    QCOMPARE(session.profile()->persistentCookiesPolicy(),
             QWebEngineProfile::NoPersistentCookies);
    QCOMPARE(session.profile()->persistentPermissionsPolicy(),
             QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    QVERIFY(session.requestInterceptor() != nullptr);
}

void WebSessionProfileTest::registrationIsBoundedAndObservable()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    QSignalSpy countSpy(&session, &WebSessionProfile::registeredPageCountChanged);
    std::vector<std::unique_ptr<WebSurface>> surfaces;
    surfaces.reserve(static_cast<std::size_t>(WebSessionProfile::maximumPageCount + 1));

    for (qsizetype index = 0; index < WebSessionProfile::maximumPageCount; ++index) {
        auto surface = std::make_unique<WebSurface>(
            session, server.url(QStringLiteral("/page-%1").arg(index)));
        QVERIFY(surface->isConfigurationValid());
        surfaces.push_back(std::move(surface));
    }
    QCOMPARE(session.registeredPageCount(), WebSessionProfile::maximumPageCount);
    QCOMPARE(countSpy.count(), static_cast<int>(WebSessionProfile::maximumPageCount));

    WebSurface overflow(session, server.url(QStringLiteral("/overflow")));
    QVERIFY(!overflow.isConfigurationValid());
    QVERIFY(overflow.page() == nullptr);
    QCOMPARE(session.registeredPageCount(), WebSessionProfile::maximumPageCount);

    surfaces.clear();
    QCOMPARE(session.registeredPageCount(), 0);

    auto page = std::make_unique<QWebEnginePage>(session.profile());
    QVERIFY(session.registerPage(page.get()));
    QCOMPARE(session.registeredPageCount(), 1);
    page.reset();
    QCOMPARE(session.registeredPageCount(), 0);
}

void WebSessionProfileTest::surfacesHaveIndependentPagesAndOneProfile()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface first(session, server.url(QStringLiteral("/first")));
    WebSurface second(session, server.url(QStringLiteral("/second")));

    QVERIFY(first.isConfigurationValid());
    QVERIFY(second.isConfigurationValid());
    QVERIFY(first.page() != second.page());
    QVERIFY(first.view() != second.view());
    QCOMPARE(first.profile(), session.profile());
    QCOMPARE(second.profile(), session.profile());
    QCOMPARE(first.page()->profile(), second.page()->profile());
    QCOMPARE(session.registeredPageCount(), 2);
}

void WebSessionProfileTest::sameOriginLocalStorageIsShared()
{
    HttpServer server([](const QByteArray &, int) {
        return std::optional<QByteArray>(
            QByteArrayLiteral("<!doctype html><title>storage</title>"));
    });
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface first(session, server.url(QStringLiteral("/first")));
    WebSurface second(session, server.url(QStringLiteral("/second")));

    QVERIFY(navigateAndWait(first, server.url(QStringLiteral("/first"))));
    const auto stored = evaluateJavaScript(
        first.page(), QStringLiteral("localStorage.setItem('shared', 'one');"
                                     "localStorage.getItem('shared')"));
    QVERIFY(stored.has_value());
    QCOMPARE(*stored, QJsonValue(QStringLiteral("one")));

    QVERIFY(navigateAndWait(second, server.url(QStringLiteral("/second"))));
    const auto shared = evaluateJavaScript(
        second.page(), QStringLiteral("localStorage.getItem('shared')"));
    QVERIFY(shared.has_value());
    QCOMPARE(*shared, QJsonValue(QStringLiteral("one")));
}

void WebSessionProfileTest::pageDomAndSessionStorageStayIndependentAcrossSwitches()
{
    HttpServer server([](const QByteArray &, int) {
        return std::optional<QByteArray>(
            QByteArrayLiteral("<!doctype html><title>state</title><body></body>"));
    });
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface first(session, server.url(QStringLiteral("/first")));
    WebSurface second(session, server.url(QStringLiteral("/second")));
    first.move(-10000, -10000);
    second.move(-10000, -10000);
    first.show();
    second.show();
    first.setTabActive(true);
    second.setTabActive(false);

    QVERIFY(navigateAndWait(first, server.url(QStringLiteral("/first"))));
    const auto firstStored = evaluateJavaScript(
        first.page(), QStringLiteral(
                          "document.body.dataset.page='one';"
                          "sessionStorage.setItem('per-page','one');true"));
    QVERIFY(firstStored.has_value());
    QCOMPARE(*firstStored, QJsonValue(true));

    first.setTabActive(false);
    second.setTabActive(true);
    QVERIFY(navigateAndWait(second, server.url(QStringLiteral("/second"))));
    const auto secondStored = evaluateJavaScript(
        second.page(), QStringLiteral(
                           "document.body.dataset.page='two';"
                           "sessionStorage.setItem('per-page','two');true"));
    QVERIFY(secondStored.has_value());
    QCOMPARE(*secondStored, QJsonValue(true));

    second.setTabActive(false);
    first.setTabActive(true);
    const auto firstState = evaluateJavaScript(
        first.page(), QStringLiteral(
                          "document.body.dataset.page+'|'+"
                          "sessionStorage.getItem('per-page')"));
    QVERIFY(firstState.has_value());
    QCOMPARE(*firstState, QJsonValue(QStringLiteral("one|one")));

    first.setTabActive(false);
    second.setTabActive(true);
    const auto secondState = evaluateJavaScript(
        second.page(), QStringLiteral(
                           "document.body.dataset.page+'|'+"
                           "sessionStorage.getItem('per-page')"));
    QVERIFY(secondState.has_value());
    QCOMPARE(*secondState, QJsonValue(QStringLiteral("two|two")));
}

void WebSessionProfileTest::reloadFromAFreshSurfaceLoadsTheRegisteredEntry()
{
    HttpServer server([](const QByteArray &, int) {
        return std::optional<QByteArray>(
            QByteArrayLiteral("<!doctype html><title>Fresh reload</title>"));
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/fresh-reload"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(surface.currentUrl() != entry);
    QVERIFY(surface.reload());
    QVERIFY(waitForLoad(surface, entry, finishedSpy, 5000));
    QCOMPARE(surface.currentUrl(), entry);
    QCOMPARE(surface.title(), QStringLiteral("Fresh reload"));
    QCOMPARE(surface.loadProgress(), 100);
    QVERIFY(!surface.isLoading());
    QCOMPARE(server.requests().count(QByteArrayLiteral("/fresh-reload")), 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toUrl(), entry);
    QVERIFY(finishedSpy.at(0).at(1).toBool());
}

void WebSessionProfileTest::reloadFromTheTrustedErrorRetriesTheRegisteredEntry()
{
    HttpServer server([](const QByteArray &, int) {
        return std::optional<QByteArray>(QByteArrayLiteral(
            "<!doctype html><title>Recovered entry</title>"
            "<body data-page='recovered'></body>"));
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/recover"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy errorFinishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(!surface.navigate(QUrl(QStringLiteral("https://example.com/rejected"))));
    QVERIFY(waitForLoad(surface, WebSurface::trustedErrorUrl(), errorFinishedSpy));
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());

    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);
    QSignalSpy loadingSpy(&surface, &WebSurface::loadingChanged);
    QSignalSpy progressSpy(&surface, &WebSurface::loadProgressChanged);
    QSignalSpy titleSpy(&surface, &WebSurface::titleChanged);
    QSignalSpy rawLoadingSpy(surface.page(), &QWebEnginePage::loadingChanged);

    QVERIFY(surface.reload());
    QVERIFY(waitForLoad(surface, entry, finishedSpy, 5000));
    QTest::qWait(250);

    QCOMPARE(surface.currentUrl(), entry);
    QCOMPARE(surface.title(), QStringLiteral("Recovered entry"));
    QCOMPARE(surface.loadProgress(), 100);
    QVERIFY(!surface.isLoading());
    QCOMPARE(server.requests().count(QByteArrayLiteral("/recover")), 1);

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toUrl(), entry);
    QVERIFY(finishedSpy.at(0).at(1).toBool());
    QVERIFY(!loadingSpy.isEmpty());
    QVERIFY(loadingSpy.at(0).at(0).toBool());
    QVERIFY(!loadingSpy.last().at(0).toBool());
    QVERIFY(!progressSpy.isEmpty());
    QCOMPARE(progressSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(progressSpy.last().at(0).toInt(), 100);
    QVERIFY(!titleSpy.isEmpty());
    QCOMPARE(titleSpy.last().at(0).toString(), QStringLiteral("Recovered entry"));
    for (const QList<QVariant> &arguments : titleSpy) {
        QVERIFY(arguments.at(0).toString()
                != QStringLiteral("Q-Browser navigation unavailable"));
    }

    bool sawEntryStarted = false;
    bool sawEntrySucceeded = false;
    for (const QList<QVariant> &arguments : rawLoadingSpy) {
        const QWebEngineLoadingInfo information =
            arguments.at(0).value<QWebEngineLoadingInfo>();
        sawEntryStarted = sawEntryStarted
            || (information.url() == entry
                && information.status()
                    == QWebEngineLoadingInfo::LoadStartedStatus);
        sawEntrySucceeded = sawEntrySucceeded
            || (information.url() == entry
                && information.status()
                    == QWebEngineLoadingInfo::LoadSucceededStatus);
    }
    QVERIFY(sawEntryStarted);
    QVERIFY(sawEntrySucceeded);
}

void WebSessionProfileTest::reloadAtTheRegisteredEntryPreservesSessionStorage()
{
    HttpServer server([](const QByteArray &target, const int requestNumber) {
        if (target == QByteArrayLiteral("/ordinary-reload")
            && requestNumber > 1) {
            return std::optional<QByteArray>(
                QByteArrayLiteral("<!doctype html><title>Reloaded entry</title>"));
        }
        return std::optional<QByteArray>(
            QByteArrayLiteral("<!doctype html><title>Initial entry</title>"));
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/ordinary-reload"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);

    QVERIFY(navigateAndWait(surface, entry));
    const auto stored = evaluateJavaScript(
        surface.page(), QStringLiteral(
                            "sessionStorage.setItem('reload-state','preserved');"
                            "sessionStorage.getItem('reload-state')"));
    QVERIFY(stored.has_value());
    QCOMPARE(*stored, QJsonValue(QStringLiteral("preserved")));

    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);
    QVERIFY(surface.reload());
    QVERIFY(waitForLoad(surface, entry, finishedSpy, 5000));
    QCOMPARE(surface.currentUrl(), entry);
    QCOMPARE(surface.title(), QStringLiteral("Reloaded entry"));
    QCOMPARE(server.requests().count(QByteArrayLiteral("/ordinary-reload")), 2);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toUrl(), entry);
    QVERIFY(finishedSpy.at(0).at(1).toBool());

    const auto preserved = evaluateJavaScript(
        surface.page(), QStringLiteral("sessionStorage.getItem('reload-state')"));
    QVERIFY(preserved.has_value());
    QCOMPARE(*preserved, QJsonValue(QStringLiteral("preserved")));
}

void WebSessionProfileTest::currentLoadForwardsIntermediateProgress()
{
    HttpServer server([](const QByteArray &target, int)
                          -> std::optional<HttpResponse> {
        if (target.startsWith(QByteArrayLiteral("/held-progress-"))) {
            return std::nullopt;
        }
        return HttpResponse{QByteArrayLiteral(
            "<!doctype html><title>Live progress</title>"
            "<img src='/held-progress-a'>"
            "<img src='/held-progress-b'>"
            "<img src='/held-progress-c'>")};
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/live-progress"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy rawProgressSpy(surface.page(), &QWebEnginePage::loadProgress);
    QSignalSpy surfaceProgressSpy(&surface, &WebSurface::loadProgressChanged);
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(surface.navigate(entry));
    QTRY_VERIFY_WITH_TIMEOUT(
        server.requests().contains(QByteArrayLiteral("/held-progress-a")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        server.requests().contains(QByteArrayLiteral("/held-progress-b")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        server.requests().contains(QByteArrayLiteral("/held-progress-c")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(rawProgressSpy.cbegin(), rawProgressSpy.cend(),
                    [](const QList<QVariant> &arguments) {
                        const int progress = arguments.at(0).toInt();
                        return progress > 0 && progress < 100;
                    }),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(surfaceProgressSpy.cbegin(), surfaceProgressSpy.cend(),
                    [&rawProgressSpy](const QList<QVariant> &arguments) {
                        const int progress = arguments.at(0).toInt();
                        return progress > 0 && progress < 100
                            && std::any_of(
                                rawProgressSpy.cbegin(), rawProgressSpy.cend(),
                                [progress](const QList<QVariant> &rawArguments) {
                                    return rawArguments.at(0).toInt() == progress;
                                });
                    }),
        1000);
    QVERIFY(surface.loadProgress() > 0 && surface.loadProgress() < 100);
    QVERIFY(std::any_of(rawProgressSpy.cbegin(), rawProgressSpy.cend(),
                        [&surface](const QList<QVariant> &arguments) {
                            return arguments.at(0).toInt()
                                == surface.loadProgress();
                        }));

    server.releaseHeldResponses();
    QVERIFY(waitForLoad(surface, entry, finishedSpy));
    QCOMPARE(surface.loadProgress(), 100);
}

void WebSessionProfileTest::replacementRejectsQueuedPriorLoadProgress()
{
    HttpServer server([](const QByteArray &, const int requestNumber)
                          -> std::optional<HttpResponse> {
        if (requestNumber > 1) return std::nullopt;
        return HttpResponse{QByteArrayLiteral(
            "<!doctype html><title>Progress incarnation</title>")};
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/progress-incarnation"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QVERIFY(navigateAndWait(surface, entry));

    QVERIFY(surface.reload());
    QTRY_COMPARE_WITH_TIMEOUT(
        server.requests().count(QByteArrayLiteral("/progress-incarnation")), 2,
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(surface.isLoading(), 5000);
    QSignalSpy progressSpy(&surface, &WebSurface::loadProgressChanged);
    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "loadProgress", Qt::DirectConnection, Q_ARG(int, 37)));
    QTRY_COMPARE_WITH_TIMEOUT(surface.loadProgress(), 37, 1000);
    QVERIFY(std::any_of(progressSpy.cbegin(), progressSpy.cend(),
                        [](const QList<QVariant> &arguments) {
                            return arguments.at(0).toInt() == 37;
                        }));

    progressSpy.clear();
    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "loadProgress", Qt::DirectConnection, Q_ARG(int, 73)));
    QVERIFY(surface.reload());
    QTRY_COMPARE_WITH_TIMEOUT(
        server.requests().count(QByteArrayLiteral("/progress-incarnation")), 3,
        5000);
    drainQueuedEvents();

    QCOMPARE(surface.loadProgress(), 0);
    QVERIFY(std::none_of(progressSpy.cbegin(), progressSpy.cend(),
                         [](const QList<QVariant> &arguments) {
                             return arguments.at(0).toInt() == 73;
                         }));
}

void WebSessionProfileTest::sameUrlReplacementRejectsQueuedOldCallbacks()
{
    HttpServer server([](const QByteArray &target, const int requestNumber)
                          -> std::optional<HttpResponse> {
        if (target != QByteArrayLiteral("/same-url")) {
            return HttpResponse{QByteArrayLiteral("<!doctype html><title>other</title>")};
        }
        if (requestNumber == 2) return std::nullopt;
        return HttpResponse{QByteArrayLiteral(
            "<!doctype html><title>Initial document</title>"
            "<script>addEventListener('beforeunload',()=>{"
            "document.title='Stale old document'})</script>")};
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/same-url"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy rawLoadingSpy(surface.page(), &QWebEnginePage::loadingChanged);
    QVERIFY(navigateAndWait(surface, entry));
    QCOMPARE(surface.title(), QStringLiteral("Initial document"));

    std::optional<QWebEngineLoadingInfo> staleSuccess;
    for (const QList<QVariant> &arguments : rawLoadingSpy) {
        const QWebEngineLoadingInfo information =
            arguments.at(0).value<QWebEngineLoadingInfo>();
        if (information.url() == entry
            && information.status()
                == QWebEngineLoadingInfo::LoadSucceededStatus) {
            staleSuccess = information;
        }
    }
    QVERIFY(staleSuccess.has_value());

    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);
    QSignalSpy titleSpy(&surface, &WebSurface::titleChanged);
    QSignalSpy progressSpy(&surface, &WebSurface::loadProgressChanged);
    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "titleChanged", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("Stale queued title"))));
    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "loadProgress", Qt::QueuedConnection,
        Q_ARG(int, 73)));
    QVERIFY(QMetaObject::invokeMethod(
        surface.page(), "loadingChanged", Qt::QueuedConnection,
        Q_ARG(QWebEngineLoadingInfo, *staleSuccess)));

    QVERIFY(surface.reload());
    QTRY_COMPARE_WITH_TIMEOUT(
        server.requests().count(QByteArrayLiteral("/same-url")), 2, 5000);
    drainQueuedEvents();

    QVERIFY(finishedSpy.isEmpty());
    QCOMPARE(surface.title(), QStringLiteral("Initial document"));
    for (const QList<QVariant> &arguments : titleSpy) {
        QVERIFY(arguments.at(0).toString() != QStringLiteral("Stale queued title"));
        QVERIFY(arguments.at(0).toString() != QStringLiteral("Stale old document"));
    }
    for (const QList<QVariant> &arguments : progressSpy) {
        QVERIFY(arguments.at(0).toInt() != 73);
    }

    server.releaseHeldResponses(
        QByteArrayLiteral("<!doctype html><title>Replacement document</title>"));
    QVERIFY(waitForLoad(surface, entry, finishedSpy));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toUrl(), entry);
    QVERIFY(finishedSpy.at(0).at(1).toBool());
    QCOMPARE(surface.title(), QStringLiteral("Replacement document"));
    QCOMPARE(surface.loadProgress(), 100);
}

void WebSessionProfileTest::rendererDrivenReloadStartsAFreshIncarnation()
{
    HttpServer server([](const QByteArray &target, const int requestNumber)
                          -> std::optional<HttpResponse> {
        if (target == QByteArrayLiteral("/renderer-reload")
            && requestNumber == 2) {
            return std::nullopt;
        }
        return HttpResponse{QByteArrayLiteral(
            "<!doctype html><title>Renderer reload</title>"
            "<script>sessionStorage.setItem('renderer-state','preserved')</script>")};
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/renderer-reload"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QVERIFY(navigateAndWait(surface, entry));
    const quint64 previousIncarnation = surface.navigationIncarnationForTesting();
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    surface.page()->runJavaScript(QStringLiteral("location.reload()"));
    QTRY_COMPARE_WITH_TIMEOUT(
        server.requests().count(QByteArrayLiteral("/renderer-reload")), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(surface.isLoading(), 5000);
    QVERIFY(surface.navigationIncarnationForTesting() > previousIncarnation);

    server.releaseHeldResponses(
        QByteArrayLiteral("<!doctype html><title>Renderer replacement</title>"));
    QVERIFY(waitForLoad(surface, entry, finishedSpy));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(surface.title(), QStringLiteral("Renderer replacement"));
    const auto state = evaluateJavaScript(
        surface.page(), QStringLiteral("sessionStorage.getItem('renderer-state')"));
    QVERIFY(state.has_value());
    QCOMPARE(*state, QJsonValue(QStringLiteral("preserved")));
}

void WebSessionProfileTest::trustedTitlesAreBoundedAndUnicodeSafe()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface surface(session, server.url(QStringLiteral("/titles")));
    const QString fallback = QStringLiteral("Restricted web");
    const QString astral = QString::fromUcs4(U"\U0001F642");

    QCOMPARE(surface.trustedTitleForTesting(QStringLiteral("A benign title")),
             QStringLiteral("A benign title"));
    QCOMPARE(surface.trustedTitleForTesting(QString(255, u'a')), QString(255, u'a'));
    QCOMPARE(surface.trustedTitleForTesting(QString(256, u'a')), QString(256, u'a'));
    QCOMPARE(surface.trustedTitleForTesting(QString(257, u'a')), QString(256, u'a'));

    const QString exactlyBounded = QString(254, u'a') + astral;
    QCOMPARE(exactlyBounded.size(), 256);
    QCOMPARE(surface.trustedTitleForTesting(exactlyBounded), exactlyBounded);
    const QString splitAtBoundary = QString(255, u'a') + astral;
    QCOMPARE(splitAtBoundary.size(), 257);
    QCOMPARE(surface.trustedTitleForTesting(splitAtBoundary), QString(255, u'a'));

    QCOMPARE(surface.trustedTitleForTesting(QString(QChar(0xD800))), fallback);
    QCOMPARE(surface.trustedTitleForTesting(QString(QChar(0xDC00))), fallback);
    QCOMPARE(surface.trustedTitleForTesting(
                 QStringLiteral("safe") + QChar(0x0001) + QStringLiteral("unsafe")),
             fallback);
    QCOMPARE(surface.trustedTitleForTesting(
                 QStringLiteral("safe\u202Eunsafe")), fallback);
    QCOMPARE(surface.trustedTitleForTesting(QString(4096, u'a')), QString(256, u'a'));
    QCOMPARE(surface.trustedTitleForTesting(QString(4097, u'a')), fallback);
}

void WebSessionProfileTest::maximumLengthNestedPercentTitleHasBoundedDecodeWork()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface surface(session, server.url(QStringLiteral("/titles")));
    const QString maximumLengthTitle = QStringLiteral("a")
        + deeplyPercentEncodedTitle(QStringLiteral("%"), 2047);
    QCOMPARE(maximumLengthTitle.size(), 4096);
    QString trusted;

    QBENCHMARK {
        trusted = surface.trustedTitleForTesting(maximumLengthTitle);
    }

    QCOMPARE(trusted, QStringLiteral("Restricted web"));
}

void WebSessionProfileTest::trustedTitlesRejectDeeplyEncodedPhysicalAddresses_data()
{
    QTest::addColumn<QString>("title");
    QTest::addColumn<bool>("exceedsOutputBound");

    QTest::newRow("ipv4-reviewer-four-layers")
        << QStringLiteral(
               "%25252568%25252574%25252574%25252570%2525253A%2525252F"
               "%2525252F%25252531%25252532%25252537%2525252E%25252530"
               "%2525252E%25252530%2525252E%25252531%2525253A%25252534"
               "%25252533%25252531%25252532%25252533%2525252F%25252568"
               "%25252565%2525256C%25252570")
        << false;
    QTest::newRow("ipv6-five-layers")
        << deeplyPercentEncodedTitle(
               QStringLiteral("http://[::1]:43123/help"), 5)
        << false;
    QTest::newRow("localhost-near-raw-bound")
        << deeplyPercentEncodedTitle(
               QStringLiteral("http://localhost:43123/help"), 70)
        << true;
}

void WebSessionProfileTest::trustedTitlesRejectDeeplyEncodedPhysicalAddresses()
{
    QFETCH(QString, title);
    QFETCH(bool, exceedsOutputBound);
    QVERIFY(title.size() <= 4096);
    QCOMPARE(title.size() > 256, exceedsOutputBound);

    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface surface(session, server.url(QStringLiteral("/titles")));
    QCOMPARE(surface.trustedTitleForTesting(title),
             QStringLiteral("Restricted web"));
}

void WebSessionProfileTest::trustedTitlesRejectEncodedPhysicalAddresses()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface surface(session, server.url(QStringLiteral("/titles")));
    const QString fallback = QStringLiteral("Restricted web");
    const QStringList rejected{
        QStringLiteral("http://127.0.0.1:43123/help"),
        QStringLiteral("127.0.0.2"),
        QStringLiteral("localhost:43123/help"),
        QStringLiteral("[::1]:43123/help"),
        QStringLiteral("::1"),
        QStringLiteral("http%3A%2F%2F127.0.0.1%3A43123%2Fhelp"),
        QStringLiteral("%5B%3A%3A1%5D%3A43123"),
        QStringLiteral("%2568%2574%2574%2570%253A%252F%252F127.0.0.1")};
    for (const QString &title : rejected) {
        QCOMPARE(surface.trustedTitleForTesting(title), fallback);
    }
    QCOMPARE(surface.trustedTitleForTesting(QStringLiteral("Release notes 127")),
             QStringLiteral("Release notes 127"));
    QCOMPARE(surface.trustedTitleForTesting(QStringLiteral("Local development guide")),
             QStringLiteral("Local development guide"));
}

void WebSessionProfileTest::mainFrameIsPinnedToItsRegisteredEntry()
{
    HttpServer server([](const QByteArray &target, int) {
        if (target == QByteArrayLiteral("/allowed.js")) {
            return std::optional<QByteArray>(
                QByteArrayLiteral("document.body.dataset.resource='loaded';"));
        }
        if (target == QByteArrayLiteral("/entry")) {
            return std::optional<QByteArray>(QByteArrayLiteral(
                "<!doctype html><title>entry</title><body>"
                "<script src='/allowed.js'></script></body>"));
        }
        return std::optional<QByteArray>(
            QByteArrayLiteral("<!doctype html><title>wrong</title>"));
    });
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    const QUrl entry = server.url(QStringLiteral("/entry"));
    WebSurface surface(session, entry);

    QVERIFY(navigateAndWait(surface, entry));
    const auto resource = evaluateJavaScript(
        surface.page(), QStringLiteral("document.body.dataset.resource"));
    QVERIFY(resource.has_value());
    QCOMPARE(*resource, QJsonValue(QStringLiteral("loaded")));
    QVERIFY(server.requests().contains(QByteArrayLiteral("/allowed.js")));

    surface.page()->setUrl(server.url(QStringLiteral("/same-origin-but-unregistered")));
    QTest::qWait(500);
    QCOMPARE(surface.currentUrl(), entry);
    QVERIFY(!server.requests().contains(
        QByteArrayLiteral("/same-origin-but-unregistered")));

    surface.page()->setUrl(WebSurface::trustedErrorUrl());
    QTRY_VERIFY_WITH_TIMEOUT(
        surface.currentUrl() == WebSurface::trustedErrorUrl()
            && !surface.page()->isLoading(),
        10000);
}

void WebSessionProfileTest::redirectedMainFrameMustRemainTheRegisteredEntry()
{
    HttpServer server([](const QByteArray &target, int)
                          -> std::optional<HttpResponse> {
        if (target == QByteArrayLiteral("/redirect")) {
            HttpResponse response;
            response.status = QByteArrayLiteral("302 Found");
            response.headers.append(
                {QByteArrayLiteral("Location"), QByteArrayLiteral("/unregistered")});
            return response;
        }
        return HttpResponse{QByteArrayLiteral(
            "<!doctype html><title>Unregistered target</title>")};
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/redirect"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(surface.navigate(entry));
    QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty(), 10000);
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());
    QVERIFY(!server.requests().contains(QByteArrayLiteral("/unregistered")));
    bool sawTrustedTerminal = false;
    for (const QList<QVariant> &arguments : finishedSpy) {
        sawTrustedTerminal = sawTrustedTerminal
            || arguments.at(0).toUrl() == WebSurface::trustedErrorUrl();
    }
    QVERIFY(sawTrustedTerminal);
}

void WebSessionProfileTest::surfaceSignalsAndTrustedTitlesAreIndependent()
{
    HttpServer server([](const QByteArray &target, int) {
        if (target == QByteArrayLiteral("/named")) {
            return std::optional<QByteArray>(
                QByteArrayLiteral("<!doctype html><title>First title</title>"));
        }
        if (target == QByteArrayLiteral("/blank")) {
            return std::optional<QByteArray>(
                QByteArrayLiteral("<!doctype html><title>   </title>"));
        }
        if (target == QByteArrayLiteral("/missing")) {
            return std::optional<QByteArray>(
                QByteArrayLiteral("<!doctype html><body>missing title</body>"));
        }
        return std::optional<QByteArray>(QByteArrayLiteral(
            "<!doctype html><script>document.title=location.href</script>"));
    });
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface named(session, server.url(QStringLiteral("/named")));
    WebSurface blank(session, server.url(QStringLiteral("/blank")));
    WebSurface missing(session, server.url(QStringLiteral("/missing")));
    WebSurface urlTitle(session, server.url(QStringLiteral("/url-title")));
    QSignalSpy namedTitleSpy(&named, &WebSurface::titleChanged);
    QSignalSpy namedLoadingSpy(&named, &WebSurface::loadingChanged);
    QSignalSpy namedProgressSpy(&named, &WebSurface::loadProgressChanged);
    QSignalSpy blankTitleSpy(&blank, &WebSurface::titleChanged);
    QSignalSpy blankLoadingSpy(&blank, &WebSurface::loadingChanged);
    QSignalSpy namedRendererSpy(&named, &WebSurface::rendererFailed);
    QSignalSpy blankRendererSpy(&blank, &WebSurface::rendererFailed);

    QVERIFY(navigateAndWait(named, server.url(QStringLiteral("/named"))));
    QCOMPARE(named.title(), QStringLiteral("First title"));
    QVERIFY(!namedTitleSpy.isEmpty());
    QVERIFY(!namedLoadingSpy.isEmpty());
    QVERIFY(!namedProgressSpy.isEmpty());
    QVERIFY(blankTitleSpy.isEmpty());
    QVERIFY(blankLoadingSpy.isEmpty());

    QVERIFY(navigateAndWait(blank, server.url(QStringLiteral("/blank"))));
    QCOMPARE(blank.title(), QStringLiteral("Restricted web"));
    QVERIFY(!blankLoadingSpy.isEmpty());
    QCOMPARE(named.title(), QStringLiteral("First title"));

    QVERIFY(navigateAndWait(missing, server.url(QStringLiteral("/missing"))));
    QCOMPARE(missing.title(), QStringLiteral("Restricted web"));
    QVERIFY(!missing.title().contains(QStringLiteral("127.0.0.1")));

    QVERIFY(navigateAndWait(urlTitle, server.url(QStringLiteral("/url-title"))));
    QCOMPARE(urlTitle.title(), QStringLiteral("Restricted web"));
    QVERIFY(!urlTitle.title().contains(QStringLiteral("127.0.0.1")));

    QVERIFY(QMetaObject::invokeMethod(
        named.page(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 42)));
    QCOMPARE(namedRendererSpy.count(), 1);
    QCOMPARE(blankRendererSpy.count(), 0);
}

void WebSessionProfileTest::stopDoesNotLoadTheTrustedErrorPage()
{
    HttpServer server([](const QByteArray &, int) -> std::optional<QByteArray> {
        return std::nullopt;
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/slow"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy loadingInfoSpy(surface.page(), &QWebEnginePage::loadingChanged);

    QVERIFY(surface.navigate(entry));
    QTRY_VERIFY_WITH_TIMEOUT(surface.isLoading(), 5000);
    surface.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!surface.page()->isLoading(), 5000);
    QVERIFY(surface.currentUrl() != WebSurface::trustedErrorUrl());
    QTRY_VERIFY_WITH_TIMEOUT(!surface.isLoading(), 5000);
    bool sawStoppedStatus = false;
    for (const QList<QVariant> &arguments : loadingInfoSpy) {
        sawStoppedStatus = sawStoppedStatus
            || arguments.at(0).value<QWebEngineLoadingInfo>().status()
                == QWebEngineLoadingInfo::LoadStoppedStatus;
    }
    QVERIFY(sawStoppedStatus);
    server.abortHeldResponses();
    QTest::qWait(100);
    QVERIFY(surface.currentUrl() != WebSurface::trustedErrorUrl());
}

void WebSessionProfileTest::aNewNavigationIncarnationIgnoresTheStoppedLoad()
{
    HttpServer server([](const QByteArray &, int) -> std::optional<QByteArray> {
        return std::nullopt;
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/incarnation"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    QSignalSpy finishedSpy(&surface, &WebSurface::navigationFinished);

    QVERIFY(surface.navigate(entry));
    QTRY_VERIFY_WITH_TIMEOUT(surface.isLoading(), 5000);
    QVERIFY(!surface.navigate(QUrl(QStringLiteral("https://example.com/rejected"))));
    QTRY_COMPARE_WITH_TIMEOUT(surface.currentUrl(), WebSurface::trustedErrorUrl(),
                              10000);
    QTRY_VERIFY_WITH_TIMEOUT(!surface.page()->isLoading(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty(), 10000);
    server.abortHeldResponses();
    QTest::qWait(250);
    QCOMPARE(surface.currentUrl(), WebSurface::trustedErrorUrl());
    bool trustedErrorSucceeded = false;
    for (const QList<QVariant> &arguments : finishedSpy) {
        trustedErrorSucceeded = trustedErrorSucceeded
            || (arguments.at(0).toUrl() == WebSurface::trustedErrorUrl()
                && arguments.at(1).toBool());
    }
    QVERIFY(trustedErrorSucceeded);
}

void WebSessionProfileTest::backgroundLifecycleWaitsUntilFreezeIsRecommended()
{
    HttpServer server([](const QByteArray &target, int)
                          -> std::optional<QByteArray> {
        if (target == QByteArrayLiteral("/slow-image")) return std::nullopt;
        return QByteArrayLiteral(
            "<!doctype html><title>lifecycle</title>"
            "<body data-page='preserved'><script>"
            "sessionStorage.setItem('state','preserved')</script>"
            "<img src='/slow-image'>");
    });
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/lifecycle"));
    WebSessionProfile session(server.origin());
    WebSurface surface(session, entry);
    surface.resize(400, 300);
    surface.move(-10000, -10000);
    surface.show();
    surface.setTabActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(surface.page()->isVisible(), 5000);
    QCOMPARE(surface.page()->lifecycleState(),
             QWebEnginePage::LifecycleState::Active);

    QVERIFY(surface.navigate(entry));
    QTRY_VERIFY_WITH_TIMEOUT(surface.page()->isLoading(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        evaluateJavaScript(surface.page(), QStringLiteral(
                                                "document.body ? "
                                                "document.body.dataset.page : ''"))
                .value_or(QJsonValue())
            == QJsonValue(QStringLiteral("preserved")),
        5000);

    bool frozeWhileVisible = false;
    bool becameVisibleBeforeActive = false;
    bool usedDiscarded = false;
    connect(surface.page(), &QWebEnginePage::lifecycleStateChanged, &surface,
            [&](const QWebEnginePage::LifecycleState state) {
                frozeWhileVisible = frozeWhileVisible
                    || (state == QWebEnginePage::LifecycleState::Frozen
                        && surface.page()->isVisible());
                usedDiscarded = usedDiscarded
                    || state == QWebEnginePage::LifecycleState::Discarded;
            });
    connect(surface.page(), &QWebEnginePage::visibleChanged, &surface,
            [&](const bool visible) {
                becameVisibleBeforeActive = becameVisibleBeforeActive
                    || (visible && surface.page()->lifecycleState()
                            != QWebEnginePage::LifecycleState::Active);
            });

    surface.setTabActive(false);
    QVERIFY(!surface.page()->isVisible());
    if (surface.page()->recommendedState()
        == QWebEnginePage::LifecycleState::Active) {
        QCOMPARE(surface.page()->lifecycleState(),
                 QWebEnginePage::LifecycleState::Active);
    }
    server.releaseHeldResponses();
    QTRY_COMPARE_WITH_TIMEOUT(surface.page()->lifecycleState(),
                              QWebEnginePage::LifecycleState::Frozen, 10000);
    QVERIFY(!frozeWhileVisible);
    QVERIFY(!usedDiscarded);

    surface.setTabActive(true);
    QCOMPARE(surface.page()->lifecycleState(),
             QWebEnginePage::LifecycleState::Active);
    QVERIFY(surface.page()->isVisible());
    QVERIFY(!becameVisibleBeforeActive);
    const auto state = evaluateJavaScript(
        surface.page(), QStringLiteral(
                            "document.body.dataset.page+'|'"
                            "+sessionStorage.getItem('state')"));
    QVERIFY(state.has_value());
    QCOMPARE(*state, QJsonValue(QStringLiteral("preserved|preserved")));
}

void WebSessionProfileTest::sessionRetirementSynchronouslyShutsDownItsSurfaces()
{
    HttpServer server;
    QVERIFY(server.listen());
    auto session = std::make_unique<WebSessionProfile>(server.origin());
    auto surface = std::make_unique<WebSurface>(
        *session, server.url(QStringLiteral("/retirement")));
    QVERIFY(surface->isConfigurationValid());
    QCOMPARE(session->registeredPageCount(), 1);

    session.reset();

    QVERIFY(!surface->isConfigurationValid());
    QVERIFY(surface->page() == nullptr);
    QVERIFY(surface->view() == nullptr);
    QVERIFY(surface->shutdown());
    surface.reset();
}

void WebSessionProfileTest::unregisterDisconnectsThePageDestructionObserver()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    auto page = std::make_unique<ObservableWebPage>(session.profile());
    const int baselineReceivers = page->destroyedReceiverCount();
    QSignalSpy countSpy(&session, &WebSessionProfile::registeredPageCountChanged);

    for (int cycle = 0; cycle < 4; ++cycle) {
        QVERIFY(session.registerPage(page.get()));
        QCOMPARE(page->destroyedReceiverCount(), baselineReceivers + 1);
        QVERIFY(session.unregisterPage(page.get()));
        QCOMPARE(page->destroyedReceiverCount(), baselineReceivers);
    }
    QCOMPARE(countSpy.count(), 8);
    page.reset();
    QCOMPARE(countSpy.count(), 8);
}

void WebSessionProfileTest::shutdownFailsUntilEveryPageIsUnregistered()
{
    HttpServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    auto surface = std::make_unique<WebSurface>(
        session, server.url(QStringLiteral("/entry")));
    QCOMPARE(session.registeredPageCount(), 1);

    QVERIFY(!session.shutdown());
    QVERIFY(session.profile() != nullptr);
    QVERIFY(surface->shutdown());
    surface.reset();
    QCOMPARE(session.registeredPageCount(), 0);
    QVERIFY(session.shutdown());
    QVERIFY(session.profile() == nullptr);
    QVERIFY(session.requestInterceptor() == nullptr);
}

void WebSessionProfileTest::mainWindowShutdownCanBeRetriedAfterForeignSurfaceRetires()
{
    HttpServer server;
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/help"));
    MainWindow window(webRoutes(entry), server.origin());
    QVERIFY(window.isRunning());
    WebSessionProfile *const session = window.webSessionProfile();
    QVERIFY(session != nullptr);
    auto extraSurface = std::make_unique<WebSurface>(
        *session, server.url(QStringLiteral("/extra")));
    QVERIFY(extraSurface->isConfigurationValid());
    QCOMPARE(session->registeredPageCount(), 2);

    QVERIFY(!window.shutdown());
    QVERIFY(!window.isRunning());
    QVERIFY(!window.isShutdownComplete());
    QVERIFY(window.webSurface() == nullptr);
    QCOMPARE(session->registeredPageCount(), 1);
    QVERIFY(!window.shutdown());

    QVERIFY(extraSurface->shutdown());
    extraSurface.reset();
    QCOMPARE(session->registeredPageCount(), 0);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
    QVERIFY(window.webSessionProfile() == nullptr);
    QVERIFY(window.shutdown());
}

void WebSessionProfileTest::reentrantZeroCountShutdownRetiresThePageBeforeTheProfile()
{
    HttpServer server;
    QVERIFY(server.listen());
    MainWindow window(webRoutes(server.url(QStringLiteral("/help"))),
                      server.origin());
    WebSessionProfile *const session = window.webSessionProfile();
    QVERIFY(session != nullptr);
    QVERIFY(window.webSurface() != nullptr);
    QPointer<QWebEnginePage> page(window.webSurface()->page());
    QPointer<QWebEngineProfile> profile(session->profile());
    QVERIFY(!page.isNull());
    QVERIFY(!profile.isNull());

    QStringList destructionOrder;
    bool zeroCountObserved = false;
    bool pageWasGoneAtZero = false;
    bool profileWasAliveAtZero = false;
    bool profileSurvivedRecursiveShutdown = false;
    std::optional<bool> recursiveShutdownResult;
    connect(page.data(), &QObject::destroyed, &window,
            [&destructionOrder] { destructionOrder.append(QStringLiteral("page")); });
    connect(profile.data(), &QObject::destroyed, &window,
            [&destructionOrder] {
                destructionOrder.append(QStringLiteral("profile"));
            });
    connect(session, &WebSessionProfile::registeredPageCountChanged, &window,
            [&](const qsizetype count) {
                if (count != 0) return;
                zeroCountObserved = true;
                destructionOrder.append(QStringLiteral("zero"));
                pageWasGoneAtZero = page.isNull();
                profileWasAliveAtZero = !profile.isNull();
                recursiveShutdownResult = window.shutdown();
                profileSurvivedRecursiveShutdown = !profile.isNull();
            },
            Qt::DirectConnection);

    QVERIFY(window.shutdown());

    QVERIFY(zeroCountObserved);
    QVERIFY(recursiveShutdownResult.has_value());
    QVERIFY(!*recursiveShutdownResult);
    QVERIFY(pageWasGoneAtZero);
    QVERIFY(profileWasAliveAtZero);
    QVERIFY(profileSurvivedRecursiveShutdown);
    QVERIFY(page.isNull());
    QVERIFY(profile.isNull());
    QCOMPARE(destructionOrder,
             QStringList({QStringLiteral("page"), QStringLiteral("zero"),
                          QStringLiteral("profile")}));
    QVERIFY(window.isShutdownComplete());
    QVERIFY(window.shutdown());
}

void WebSessionProfileTest::mainWindowRejectsCommandsAfterShutdown()
{
    HttpServer server;
    QVERIFY(server.listen());
    const QUrl entry = server.url(QStringLiteral("/help"));
    MainWindow window(webRoutes(entry), server.origin());
    QVERIFY(window.navigate(QStringLiteral("app://pilot/first")));
    QVERIFY(window.navigate(QStringLiteral("app://pilot/second")));
    const QString currentUrl = window.currentAppUrl();
    const int historyCount = window.historyCount();
    const int historyIndex = window.historyIndex();
    QSignalSpy currentUrlSpy(&window, &MainWindow::currentUrlChanged);

    QVERIFY(window.shutdown());
    QVERIFY(!window.isRunning());
    QVERIFY(!window.navigate(QStringLiteral("app://pilot/first")));
    QVERIFY(!window.navigateFromWorker(QStringLiteral("com.qbrowser.web"),
                                       QStringLiteral("/first")));
    QVERIFY(!window.goBack());
    QVERIFY(!window.goForward());
    QVERIFY(!window.attachWorkerSurface({}));
    window.detachWorkerSurface();
    QVERIFY(QMetaObject::invokeMethod(
        window.navigationBar(), "navigateRequested", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("app://pilot/first"))));

    QCOMPARE(window.currentAppUrl(), currentUrl);
    QCOMPARE(window.historyCount(), historyCount);
    QCOMPARE(window.historyIndex(), historyIndex);
    QVERIFY(currentUrlSpy.isEmpty());
}

void WebSessionProfileTest::hostApplicationDoesNotRestartARetiredWindow()
{
    HttpServer server;
    QVERIFY(server.listen());
    HostApplication host(server.origin());
    QVERIFY(host.start());
    MainWindow *const window = host.mainWindow();
    QVERIFY(window != nullptr);
    QVERIFY(window->shutdown());
    QVERIFY(window->isShutdownComplete());
    QVERIFY(!host.start());
    QVERIFY(!window->isVisible());
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-web-session-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    WebSessionProfileTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_web_session_profile.moc"
