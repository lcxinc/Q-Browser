#include "BrowserChrome.h"
#include "BrowserSessionStore.h"
#include "BrowserTabModel.h"
#include "MainWindow.h"
#include "NavigationBar.h"
#include "NewTabPage.h"
#include "RouteRegistry.h"
#include "TabController.h"
#include "WebSessionProfile.h"
#include "WebSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <memory>
#include <optional>
#include <stdexcept>

namespace {

class BrowserServer final : public QObject
{
public:
    explicit BrowserServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                QTcpSocket *const socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket] { handleRequest(socket); });
            }
        });
    }

    ~BrowserServer() override
    {
        for (const QPointer<QTcpSocket> &socket : heldSockets_) {
            if (socket) socket->abort();
        }
    }

    [[nodiscard]] bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    [[nodiscard]] QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/")
                        .arg(server_.serverPort()));
    }

    [[nodiscard]] QUrl url(const QString &path) const
    {
        return origin().resolved(QUrl(path));
    }

    [[nodiscard]] int requestCount(const QByteArray &target) const
    {
        return requestCounts_.value(target);
    }

private:
    void handleRequest(QTcpSocket *socket)
    {
        if (socket == nullptr || socket->property("handled").toBool()) return;
        QByteArray request = socket->property("request").toByteArray();
        request += socket->readAll();
        socket->setProperty("request", request);
        const qsizetype headerEnd = request.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        socket->setProperty("handled", true);

        const qsizetype lineEnd = request.indexOf("\r\n");
        if (lineEnd < 0) {
            socket->abort();
            return;
        }
        const QList<QByteArray> requestLine = request.first(lineEnd).split(' ');
        if (requestLine.size() < 2) {
            socket->abort();
            return;
        }
        const QByteArray target = requestLine.at(1);
        ++requestCounts_[target];
        if (target == QByteArrayLiteral("/slow")) {
            heldSockets_.append(socket);
            return;
        }

        QByteArray title = QByteArrayLiteral("Unknown");
        if (target == QByteArrayLiteral("/alpha")) {
            title = QByteArrayLiteral("Alpha page");
        } else if (target == QByteArrayLiteral("/beta")) {
            title = QByteArrayLiteral("Beta page");
        }
        const QByteArray body = QByteArrayLiteral("<!doctype html><title>")
            + title
            + QByteArrayLiteral("</title><main>Q-Browser test page</main>");
        const QByteArray response = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QHash<QByteArray, int> requestCounts_;
    QList<QPointer<QTcpSocket>> heldSockets_;
};

RouteRegistry browserRoutes(const BrowserServer &server)
{
    RouteRegistry registry;
    const QList<RouteRecord> records{
        {QStringLiteral("/web/alpha"), Engine::WebEngine,
         QStringLiteral("com.qbrowser.web"),
         server.url(QStringLiteral("alpha")).toString(QUrl::FullyEncoded)},
        {QStringLiteral("/web/beta"), Engine::WebEngine,
         QStringLiteral("com.qbrowser.web"),
         server.url(QStringLiteral("beta")).toString(QUrl::FullyEncoded)},
        {QStringLiteral("/web/slow"), Engine::WebEngine,
         QStringLiteral("com.qbrowser.web"),
         server.url(QStringLiteral("slow")).toString(QUrl::FullyEncoded)},
        {QStringLiteral("/orders"), Engine::QmlWorker,
         QStringLiteral("com.qbrowser.pilot"),
         QStringLiteral("qml/Main.qml")},
    };
    for (const RouteRecord &record : records) {
        if (registry.add(record) != RouteAddResult::Added) return {};
    }
    return registry;
}

bool waitForWebLoad(WebSurface *surface,
                    const QUrl &expectedUrl,
                    const QString &expectedTitle)
{
    if (surface == nullptr || surface->page() == nullptr) return false;
    const auto complete = [surface, &expectedUrl, &expectedTitle] {
        return surface->currentUrl() == expectedUrl
            && !surface->page()->isLoading()
            && surface->title() == expectedTitle;
    };
    if (complete()) return true;

    QSignalSpy finished(surface, &WebSurface::navigationFinished);
    QElapsedTimer elapsed;
    elapsed.start();
    constexpr int timeoutMs = 10'000;
    while (elapsed.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (!finished.wait(remaining)) return complete();
        if (complete()) return true;
    }
    return complete();
}

std::optional<QVariant> evaluateJavaScript(QWebEnginePage *page,
                                           const QString &source)
{
    if (page == nullptr) return std::nullopt;
    struct Result final {
        QVariant value;
        bool completed = false;
    };
    const auto result = std::make_shared<Result>();
    QEventLoop loop;
    const QPointer<QEventLoop> guardedLoop(&loop);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->runJavaScript(source, [result, guardedLoop](const QVariant &value) {
        result->value = value;
        result->completed = true;
        if (guardedLoop) guardedLoop->quit();
    });
    timeout.start(5'000);
    loop.exec();
    return result->completed ? std::optional<QVariant>(result->value)
                             : std::nullopt;
}

void activateTab(MainWindow &window, const QString &id)
{
    const int index = window.tabModel()->indexOfId(id);
    QVERIFY(index >= 0);
    window.browserChrome()->tabBar()->setCurrentIndex(index);
    QTRY_COMPARE_WITH_TIMEOUT(window.tabModel()->activeId(), id, 2'000);
}

} // namespace

class BrowserShellTest final : public QObject
{
    Q_OBJECT

private slots:
    void deferredSessionShellStartsHiddenEmptyAndResourceFree();
    void loadedSessionAppliesAtomicallyWithoutSaveAndStartsOnlyActive();
    void missingSessionCreatesExactlyOneCleanNewTabWithoutInitializationSave();
    void corruptSessionCreatesExactlyOneCleanNewTabWithoutInitializationSave();
    void ioFailureSessionCreatesExactlyOneInMemoryNewTab();
    void loadedSessionWithoutSnapshotFallsBackToOneInMemoryNewTab();
    void invalidResolverResultFallsBackOnceWithoutSavingUnknownState();
    void throwingResolverFallsBackWithoutEscapingOrSaving();
    void resolverAndShellKindMismatchFallsBackWithoutLaunchingOrSaving();
    void reentrantResolverCannotInterruptAtomicSessionApply();
    void loadedUntrustedDescriptorBecomesResourceFreeTrustedError();
    void loadedActiveNewTabCreatesZeroWorkerAndWebPages();
    void loadedActiveAppStartsExactlyOnceAfterQueuedActivation();
    void persistenceNeededUsesOneSingleShotTimerAndCoalescesOneSnapshotSave();
    void normalShutdownStopsDebounceAndPerformsOneFinalSave();
    void explicitFinalFlushFreezesOnceAndNeverRestartsDebounce();
    void emptySaveCallbackLeavesPersistenceDisabled();
    void throwingFinalSaveCannotEscapeOrBlockShutdown();
    void throwingDebouncedSaveIsContainedBeforeOneFinalAttempt();
    void reentrantDebouncedSaveCannotStartNestedFinalFlush();
    void reentrantFinalSaveCannotReportSuccessOrShutdownDuringSave();
    void failedFinalSaveReturnsFalseButShutdownStillCleansResources();
    void freezeBeforeApplyIsTerminalAndRejectsLateSessionState();
    void immediateShellRejectsLateSessionApplicationWithoutChangingModel();
    void startupHasOneTrustedHostTabWithoutWorkerOrWebPage();
    void tabCommandsUseStableIdsAndReplaceTheLastClosedTab();
    void tabMutationsRejectDirectReentrantCommands();
    void webTitleCallbackRejectsReentrantClose();
    void webLoadingCallbackRejectsReentrantShutdown();
    void rejectedWebCallbackUiMutationsRestoreChrome();
    void webTabsOwnDistinctPagesOverOneLongLivedProfile();
    void switchingPreservesWebPageTitleLoadingAndState();
    void historiesAreIndependentAndInvalidInputDoesNotCommit();
    void inactiveRestoredDescriptorsRemainResourceFree();
    void lazyActivationRejectsReentrantCloseAndShutdown();
    void closeReachesRetiredBeforeTheModelEntryIsRemoved();
    void closeRejectsReentrantShutdown();
    void navigationPublishesCommittedStateBeforeOnePersistenceSignal();
    void navigationCommandsAffectOnlyTheActiveStableTab();
    void rendererFailureRetiresOnlyTheFailedSurfaceAndReloadRecreatesIt();
    void reloadBeforeQueuedRendererCleanupCannotReuseFailedSurface();
    void lateRendererFailureCannotRetireFreshIncarnation();
    void rendererFailureAtPageLimitReleasesSlotBeforeReload();
    void windowShutdownBeforeQueuedRendererCleanupCancelsIt();
};

void BrowserShellTest::deferredSessionShellStartsHiddenEmptyAndResourceFree()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);

    QVERIFY(!window.isVisible());
    QCOMPARE(window.tabModel()->count(), 0);
    QCOMPARE(window.tabModel()->activeIndex(), -1);
    QVERIFY(window.tabModel()->activeId().isEmpty());
    QCOMPARE(window.surfaceStack()->count(), 0);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QVERIFY(window.findChildren<QWebEngineView *>().isEmpty());
    QCOMPARE(window.workerSurface(), nullptr);
    QCOMPARE(window.webSurface(), nullptr);
}

void BrowserShellTest::loadedSessionAppliesAtomicallyWithoutSaveAndStartsOnlyActive()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    BrowserTabModel *const model = window.tabModel();
    QSignalSpy persistenceSpy(model, &BrowserTabModel::persistenceNeeded);
    int saveCount = 0;

    const QVector<BrowserTabSnapshot> rawTabs{
        {QStringLiteral("11111111111111111111111111111111"),
         BrowserTabKind::Host, QStringLiteral("Orders"),
         QStringLiteral("app://pilot/orders"),
         {QStringLiteral("qbrowser://newtab"),
          QStringLiteral("app://pilot/orders")}, 1},
        {QStringLiteral("22222222222222222222222222222222"),
         BrowserTabKind::Host, QStringLiteral("Alpha"),
         QStringLiteral("app://pilot/web/alpha"),
         {QStringLiteral("qbrowser://newtab"),
          QStringLiteral("app://pilot/web/alpha")}, 1},
        {QStringLiteral("33333333333333333333333333333333"),
         BrowserTabKind::Web, QStringLiteral("New tab"),
         QStringLiteral("qbrowser://newtab"),
         {QStringLiteral("qbrowser://newtab")}, 0},
    };
    const QRect persistedGeometry(120, 80, 900, 650);
    const BrowserSessionLoadResult loaded{
        BrowserSessionLoadStatus::Loaded,
        BrowserWindowSnapshot{
            persistedGeometry, rawTabs.at(1).id, rawTabs},
        {}};
    const RestoredAddressResolver resolver = [](const BrowserAddress &address)
        -> std::optional<BrowserTabKind> {
        if (address.kind() == BrowserAddressKind::NewTab) {
            return BrowserTabKind::Host;
        }
        if (address.kind() != BrowserAddressKind::App) return std::nullopt;
        if (address.appPath() == QStringLiteral("/orders")) {
            return BrowserTabKind::App;
        }
        if (address.appPath() == QStringLiteral("/web/alpha")) {
            return BrowserTabKind::Web;
        }
        return std::nullopt;
    };

    QVERIFY(window.applyBrowserSessionLoadResult(
        loaded, resolver, QRect(0, 0, 1920, 1080),
        {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    QCOMPARE(window.geometry(), persistedGeometry);
    QCOMPARE(model->count(), 3);
    QCOMPARE(model->activeId(), rawTabs.at(1).id);
    QCOMPARE(model->activeIndex(), 1);
    QCOMPARE(model->snapshotAt(0).id, rawTabs.at(0).id);
    QCOMPARE(model->snapshotAt(0).kind, BrowserTabKind::App);
    QCOMPARE(model->snapshotAt(0).history, rawTabs.at(0).history);
    QCOMPARE(model->snapshotAt(0).historyIndex, 1);
    QCOMPARE(model->snapshotAt(1).id, rawTabs.at(1).id);
    QCOMPARE(model->snapshotAt(1).kind, BrowserTabKind::Web);
    QCOMPARE(model->snapshotAt(1).history, rawTabs.at(1).history);
    QCOMPARE(model->snapshotAt(1).historyIndex, 1);
    QCOMPARE(model->snapshotAt(2).id, rawTabs.at(2).id);
    QCOMPARE(model->snapshotAt(2).kind, BrowserTabKind::Host);

    TabController *const app = window.tabController(rawTabs.at(0).id);
    TabController *const activeWeb = window.tabController(rawTabs.at(1).id);
    TabController *const host = window.tabController(rawTabs.at(2).id);
    QVERIFY(app != nullptr);
    QVERIFY(activeWeb != nullptr);
    QVERIFY(host != nullptr);
    QCOMPARE(app->lifecycle(), BrowserTabLifecycle::Dormant);
    QCOMPARE(app->hostSurface(), nullptr);
    QCOMPARE(app->webSurface(), nullptr);
    QCOMPARE(app->workerSurface(), nullptr);
    QVERIFY(activeWeb->lifecycle() != BrowserTabLifecycle::Dormant);
    QVERIFY(activeWeb->webSurface() != nullptr);
    QCOMPARE(host->lifecycle(), BrowserTabLifecycle::Dormant);
    QCOMPARE(host->hostSurface(), nullptr);
    QCOMPARE(host->webSurface(), nullptr);
    QCOMPARE(host->workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(1));
    QCOMPARE(window.surfaceStack()->count(), 1);
    QCOMPARE(persistenceSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::missingSessionCreatesExactlyOneCleanNewTabWithoutInitializationSave()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    BrowserTabModel *const model = window.tabModel();
    QSignalSpy persistenceSpy(model, &BrowserTabModel::persistenceNeeded);
    int saveCount = 0;
    bool resolverCalled = false;

    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}},
        [&](const BrowserAddress &) -> std::optional<BrowserTabKind> {
            resolverCalled = true;
            return std::nullopt;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    QCOMPARE(model->count(), 1);
    QCOMPARE(model->activeIndex(), 0);
    const BrowserTabSnapshot clean = model->snapshotAt(0);
    QVERIFY(!clean.id.isEmpty());
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.title, QStringLiteral("New tab"));
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(clean.history,
             QStringList({QStringLiteral("qbrowser://newtab")}));
    QCOMPARE(clean.historyIndex, 0);
    TabController *const controller = window.tabController(clean.id);
    QVERIFY(controller != nullptr);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::Active);
    QVERIFY(controller->hostSurface() != nullptr);
    QCOMPARE(controller->webSurface(), nullptr);
    QCOMPARE(controller->workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(window.surfaceStack()->count(), 1);
    QVERIFY(!resolverCalled);
    QCOMPARE(persistenceSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::corruptSessionCreatesExactlyOneCleanNewTabWithoutInitializationSave()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QSignalSpy persistenceSpy(
        window.tabModel(), &BrowserTabModel::persistenceNeeded);
    int saveCount = 0;

    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Corrupt, std::nullopt,
         QStringLiteral("host.session.corrupt")},
        {}, QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QCOMPARE(window.tabModel()->activeId(), clean.id);
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.title, QStringLiteral("New tab"));
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(clean.history,
             QStringList({QStringLiteral("qbrowser://newtab")}));
    QCOMPARE(clean.historyIndex, 0);
    QVERIFY(window.tabController(clean.id)->hostSurface() != nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(persistenceSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.tabModel()->setTitle(
        clean.id, QStringLiteral("Recovered clean session")));
    QTRY_COMPARE_WITH_TIMEOUT(saveCount, 1, 2'000);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 2);
}

void BrowserShellTest::ioFailureSessionCreatesExactlyOneInMemoryNewTab()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QSignalSpy persistenceSpy(
        window.tabModel(), &BrowserTabModel::persistenceNeeded);
    int saveCount = 0;

    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::IoFailure, std::nullopt,
         QStringLiteral("host.session.io_failed")},
        {}, QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QCOMPARE(window.tabModel()->activeId(), clean.id);
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.title, QStringLiteral("New tab"));
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(clean.history,
             QStringList({QStringLiteral("qbrowser://newtab")}));
    QCOMPARE(clean.historyIndex, 0);
    QVERIFY(window.tabController(clean.id)->hostSurface() != nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(persistenceSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.tabModel()->setTitle(
        clean.id, QStringLiteral("Memory only")));
    QTimer *const debounce = window.findChild<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(debounce != nullptr);
    QVERIFY(!debounce->isActive());
    QTest::qWait(350);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QVERIFY(window.shutdown());
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::loadedSessionWithoutSnapshotFallsBackToOneInMemoryNewTab()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;

    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Loaded, std::nullopt, {}},
        {}, QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QCOMPARE(window.tabModel()->activeId(), clean.id);
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(clean.history,
             QStringList({QStringLiteral("qbrowser://newtab")}));
    QCOMPARE(clean.historyIndex, 0);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(saveCount, 0);
    QVERIFY(window.tabModel()->setTitle(
        clean.id, QStringLiteral("Validation fallback")));
    QTest::qWait(350);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::invalidResolverResultFallsBackOnceWithoutSavingUnknownState()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    const BrowserSessionLoadResult loaded{
        BrowserSessionLoadStatus::Loaded,
        BrowserWindowSnapshot{
            QRect(10, 20, 800, 600),
            QStringLiteral("11111111111111111111111111111111"),
            {{QStringLiteral("11111111111111111111111111111111"),
              BrowserTabKind::App, QStringLiteral("Orders"),
              QStringLiteral("app://pilot/orders"),
              {QStringLiteral("app://pilot/orders")}, 0}}},
        {}};
    const auto save = [&](const BrowserWindowSnapshot &) {
        ++saveCount;
        return BrowserSessionSaveResult{
            BrowserSessionSaveStatus::Saved, {}};
    };

    QVERIFY(window.applyBrowserSessionLoadResult(
        loaded,
        [](const BrowserAddress &) -> std::optional<BrowserTabKind> {
            return static_cast<BrowserTabKind>(99);
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)}, save));

    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QVERIFY(clean.id
            != QStringLiteral("11111111111111111111111111111111"));
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QVERIFY(!window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)}, save));
    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(window.tabModel()->activeId(), clean.id);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.tabModel()->setTitle(
        clean.id, QStringLiteral("Still memory only")));
    QTest::qWait(350);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::throwingResolverFallsBackWithoutEscapingOrSaving()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    const QString stableId =
        QStringLiteral("11111111111111111111111111111111");
    const BrowserSessionLoadResult loaded{
        BrowserSessionLoadStatus::Loaded,
        BrowserWindowSnapshot{
            QRect(10, 20, 800, 600), stableId,
            {{stableId, BrowserTabKind::App, QStringLiteral("Orders"),
              QStringLiteral("app://pilot/orders"),
              {QStringLiteral("app://pilot/orders")}, 0}}},
        {}};
    bool escaped = false;
    bool applied = false;
    try {
        applied = window.applyBrowserSessionLoadResult(
            loaded,
            [](const BrowserAddress &) -> std::optional<BrowserTabKind> {
                throw std::runtime_error("resolver failed");
            },
            QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
            [&](const BrowserWindowSnapshot &) {
                ++saveCount;
                return BrowserSessionSaveResult{
                    BrowserSessionSaveStatus::Saved, {}};
            });
    } catch (...) {
        escaped = true;
    }

    QVERIFY(!escaped);
    QVERIFY(applied);
    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QVERIFY(clean.id != stableId);
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::resolverAndShellKindMismatchFallsBackWithoutLaunchingOrSaving()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    window.setPackageRuntimeEnabled(true);
    QSignalSpy launchSpy(&window, &MainWindow::appLaunchRequested);
    int saveCount = 0;
    const QString restoredId =
        QStringLiteral("11111111111111111111111111111111");

    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Loaded,
         BrowserWindowSnapshot{
             QRect(10, 20, 800, 600), restoredId,
             {{restoredId, BrowserTabKind::App, QStringLiteral("Orders"),
               QStringLiteral("app://pilot/orders"),
               {QStringLiteral("app://pilot/orders")}, 0}}},
         {}},
        [](const BrowserAddress &) -> std::optional<BrowserTabKind> {
            return BrowserTabKind::Web;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCoreApplication::processEvents();

    QCOMPARE(window.tabModel()->count(), 1);
    const BrowserTabSnapshot clean = window.tabModel()->snapshotAt(0);
    QVERIFY(clean.id != restoredId);
    QCOMPARE(clean.kind, BrowserTabKind::Host);
    QCOMPARE(clean.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(launchSpy.count(), 0);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::reentrantResolverCannotInterruptAtomicSessionApply()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    bool nestedApplyResult = true;
    bool resolverFreezeResult = true;
    bool resolverShutdownResult = true;
    bool insertedFreezeResult = true;
    bool insertedShutdownResult = true;
    const QString restoredId =
        QStringLiteral("11111111111111111111111111111111");
    connect(window.tabModel(), &BrowserTabModel::tabInserted, &window,
            [&](int, const QString &id) {
                if (id != restoredId) return;
                insertedFreezeResult = window.freezeBrowserSessionAndFlush();
                insertedShutdownResult = window.shutdown();
            });

    const bool outerApplyResult = window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Loaded,
         BrowserWindowSnapshot{
             QRect(10, 20, 800, 600), restoredId,
             {{restoredId, BrowserTabKind::Host, QStringLiteral("Orders"),
               QStringLiteral("app://pilot/orders"),
               {QStringLiteral("app://pilot/orders")}, 0}}},
         {}},
        [&](const BrowserAddress &) -> std::optional<BrowserTabKind> {
            nestedApplyResult = window.applyBrowserSessionLoadResult(
                {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
                QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)}, {});
            resolverFreezeResult = window.freezeBrowserSessionAndFlush();
            resolverShutdownResult = window.shutdown();
            return BrowserTabKind::App;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        });

    QVERIFY(!nestedApplyResult);
    QVERIFY(outerApplyResult);
    QVERIFY(!resolverFreezeResult);
    QVERIFY(!resolverShutdownResult);
    QVERIFY(!insertedFreezeResult);
    QVERIFY(!insertedShutdownResult);
    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(window.tabModel()->activeId(), restoredId);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::loadedUntrustedDescriptorBecomesResourceFreeTrustedError()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    window.setPackageRuntimeEnabled(true);
    QSignalSpy launchSpy(
        &window, &MainWindow::appLaunchRequested);
    int saveCount = 0;
    const QString stableId =
        QStringLiteral("11111111111111111111111111111111");
    const BrowserSessionLoadResult loaded{
        BrowserSessionLoadStatus::Loaded,
        BrowserWindowSnapshot{
            QRect(10, 20, 800, 600), stableId,
            {{stableId, BrowserTabKind::App, QStringLiteral("Orders"),
              QStringLiteral("app://pilot/orders"),
              {QStringLiteral("app://pilot/orders")}, 0}}},
        {}};

    QVERIFY(window.applyBrowserSessionLoadResult(
        loaded,
        [](const BrowserAddress &) -> std::optional<BrowserTabKind> {
            return std::nullopt;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCoreApplication::processEvents();

    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(window.tabModel()->activeId(), stableId);
    QCOMPARE(window.tabModel()->snapshotAt(0).kind,
             BrowserTabKind::TrustedError);
    TabController *const controller = window.tabController(stableId);
    QVERIFY(controller != nullptr);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::TrustedError);
    QCOMPARE(controller->surfaceKind(), HostSurfaceKind::TrustedError);
    QCOMPARE(controller->hostSurface(), nullptr);
    QCOMPARE(controller->webSurface(), nullptr);
    QCOMPARE(controller->workerSurface(), nullptr);
    QVERIFY(!controller->trustedErrorText().isEmpty());
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(launchSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::loadedActiveNewTabCreatesZeroWorkerAndWebPages()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QSignalSpy launchSpy(&window, &MainWindow::appLaunchRequested);
    int saveCount = 0;
    const QString stableId =
        QStringLiteral("11111111111111111111111111111111");
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Loaded,
         BrowserWindowSnapshot{
             QRect(10, 20, 800, 600), stableId,
             {{stableId, BrowserTabKind::Web, QStringLiteral("New tab"),
               QStringLiteral("qbrowser://newtab"),
               {QStringLiteral("qbrowser://newtab")}, 0}}},
         {}},
        [](const BrowserAddress &address)
            -> std::optional<BrowserTabKind> {
            return address.kind() == BrowserAddressKind::NewTab
                ? std::optional<BrowserTabKind>(BrowserTabKind::Host)
                : std::nullopt;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCoreApplication::processEvents();

    TabController *const controller = window.tabController(stableId);
    QVERIFY(controller != nullptr);
    QCOMPARE(window.tabModel()->snapshotAt(0).kind, BrowserTabKind::Host);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::Active);
    QVERIFY(controller->hostSurface() != nullptr);
    QCOMPARE(controller->webSurface(), nullptr);
    QCOMPARE(controller->workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QVERIFY(window.findChildren<QWebEngineView *>().isEmpty());
    QCOMPARE(launchSpy.count(), 0);
    QCOMPARE(saveCount, 0);
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::loadedActiveAppStartsExactlyOnceAfterQueuedActivation()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    window.setPackageRuntimeEnabled(true);
    QSignalSpy launchSpy(&window, &MainWindow::appLaunchRequested);
    const QString activeId =
        QStringLiteral("11111111111111111111111111111111");
    const QString siblingId =
        QStringLiteral("22222222222222222222222222222222");
    const BrowserSessionLoadResult loaded{
        BrowserSessionLoadStatus::Loaded,
        BrowserWindowSnapshot{
            QRect(10, 20, 800, 600), activeId,
            {{activeId, BrowserTabKind::Host, QStringLiteral("Orders"),
              QStringLiteral("app://pilot/orders"),
              {QStringLiteral("app://pilot/orders")}, 0},
             {siblingId, BrowserTabKind::Host, QStringLiteral("Alpha"),
              QStringLiteral("app://pilot/web/alpha"),
              {QStringLiteral("app://pilot/web/alpha")}, 0}}},
        {}};

    QVERIFY(window.applyBrowserSessionLoadResult(
        loaded,
        [](const BrowserAddress &address)
            -> std::optional<BrowserTabKind> {
            if (address.appPath() == QStringLiteral("/orders")) {
                return BrowserTabKind::App;
            }
            if (address.appPath() == QStringLiteral("/web/alpha")) {
                return BrowserTabKind::Web;
            }
            return std::nullopt;
        },
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [](const BrowserWindowSnapshot &) {
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCoreApplication::processEvents();

    QCOMPARE(launchSpy.count(), 1);
    QCOMPARE(launchSpy.constFirst().at(0).toString(), activeId);
    TabController *const active = window.tabController(activeId);
    TabController *const sibling = window.tabController(siblingId);
    QVERIFY(active != nullptr);
    QVERIFY(sibling != nullptr);
    QCOMPARE(active->lifecycle(), BrowserTabLifecycle::Loading);
    QCOMPARE(active->hostSurface(), nullptr);
    QCOMPARE(active->webSurface(), nullptr);
    QCOMPARE(active->workerSurface(), nullptr);
    QCOMPARE(sibling->lifecycle(), BrowserTabLifecycle::Dormant);
    QCOMPARE(sibling->hostSurface(), nullptr);
    QCOMPARE(sibling->webSurface(), nullptr);
    QCOMPARE(sibling->workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(window.surfaceStack()->count(), 0);
}

void BrowserShellTest::persistenceNeededUsesOneSingleShotTimerAndCoalescesOneSnapshotSave()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QVector<BrowserWindowSnapshot> savedSnapshots;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &snapshot) {
            savedSnapshots.append(snapshot);
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));

    const QList<QTimer *> timers = window.findChildren<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QCOMPARE(timers.size(), 1);
    QVERIFY(timers.constFirst()->isSingleShot());
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(window.tabModel()->setTitle(activeId, QStringLiteral("First")));
    QVERIFY(window.tabModel()->setTitle(activeId, QStringLiteral("Second")));
    QVERIFY(window.tabModel()->setTitle(activeId, QStringLiteral("Final")));

    QTRY_COMPARE_WITH_TIMEOUT(savedSnapshots.size(), 1, 2'000);
    QTest::qWait(350);
    QCOMPARE(savedSnapshots.size(), 1);
    const BrowserWindowSnapshot saved = savedSnapshots.constFirst();
    QCOMPARE(saved.geometry, window.geometry());
    QCOMPARE(saved.activeTabId, window.tabModel()->activeId());
    QCOMPARE(saved.tabs, window.tabModel()->snapshots());
    QCOMPARE(saved.tabs.constFirst().title, QStringLiteral("Final"));
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(savedSnapshots.size(), 2);
}

void BrowserShellTest::normalShutdownStopsDebounceAndPerformsOneFinalSave()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QVector<BrowserWindowSnapshot> savedSnapshots;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &snapshot) {
            savedSnapshots.append(snapshot);
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("Frozen at shutdown")));
    QTimer *const debounce = window.findChild<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(debounce != nullptr);
    QVERIFY(debounce->isActive());

    const QRect expectedGeometry = window.geometry();
    const QString expectedActiveId = window.tabModel()->activeId();
    const QVector<BrowserTabSnapshot> expectedTabs =
        window.tabModel()->snapshots();
    QVERIFY(window.shutdown());

    QCOMPARE(savedSnapshots.size(), 1);
    QCOMPARE(savedSnapshots.constFirst().geometry, expectedGeometry);
    QCOMPARE(savedSnapshots.constFirst().activeTabId, expectedActiveId);
    QCOMPARE(savedSnapshots.constFirst().tabs, expectedTabs);
    QVERIFY(!debounce->isActive());
    QTest::qWait(350);
    QCOMPARE(savedSnapshots.size(), 1);
}

void BrowserShellTest::explicitFinalFlushFreezesOnceAndNeverRestartsDebounce()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QVector<BrowserWindowSnapshot> savedSnapshots;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &snapshot) {
            savedSnapshots.append(snapshot);
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("Frozen title")));
    QTimer *const debounce = window.findChild<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(debounce != nullptr);
    QVERIFY(debounce->isActive());

    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(savedSnapshots.size(), 1);
    QVERIFY(!debounce->isActive());
    QCOMPARE(savedSnapshots.constFirst().tabs.constFirst().title,
             QStringLiteral("Frozen title"));

    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("Changed after freeze")));
    QVERIFY(!debounce->isActive());
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(savedSnapshots.size(), 1);
    QTest::qWait(350);
    QCOMPARE(savedSnapshots.size(), 1);
    QVERIFY(window.shutdown());
    QCOMPARE(savedSnapshots.size(), 1);
}

void BrowserShellTest::emptySaveCallbackLeavesPersistenceDisabled()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)}, {}));
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("No persistence authority")));
    QTimer *const debounce = window.findChild<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(debounce != nullptr);
    QVERIFY(!debounce->isActive());
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QVERIFY(!debounce->isActive());
    QVERIFY(window.shutdown());
}

void BrowserShellTest::throwingFinalSaveCannotEscapeOrBlockShutdown()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &)
            -> BrowserSessionSaveResult {
            ++saveCount;
            throw std::runtime_error("save failed");
        }));
    const QString activeId = window.tabModel()->activeId();
    bool escaped = false;
    bool flushResult = true;
    try {
        flushResult = window.freezeBrowserSessionAndFlush();
    } catch (...) {
        escaped = true;
    }

    QVERIFY(!escaped);
    QVERIFY(!flushResult);
    QCOMPARE(saveCount, 1);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
    QCOMPARE(window.tabController(activeId), nullptr);
    QCOMPARE(window.webSessionProfile(), nullptr);
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::throwingDebouncedSaveIsContainedBeforeOneFinalAttempt()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &)
            -> BrowserSessionSaveResult {
            ++saveCount;
            throw std::runtime_error("save failed");
        }));
    QVERIFY(window.tabModel()->setTitle(
        window.tabModel()->activeId(), QStringLiteral("Schedule save")));

    QTRY_COMPARE_WITH_TIMEOUT(saveCount, 1, 2'000);
    QTimer *const debounce = window.findChild<QTimer *>(
        QStringLiteral("browser-session-save-debounce"),
        Qt::FindDirectChildrenOnly);
    QVERIFY(debounce != nullptr);
    QVERIFY(!debounce->isActive());
    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 2);
    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 2);
    QVERIFY(window.shutdown());
    QCOMPARE(saveCount, 2);
}

void BrowserShellTest::reentrantDebouncedSaveCannotStartNestedFinalFlush()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    bool nestedFreezeResult = true;
    bool nestedShutdownResult = true;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            if (saveCount == 1) {
                nestedFreezeResult = window.freezeBrowserSessionAndFlush();
                nestedShutdownResult = window.shutdown();
            }
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("First debounced save")));

    QTRY_COMPARE_WITH_TIMEOUT(saveCount, 1, 2'000);
    QVERIFY(!nestedFreezeResult);
    QVERIFY(!nestedShutdownResult);
    QVERIFY(window.isRunning());
    QVERIFY(window.tabModel()->setTitle(
        activeId, QStringLiteral("Second debounced save")));
    QTRY_COMPARE_WITH_TIMEOUT(saveCount, 2, 2'000);

    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 3);
    QVERIFY(window.shutdown());
    QCOMPARE(saveCount, 3);
}

void BrowserShellTest::reentrantFinalSaveCannotReportSuccessOrShutdownDuringSave()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    bool nestedFreezeResult = true;
    bool nestedShutdownResult = true;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            nestedFreezeResult = window.freezeBrowserSessionAndFlush();
            nestedShutdownResult = window.shutdown();
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::IoFailure,
                QStringLiteral("host.session.save_failed")};
        }));

    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
    QVERIFY(!nestedFreezeResult);
    QVERIFY(!nestedShutdownResult);
    QVERIFY(window.isRunning());
    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::failedFinalSaveReturnsFalseButShutdownStillCleansResources()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;
    QVERIFY(window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::IoFailure,
                QStringLiteral("host.session.save_failed")};
        }));
    const QString activeId = window.tabModel()->activeId();

    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
    QVERIFY(!window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 1);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
    QCOMPARE(window.tabController(activeId), nullptr);
    QCOMPARE(window.webSessionProfile(), nullptr);
    QCOMPARE(saveCount, 1);
}

void BrowserShellTest::freezeBeforeApplyIsTerminalAndRejectsLateSessionState()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(
        browserRoutes(server), server.origin(),
        MainWindowInitialState::DeferredSession);
    int saveCount = 0;

    QVERIFY(window.freezeBrowserSessionAndFlush());
    QVERIFY(!window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCOMPARE(window.tabModel()->count(), 0);
    QVERIFY(window.tabModel()->activeId().isEmpty());
    QVERIFY(window.freezeBrowserSessionAndFlush());
    QCOMPARE(saveCount, 0);
    QVERIFY(window.shutdown());
}

void BrowserShellTest::immediateShellRejectsLateSessionApplicationWithoutChangingModel()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    const QVector<BrowserTabSnapshot> before = window.tabModel()->snapshots();
    const QString activeBefore = window.tabModel()->activeId();
    int saveCount = 0;

    QVERIFY(!window.applyBrowserSessionLoadResult(
        {BrowserSessionLoadStatus::Missing, std::nullopt, {}}, {},
        QRect(0, 0, 1920, 1080), {QRect(0, 0, 1920, 1080)},
        [&](const BrowserWindowSnapshot &) {
            ++saveCount;
            return BrowserSessionSaveResult{
                BrowserSessionSaveStatus::Saved, {}};
        }));
    QCOMPARE(window.tabModel()->snapshots(), before);
    QCOMPARE(window.tabModel()->activeId(), activeBefore);
    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(saveCount, 0);
}

void BrowserShellTest::startupHasOneTrustedHostTabWithoutWorkerOrWebPage()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());

    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(window.tabModel()->activeIndex(), 0);
    const QString activeId = window.tabModel()->activeId();
    QVERIFY(!activeId.isEmpty());
    const BrowserTabSnapshot snapshot = window.tabModel()->snapshotAt(0);
    QCOMPARE(snapshot.id, activeId);
    QCOMPARE(snapshot.kind, BrowserTabKind::Host);
    QCOMPARE(snapshot.address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(snapshot.history,
             QStringList({QStringLiteral("qbrowser://newtab")}));
    QCOMPARE(snapshot.historyIndex, 0);

    TabController *const controller = window.tabController(activeId);
    QVERIFY(controller != nullptr);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::Active);
    QCOMPARE(controller->surfaceKind(), HostSurfaceKind::Host);
    QVERIFY(controller->hostSurface() != nullptr);
    QCOMPARE(controller->webSurface(), nullptr);
    QCOMPARE(controller->workerSurface(), nullptr);
    QCOMPARE(window.webSurface(), nullptr);
    QCOMPARE(window.workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(0));
    QCOMPARE(window.surfaceStack()->count(), 1);
    QCOMPARE(window.surfaceStack()->currentWidget(), controller->hostSurface());
    QVERIFY(window.findChildren<QWebEngineView *>().isEmpty());
}

void BrowserShellTest::tabCommandsUseStableIdsAndReplaceTheLastClosedTab()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    BrowserChrome *const chrome = window.browserChrome();
    const QString firstId = model->activeId();

    chrome->dispatchCommand(BrowserCommand::NewTab);
    QCOMPARE(model->count(), 2);
    const QString secondId = model->activeId();
    QVERIFY(!secondId.isEmpty());
    QVERIFY(secondId != firstId);
    QVERIFY(window.tabController(firstId) != nullptr);
    QVERIFY(window.tabController(secondId) != nullptr);

    chrome->tabBar()->moveTab(model->indexOfId(secondId), 0);
    QCOMPARE(model->indexOfId(secondId), 0);
    QCOMPARE(model->activeId(), secondId);

    activateTab(window, firstId);
    chrome->dispatchCommand(BrowserCommand::CloseTab);
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->activeId(), secondId);
    QCOMPARE(window.tabController(firstId), nullptr);
    QCOMPARE(model->recentlyClosedCount(), 1);

    chrome->dispatchCommand(BrowserCommand::ReopenClosedTab);
    QCOMPARE(model->count(), 2);
    const QString reopenedId = model->activeId();
    QVERIFY(!reopenedId.isEmpty());
    QVERIFY(reopenedId != firstId);
    QCOMPARE(model->snapshotAt(model->indexOfId(reopenedId)).address,
             QStringLiteral("qbrowser://newtab"));

    chrome->dispatchCommand(BrowserCommand::CloseTab);
    QCOMPARE(model->count(), 1);
    activateTab(window, secondId);

    QSignalSpy finalPersistence(model, &BrowserTabModel::persistenceNeeded);
    QSignalSpy finalActiveChange(model, &BrowserTabModel::activeTabChanged);
    QSignalSpy finalRemoval(model, &BrowserTabModel::tabRemoved);
    QSignalSpy finalInsertion(model, &BrowserTabModel::tabInserted);
    bool persistenceSawEmptyModel = false;
    bool removalSawCompleteControllerMap = false;
    bool insertionSawCompleteControllerMap = false;
    bool activationSawCompleteControllerMap = false;
    const auto controllerMapCoversModel = [&window, model] {
        for (const BrowserTabSnapshot &tab : model->snapshots()) {
            if (window.tabController(tab.id) == nullptr) return false;
        }
        return true;
    };
    connect(model, &BrowserTabModel::persistenceNeeded, this, [&] {
        persistenceSawEmptyModel = persistenceSawEmptyModel || model->isEmpty();
    }, Qt::DirectConnection);
    connect(model, &BrowserTabModel::tabRemoved, this,
            [&](int, const QString &removedId) {
                removalSawCompleteControllerMap =
                    window.tabController(removedId) == nullptr
                    && controllerMapCoversModel();
            }, Qt::DirectConnection);
    connect(model, &BrowserTabModel::tabInserted, this,
            [&](int, const QString &) {
                insertionSawCompleteControllerMap = controllerMapCoversModel();
            }, Qt::DirectConnection);
    connect(model, &BrowserTabModel::activeTabChanged, this,
            [&](int, int) {
                activationSawCompleteControllerMap = controllerMapCoversModel();
            }, Qt::DirectConnection);
    chrome->dispatchCommand(BrowserCommand::CloseTab);
    QCOMPARE(model->count(), 1);
    QVERIFY(model->activeId() != secondId);
    QCOMPARE(model->snapshotAt(0).kind, BrowserTabKind::Host);
    QCOMPARE(model->snapshotAt(0).address,
             QStringLiteral("qbrowser://newtab"));
    QVERIFY(window.tabController(secondId) == nullptr);
    QCOMPARE(finalRemoval.count(), 1);
    QCOMPARE(finalInsertion.count(), 1);
    QCOMPARE(finalActiveChange.count(), 1);
    QCOMPARE(finalActiveChange.first().at(0).toInt(), 0);
    QCOMPARE(finalActiveChange.first().at(1).toInt(), 0);
    QCOMPARE(finalPersistence.count(), 1);
    QVERIFY(!persistenceSawEmptyModel);
    QVERIFY(removalSawCompleteControllerMap);
    QVERIFY(insertionSawCompleteControllerMap);
    QVERIFY(activationSawCompleteControllerMap);
}

void BrowserShellTest::tabMutationsRejectDirectReentrantCommands()
{
    BrowserServer server;
    QVERIFY(server.listen());

    {
        MainWindow window(browserRoutes(server), server.origin());
        BrowserTabModel *const model = window.tabModel();
        QString insertedId;
        TabController *insertedController = nullptr;
        bool reentryAttempted = false;
        connect(model, &BrowserTabModel::tabInserted, this,
                [&](int, const QString &id) {
                    if (reentryAttempted) return;
                    reentryAttempted = true;
                    insertedId = id;
                    insertedController = window.tabController(id);
                    window.browserChrome()->dispatchCommand(
                        BrowserCommand::CloseTab);
                }, Qt::DirectConnection);

        window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
        QVERIFY(reentryAttempted);
        QVERIFY(!insertedId.isEmpty());
        QVERIFY(insertedController != nullptr);
        QCOMPARE(window.tabController(insertedId), insertedController);
        QVERIFY(insertedController->lifecycle()
                != BrowserTabLifecycle::Closing);
        QVERIFY(insertedController->lifecycle()
                != BrowserTabLifecycle::Retired);
    }

    {
        MainWindow window(browserRoutes(server), server.origin());
        BrowserTabModel *const model = window.tabModel();
        window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
        const QString activeId = model->activeId();
        TabController *const activeController = window.tabController(activeId);
        QVERIFY(activeController != nullptr);
        bool reentryAttempted = false;
        connect(model, &BrowserTabModel::tabMoved, this,
                [&](int, int) {
                    if (reentryAttempted) return;
                    reentryAttempted = true;
                    window.browserChrome()->dispatchCommand(
                        BrowserCommand::CloseTab);
                }, Qt::DirectConnection);

        window.browserChrome()->tabBar()->moveTab(
            model->indexOfId(activeId), 0);
        QVERIFY(reentryAttempted);
        QCOMPARE(window.tabController(activeId), activeController);
        QVERIFY(activeController->lifecycle()
                != BrowserTabLifecycle::Closing);
        QVERIFY(activeController->lifecycle()
                != BrowserTabLifecycle::Retired);
    }

    {
        MainWindow window(browserRoutes(server), server.origin());
        BrowserTabModel *const model = window.tabModel();
        const QString firstId = model->activeId();
        window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
        TabController *const firstController = window.tabController(firstId);
        QVERIFY(firstController != nullptr);
        bool reentryAttempted = false;
        connect(model, &BrowserTabModel::activeTabChanged, this,
                [&](int, int) {
                    if (reentryAttempted) return;
                    reentryAttempted = true;
                    window.browserChrome()->dispatchCommand(
                        BrowserCommand::CloseTab);
                }, Qt::DirectConnection);

        window.browserChrome()->tabBar()->setCurrentIndex(
            model->indexOfId(firstId));
        QVERIFY(reentryAttempted);
        QCOMPARE(model->activeId(), firstId);
        QCOMPARE(window.tabController(firstId), firstController);
        QVERIFY(firstController->lifecycle()
                != BrowserTabLifecycle::Closing);
        QVERIFY(firstController->lifecycle()
                != BrowserTabLifecycle::Retired);
    }
}

void BrowserShellTest::webTitleCallbackRejectsReentrantClose()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString tabId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const surface = window.webSurface();
    QVERIFY(waitForWebLoad(surface, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    TabController *const controller = window.tabController(tabId);
    QVERIFY(controller != nullptr);
    QWebEnginePage *const page = surface->page();
    QVERIFY(page != nullptr);

    bool observerRan = false;
    bool closeChangedTab = false;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](const int index) {
                if (observerRan || index != model->indexOfId(tabId)
                    || model->snapshotAt(index).title
                        != QStringLiteral("Observer title")) {
                    return;
                }
                observerRan = true;
                const int countBefore = model->count();
                window.browserChrome()->dispatchCommand(
                    BrowserCommand::CloseTab);
                closeChangedTab = model->count() != countBefore
                    || window.tabController(tabId) != controller
                    || controller->lifecycle()
                        == BrowserTabLifecycle::Closing
                    || controller->lifecycle()
                        == BrowserTabLifecycle::Retired
                    || controller->webSurface() != surface;
            }, Qt::DirectConnection);

    const auto changed = evaluateJavaScript(
        page,
        QStringLiteral("document.title='Observer title'; document.title"));
    QVERIFY(changed.has_value());
    QCOMPARE(changed->toString(), QStringLiteral("Observer title"));
    QTRY_VERIFY_WITH_TIMEOUT(observerRan, 5'000);
    QVERIFY(!closeChangedTab);
    QVERIFY(window.isRunning());
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->activeId(), tabId);
    QCOMPARE(window.tabController(tabId), controller);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::Active);
    QCOMPARE(controller->webSurface(), surface);
    QCOMPARE(surface->page(), page);
}

void BrowserShellTest::webLoadingCallbackRejectsReentrantShutdown()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString tabId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const surface = window.webSurface();
    QVERIFY(waitForWebLoad(surface, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    TabController *const controller = window.tabController(tabId);
    QVERIFY(controller != nullptr);

    bool observerRan = false;
    bool shutdownAccepted = true;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](const int index) {
                if (observerRan || index != model->indexOfId(tabId)
                    || !model->presentationAt(index).loading) {
                    return;
                }
                observerRan = true;
                shutdownAccepted = window.shutdown();
            }, Qt::DirectConnection);

    QVERIFY(surface->reload());
    QTRY_VERIFY_WITH_TIMEOUT(observerRan, 5'000);
    QVERIFY(!shutdownAccepted);
    QVERIFY(window.isRunning());
    QCOMPARE(model->activeId(), tabId);
    QCOMPARE(window.tabController(tabId), controller);
    QCOMPARE(controller->webSurface(), surface);
    QVERIFY(surface->page() != nullptr);
}

void BrowserShellTest::rejectedWebCallbackUiMutationsRestoreChrome()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    window.resize(900, 600);
    window.show();
    BrowserTabModel *const model = window.tabModel();
    BrowserChrome *const chrome = window.browserChrome();
    const QString webId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const surface = window.webSurface();
    QVERIFY(waitForWebLoad(surface, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));

    chrome->dispatchCommand(BrowserCommand::NewTab);
    const QString siblingId = model->activeId();
    QVERIFY(siblingId != webId);
    activateTab(window, webId);
    QLineEdit *const address = window.navigationBar()->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);

    bool observerRan = false;
    connect(model, &BrowserTabModel::persistenceNeeded, this, [&] {
        const int webIndex = model->indexOfId(webId);
        if (observerRan || webIndex < 0
            || model->snapshotAt(webIndex).title
                != QStringLiteral("Callback title")) {
            return;
        }
        observerRan = true;
        address->setText(QStringLiteral("app://pilot/web/beta"));
        QTest::keyClick(address, Qt::Key_Return);
        chrome->tabBar()->moveTab(
            model->indexOfId(webId), model->indexOfId(siblingId));
    }, Qt::DirectConnection);

    const auto changed = evaluateJavaScript(
        surface->page(),
        QStringLiteral("document.title='Callback title'; document.title"));
    QVERIFY(changed.has_value());
    QCOMPARE(changed->toString(), QStringLiteral("Callback title"));
    QTRY_VERIFY_WITH_TIMEOUT(observerRan, 5'000);
    QCOMPARE(model->snapshotAt(model->indexOfId(webId)).address,
             QStringLiteral("app://pilot/web/alpha"));
    QCOMPARE(address->text(), QStringLiteral("app://pilot/web/alpha"));
    for (int index = 0; index < model->count(); ++index) {
        QCOMPARE(chrome->tabBar()->tabData(index).toString(),
                 model->snapshotAt(index).id);
    }
}

void BrowserShellTest::webTabsOwnDistinctPagesOverOneLongLivedProfile()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());

    const QString alphaId = window.tabModel()->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const alpha = window.webSurface();
    QVERIFY(alpha != nullptr);
    QVERIFY(waitForWebLoad(alpha, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString betaId = window.tabModel()->activeId();
    QVERIFY(betaId != alphaId);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *const beta = window.webSurface();
    QVERIFY(beta != nullptr);
    QVERIFY(waitForWebLoad(beta, server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));

    QVERIFY(alpha != beta);
    QVERIFY(alpha->page() != beta->page());
    QVERIFY(alpha->view() != beta->view());
    QCOMPARE(alpha->profile(), beta->profile());
    QCOMPARE(alpha->profile(), window.webSessionProfile()->profile());
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(2));
    QCOMPARE(window.tabController(alphaId)->webSurface(), alpha);
    QCOMPARE(window.tabController(betaId)->webSurface(), beta);
}

void BrowserShellTest::switchingPreservesWebPageTitleLoadingAndState()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    const QString alphaId = window.tabModel()->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const alpha = window.webSurface();
    QVERIFY(waitForWebLoad(alpha, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    const auto stored = evaluateJavaScript(
        alpha->page(),
        QStringLiteral("sessionStorage.setItem('tab-state','alpha-owned'); 'stored'"));
    QVERIFY(stored.has_value());
    QCOMPARE(stored->toString(), QStringLiteral("stored"));

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString betaId = window.tabModel()->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *const beta = window.webSurface();
    QVERIFY(waitForWebLoad(beta, server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QCOMPARE(window.tabController(alphaId)->lifecycle(),
             BrowserTabLifecycle::Background);

    activateTab(window, alphaId);
    QCOMPARE(window.webSurface(), alpha);
    QCOMPARE(window.tabController(alphaId)->webSurface(), alpha);
    QCOMPARE(alpha->title(), QStringLiteral("Alpha page"));
    QVERIFY(!alpha->isLoading());
    QCOMPARE(window.tabController(alphaId)->lifecycle(),
             BrowserTabLifecycle::Active);
    QCOMPARE(window.tabController(betaId)->lifecycle(),
             BrowserTabLifecycle::Background);
    const auto restored = evaluateJavaScript(
        alpha->page(), QStringLiteral("sessionStorage.getItem('tab-state')"));
    QVERIFY(restored.has_value());
    QCOMPARE(restored->toString(), QStringLiteral("alpha-owned"));

    TabController *const alphaController = window.tabController(alphaId);
    QVERIFY(alphaController != nullptr);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/slow")));
    WebSurface *const slowSurface = window.webSurface();
    QVERIFY(slowSurface != nullptr);
    QWebEnginePage *const slowPage = slowSurface->page();
    QVERIFY(slowPage != nullptr);
    const quint64 slowIncarnation = alphaController->incarnation();
    QTRY_VERIFY_WITH_TIMEOUT(
        window.tabModel()->presentationAt(
            window.tabModel()->indexOfId(alphaId)).loading,
        5'000);
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount(QByteArrayLiteral("/slow")) > 0,
                             5'000);
    const int slowProgress = window.tabModel()->presentationAt(
        window.tabModel()->indexOfId(alphaId)).progress;

    activateTab(window, betaId);
    QCOMPARE(alphaController->webSurface(), slowSurface);
    QCOMPARE(slowSurface->page(), slowPage);
    QCOMPARE(alphaController->incarnation(), slowIncarnation);
    QCOMPARE(alphaController->lifecycle(), BrowserTabLifecycle::Loading);
    QVERIFY(window.tabModel()->presentationAt(
        window.tabModel()->indexOfId(alphaId)).loading);
    QCOMPARE(window.tabModel()->presentationAt(
                 window.tabModel()->indexOfId(alphaId)).progress,
             slowProgress);

    activateTab(window, alphaId);
    QCOMPARE(window.webSurface(), slowSurface);
    QCOMPARE(slowSurface->page(), slowPage);
    QCOMPARE(alphaController->incarnation(), slowIncarnation);
    QVERIFY(window.tabModel()->presentationAt(
        window.tabModel()->indexOfId(alphaId)).loading);
    QCOMPARE(window.tabModel()->presentationAt(
                 window.tabModel()->indexOfId(alphaId)).progress,
             slowProgress);
    window.browserChrome()->dispatchCommand(BrowserCommand::Stop);
    QTRY_VERIFY_WITH_TIMEOUT(
        !window.tabModel()->presentationAt(
            window.tabModel()->indexOfId(alphaId)).loading,
        5'000);
}

void BrowserShellTest::historiesAreIndependentAndInvalidInputDoesNotCommit()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString alphaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    QVERIFY(waitForWebLoad(window.webSurface(),
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    const BrowserTabSnapshot alphaBefore =
        model->snapshotAt(model->indexOfId(alphaId));

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString betaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    QVERIFY(waitForWebLoad(window.webSurface(),
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QVERIFY(window.goBack());
    QCOMPARE(model->snapshotAt(model->indexOfId(betaId)).address,
             QStringLiteral("qbrowser://newtab"));
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaBefore);
    QVERIFY(window.goForward());

    const BrowserTabSnapshot betaBefore =
        model->snapshotAt(model->indexOfId(betaId));
    QVERIFY(!window.navigate(QStringLiteral("https://example.com/outside")));
    QVERIFY(model->snapshotAt(model->indexOfId(betaId)) == betaBefore);
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaBefore);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.currentAppUrl(), betaBefore.address);
    QCOMPARE(window.navigationBar()->addressText(), betaBefore.address);
    QLabel *const error = window.surfaceStack()->currentWidget()
                              ->findChild<QLabel *>(
                                  QStringLiteral("trusted-error-message"));
    QVERIFY(error != nullptr);
    QCOMPARE(error->textFormat(), Qt::PlainText);
    QVERIFY(!error->text().isEmpty());

    QVERIFY(!window.navigate(QStringLiteral("app://pilot/not-registered")));
    QVERIFY(model->snapshotAt(model->indexOfId(betaId)) == betaBefore);
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaBefore);
}

void BrowserShellTest::inactiveRestoredDescriptorsRemainResourceFree()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    const QVector<BrowserTabSnapshot> restored{
        {QStringLiteral("11111111111111111111111111111111"),
         BrowserTabKind::Web, QStringLiteral("Alpha"),
         QStringLiteral("app://pilot/web/alpha"),
         {QStringLiteral("app://pilot/web/alpha")}, 0},
        {QStringLiteral("22222222222222222222222222222222"),
         BrowserTabKind::Web, QStringLiteral("Beta"),
         QStringLiteral("app://pilot/web/beta"),
         {QStringLiteral("app://pilot/web/beta")}, 0},
    };

    QVERIFY(window.tabModel()->replaceFromValidatedSnapshot(restored, 0));
    TabController *const active = window.tabController(restored.at(0).id);
    TabController *const inactive = window.tabController(restored.at(1).id);
    QVERIFY(active != nullptr);
    QVERIFY(inactive != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(active->webSurface() != nullptr, 2'000);
    QVERIFY(active->lifecycle() != BrowserTabLifecycle::Dormant);
    QCOMPARE(inactive->lifecycle(), BrowserTabLifecycle::Dormant);
    QCOMPARE(inactive->hostSurface(), nullptr);
    QCOMPARE(inactive->webSurface(), nullptr);
    QCOMPARE(inactive->workerSurface(), nullptr);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(), qsizetype(1));
    QCOMPARE(window.surfaceStack()->count(), 1);
}

void BrowserShellTest::lazyActivationRejectsReentrantCloseAndShutdown()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QVector<BrowserTabSnapshot> restored{
        {QStringLiteral("11111111111111111111111111111111"),
         BrowserTabKind::Host, QStringLiteral("First"),
         QStringLiteral("qbrowser://newtab"),
         {QStringLiteral("qbrowser://newtab")}, 0},
        {QStringLiteral("22222222222222222222222222222222"),
         BrowserTabKind::Host, QStringLiteral("Second"),
         QStringLiteral("qbrowser://newtab"),
         {QStringLiteral("qbrowser://newtab")}, 0},
    };

    QVERIFY(model->replaceFromValidatedSnapshot(restored, 0));
    TabController *const target = window.tabController(restored.at(1).id);
    QVERIFY(target != nullptr);
    QCOMPARE(target->lifecycle(), BrowserTabLifecycle::Dormant);

    bool reentryAttempted = false;
    bool closeChangedModel = false;
    bool shutdownAccepted = true;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](const int index) {
                if (reentryAttempted || index != model->indexOfId(restored.at(1).id)
                    || model->lifecycleAt(index)
                        != BrowserTabLifecycle::Starting) {
                    return;
                }
                reentryAttempted = true;
                const int countBefore = model->count();
                window.browserChrome()->dispatchCommand(BrowserCommand::CloseTab);
                closeChangedModel = model->count() != countBefore
                    || window.tabController(restored.at(1).id) != target;
                shutdownAccepted = window.shutdown();
            }, Qt::DirectConnection);

    window.browserChrome()->tabBar()->setCurrentIndex(
        model->indexOfId(restored.at(1).id));
    QTRY_VERIFY_WITH_TIMEOUT(reentryAttempted, 2'000);
    QVERIFY(!closeChangedModel);
    QVERIFY(!shutdownAccepted);
    QVERIFY(window.isRunning());
    QCOMPARE(window.tabController(restored.at(1).id), target);
    QTRY_VERIFY_WITH_TIMEOUT(target->hostSurface() != nullptr, 2'000);
    QCOMPARE(target->lifecycle(), BrowserTabLifecycle::Active);
}

void BrowserShellTest::closeReachesRetiredBeforeTheModelEntryIsRemoved()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString closingId = model->activeId();
    QStringList order;
    connect(model, &BrowserTabModel::tabChanged, this,
            [model, closingId, &order](int) {
                const int index = model->indexOfId(closingId);
                if (index < 0) return;
                switch (model->lifecycleAt(index)) {
                case BrowserTabLifecycle::Closing:
                    order.append(QStringLiteral("closing"));
                    break;
                case BrowserTabLifecycle::Retired:
                    order.append(QStringLiteral("retired"));
                    break;
                default:
                    break;
                }
            });
    connect(model, &BrowserTabModel::tabRemoved, this,
            [closingId, &order](int, const QString &id) {
                if (id == closingId) order.append(QStringLiteral("removed"));
            });

    window.browserChrome()->dispatchCommand(BrowserCommand::CloseTab);
    const int closing = order.indexOf(QStringLiteral("closing"));
    const int retired = order.indexOf(QStringLiteral("retired"));
    const int removed = order.indexOf(QStringLiteral("removed"));
    QVERIFY(closing >= 0);
    QVERIFY(retired > closing);
    QVERIFY(removed > retired);
    QCOMPARE(window.tabController(closingId), nullptr);
    QCOMPARE(model->count(), 1);
    QVERIFY(model->activeId() != closingId);
}

void BrowserShellTest::closeRejectsReentrantShutdown()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString closingId = model->activeId();
    bool reentryAttempted = false;
    bool shutdownAccepted = true;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](const int index) {
                if (reentryAttempted || index != model->indexOfId(closingId)
                    || model->lifecycleAt(index)
                        != BrowserTabLifecycle::Closing) {
                    return;
                }
                reentryAttempted = true;
                shutdownAccepted = window.shutdown();
            }, Qt::DirectConnection);

    window.browserChrome()->dispatchCommand(BrowserCommand::CloseTab);
    QVERIFY(reentryAttempted);
    QVERIFY(!shutdownAccepted);
    QVERIFY(window.isRunning());
    QCOMPARE(window.tabController(closingId), nullptr);
    QCOMPARE(model->count(), 1);
    QVERIFY(model->activeId() != closingId);
}

void BrowserShellTest::navigationPublishesCommittedStateBeforeOnePersistenceSignal()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();
    const QString targetId = model->activeId();
    const QString siblingId = model->createTab(
        BrowserTabKind::Host, QStringLiteral("Sibling"),
        QStringLiteral("qbrowser://newtab"), false);
    QVERIFY(!siblingId.isEmpty());
    const quint64 previousIncarnation =
        window.tabController(targetId)->incarnation();
    QStringList publicationOrder;
    bool firstCommittedPublicationSeen = false;
    bool observerSawFreshIncarnation = false;
    bool observerSawAlignedLifecycle = false;
    bool reentryAttempted = false;
    bool reentryAccepted = true;
    bool commandReentryChangedModel = false;
    bool activationReentryChangedModel = false;
    bool observerSawCommittedSnapshot = false;
    bool persistenceSawCompletedTransition = false;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](int index) {
                if (index != model->indexOfId(targetId)) return;
                const BrowserTabSnapshot snapshot = model->snapshotAt(index);
                if (snapshot.address != QStringLiteral("app://pilot/orders")) {
                    return;
                }
                if (!firstCommittedPublicationSeen) {
                    firstCommittedPublicationSeen = true;
                    observerSawFreshIncarnation =
                        window.tabController(targetId)->incarnation()
                        > previousIncarnation;
                    observerSawAlignedLifecycle =
                        window.tabController(targetId)->lifecycle()
                        == model->lifecycleAt(index);
                }
                publicationOrder.append(QStringLiteral("changed"));
                observerSawCommittedSnapshot = snapshot.history.back()
                        == QStringLiteral("app://pilot/orders")
                    && snapshot.historyIndex == snapshot.history.size() - 1;
                if (!reentryAttempted) {
                    reentryAttempted = true;
                    reentryAccepted = window.navigate(
                        QStringLiteral("app://pilot/web/alpha"));
                    const int tabCountBeforeCommand = model->count();
                    window.browserChrome()->dispatchCommand(
                        BrowserCommand::NewTab);
                    commandReentryChangedModel =
                        model->count() != tabCountBeforeCommand;
                    const QString activeBeforeActivation = model->activeId();
                    window.browserChrome()->tabBar()->setCurrentIndex(
                        model->indexOfId(siblingId));
                    activationReentryChangedModel =
                        model->activeId() != activeBeforeActivation;
                }
            }, Qt::DirectConnection);
    QSignalSpy persistenceSpy(model, &BrowserTabModel::persistenceNeeded);
    connect(model, &BrowserTabModel::persistenceNeeded, this, [&] {
        publicationOrder.append(QStringLiteral("persistence"));
        persistenceSawCompletedTransition =
            window.activeSurface() == HostSurfaceKind::TrustedError
            && window.tabController(targetId)->lifecycle()
                == BrowserTabLifecycle::TrustedError;
    }, Qt::DirectConnection);

    QVERIFY(!window.navigate(QStringLiteral("app://pilot/orders")));
    QVERIFY(observerSawCommittedSnapshot);
    QVERIFY(firstCommittedPublicationSeen);
    QVERIFY(observerSawFreshIncarnation);
    QVERIFY(observerSawAlignedLifecycle);
    QVERIFY(reentryAttempted);
    QVERIFY(!reentryAccepted);
    QVERIFY(!commandReentryChangedModel);
    QVERIFY(!activationReentryChangedModel);
    QCOMPARE(persistenceSpy.count(), 1);
    QVERIFY(persistenceSawCompletedTransition);
    QVERIFY(publicationOrder.size() >= 2);
    QCOMPARE(publicationOrder.constLast(), QStringLiteral("persistence"));
    QCOMPARE(model->activeId(), targetId);
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/orders"));
    disconnect(model, nullptr, this, nullptr);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
}

void BrowserShellTest::navigationCommandsAffectOnlyTheActiveStableTab()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    window.resize(900, 600);
    window.show();
    BrowserTabModel *const model = window.tabModel();
    BrowserChrome *const chrome = window.browserChrome();

    const QString alphaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    QVERIFY(waitForWebLoad(window.webSurface(),
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    TabController *const alphaController = window.tabController(alphaId);
    QVERIFY(alphaController != nullptr);

    chrome->dispatchCommand(BrowserCommand::NewTab);
    const QString betaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *betaSurface = window.webSurface();
    QVERIFY(waitForWebLoad(betaSurface,
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    TabController *const betaController = window.tabController(betaId);
    QVERIFY(betaController != nullptr);

    chrome->tabBar()->moveTab(model->indexOfId(betaId), 0);
    QCOMPARE(model->activeId(), betaId);
    const BrowserTabSnapshot alphaStable =
        model->snapshotAt(model->indexOfId(alphaId));
    const quint64 alphaIncarnation = alphaController->incarnation();

    chrome->dispatchCommand(BrowserCommand::Back);
    QCOMPARE(model->snapshotAt(model->indexOfId(betaId)).address,
             QStringLiteral("qbrowser://newtab"));
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaStable);
    chrome->dispatchCommand(BrowserCommand::Forward);
    QCOMPARE(model->snapshotAt(model->indexOfId(betaId)).address,
             QStringLiteral("app://pilot/web/beta"));
    betaSurface = betaController->webSurface();
    QVERIFY(waitForWebLoad(betaSurface,
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));

    const quint64 betaBeforeReload = betaController->incarnation();
    chrome->dispatchCommand(BrowserCommand::Reload);
    QVERIFY(betaController->incarnation() > betaBeforeReload);
    QCOMPARE(alphaController->incarnation(), alphaIncarnation);
    QVERIFY(waitForWebLoad(betaController->webSurface(),
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));

    QLineEdit *const address = window.navigationBar()->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);
    address->setText(QStringLiteral("app://pilot/web/slow"));
    QTest::keyClick(address, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(
        model->snapshotAt(model->indexOfId(betaId)).address,
        QStringLiteral("app://pilot/web/slow"), 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        model->presentationAt(model->indexOfId(betaId)).loading, 5'000);
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaStable);
    chrome->dispatchCommand(BrowserCommand::Stop);
    QTRY_VERIFY_WITH_TIMEOUT(
        !model->presentationAt(model->indexOfId(betaId)).loading, 5'000);
    QCOMPARE(alphaController->incarnation(), alphaIncarnation);

    chrome->dispatchCommand(BrowserCommand::Home);
    QCOMPARE(model->snapshotAt(model->indexOfId(betaId)).address,
             QStringLiteral("qbrowser://newtab"));
    QCOMPARE(model->snapshotAt(model->indexOfId(betaId)).title,
             QStringLiteral("New tab"));
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaStable);
}

void BrowserShellTest::rendererFailureRetiresOnlyTheFailedSurfaceAndReloadRecreatesIt()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();

    const QString alphaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const alpha = window.webSurface();
    QVERIFY(waitForWebLoad(alpha, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    QPointer<QWebEnginePage> retiredPage(alpha->page());
    QPointer<WebSurface> retiredSurface(alpha);
    const BrowserTabSnapshot alphaBeforeCrash =
        model->snapshotAt(model->indexOfId(alphaId));

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString betaId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *const beta = window.webSurface();
    QVERIFY(waitForWebLoad(beta, server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QWebEnginePage *const siblingPage = beta->page();

    activateTab(window, alphaId);
    TabController *const alphaController = window.tabController(alphaId);
    QVERIFY(alphaController != nullptr);
    const quint64 crashedIncarnation = alphaController->incarnation();
    bool errorPublicationObserved = false;
    bool errorPublicationWasAtomic = true;
    connect(model, &BrowserTabModel::tabChanged, this,
            [&](const int index) {
                if (index != model->indexOfId(alphaId)) return;
                TabController *const controller =
                    window.tabController(alphaId);
                if (controller == nullptr
                    || controller->surfaceKind()
                        != HostSurfaceKind::TrustedError) {
                    return;
                }
                errorPublicationObserved = true;
                errorPublicationWasAtomic = errorPublicationWasAtomic
                    && controller->currentSurface() != nullptr
                    && window.surfaceStack()->currentWidget()
                        == controller->currentSurface()
                    && window.surfaceStack()->currentWidget() != beta;
            }, Qt::DirectConnection);
    QVERIFY(QMetaObject::invokeMethod(
        retiredPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
              Q_ARG(int, 91)));
    QCOMPARE(window.tabController(alphaId)->webSurface(), alpha);
    QCOMPARE(window.surfaceStack()->currentWidget(), alpha);
    QTRY_COMPARE_WITH_TIMEOUT(window.tabController(alphaId)->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QVERIFY(alphaController->incarnation() > crashedIncarnation);
    QVERIFY(errorPublicationObserved);
    QVERIFY(errorPublicationWasAtomic);
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaBeforeCrash);
    QTRY_VERIFY_WITH_TIMEOUT(retiredSurface.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(retiredPage.isNull(), 5'000);
    QCOMPARE(window.tabController(alphaId)->webSurface(), nullptr);
    QCOMPARE(window.tabController(alphaId)->surfaceKind(),
             HostSurfaceKind::TrustedError);
    QCOMPARE(model->lifecycleAt(model->indexOfId(alphaId)),
             BrowserTabLifecycle::TrustedError);
    QCOMPARE(model->snapshotAt(model->indexOfId(alphaId)).address,
             QStringLiteral("app://pilot/web/alpha"));
    QCOMPARE(window.surfaceStack()->currentWidget(),
             window.tabController(alphaId)->currentSurface());
    QVERIFY(window.surfaceStack()->currentWidget() != beta);
    QCOMPARE(window.tabController(betaId)->webSurface(), beta);
    QCOMPARE(beta->page(), siblingPage);
    QVERIFY(!window.navigate(QStringLiteral("https://example.com/outside")));
    QVERIFY(model->snapshotAt(model->indexOfId(alphaId)) == alphaBeforeCrash);

    const quint64 firstErrorIncarnation = alphaController->incarnation();
    window.browserChrome()->dispatchCommand(BrowserCommand::Reload);
    WebSurface *const replacement = window.webSurface();
    QVERIFY(replacement != nullptr);
    QVERIFY(replacement->page() != nullptr);
    QVERIFY(alphaController->incarnation() > firstErrorIncarnation);
    QVERIFY(waitForWebLoad(replacement,
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    QCOMPARE(window.tabController(betaId)->webSurface(), beta);
    QCOMPARE(beta->page(), siblingPage);

    QPointer<WebSurface> reloadedSurface(replacement);
    QPointer<QWebEnginePage> reloadedPage(replacement->page());
    QVERIFY(QMetaObject::invokeMethod(
        reloadedPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 92)));
    QTRY_COMPARE_WITH_TIMEOUT(window.tabController(alphaId)->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(reloadedSurface.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(reloadedPage.isNull(), 5'000);
    const quint64 secondErrorIncarnation = alphaController->incarnation();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *const otherNavigation = window.webSurface();
    QVERIFY(otherNavigation != nullptr);
    QVERIFY(alphaController->incarnation() > secondErrorIncarnation);
    QVERIFY(waitForWebLoad(otherNavigation,
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QCOMPARE(model->snapshotAt(model->indexOfId(alphaId)).address,
             QStringLiteral("app://pilot/web/beta"));
    QCOMPARE(window.tabController(betaId)->webSurface(), beta);
    QCOMPARE(beta->page(), siblingPage);

    QPointer<WebSurface> closingSurface(otherNavigation);
    QPointer<QWebEnginePage> closingPage(otherNavigation->page());
    QVERIFY(QMetaObject::invokeMethod(
        closingPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 93)));
    QTRY_COMPARE_WITH_TIMEOUT(window.tabController(alphaId)->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(closingSurface.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(closingPage.isNull(), 5'000);
    window.browserChrome()->dispatchCommand(BrowserCommand::CloseTab);
    QCOMPARE(window.tabController(alphaId), nullptr);
    QCOMPARE(model->activeId(), betaId);
    QCOMPARE(window.tabController(betaId)->webSurface(), beta);
    QCOMPARE(beta->page(), siblingPage);

    QPointer<WebSurface> shutdownSurface(beta);
    QPointer<QWebEnginePage> shutdownPage(beta->page());
    QVERIFY(QMetaObject::invokeMethod(
        shutdownPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 94)));
    QTRY_COMPARE_WITH_TIMEOUT(window.tabController(betaId)->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(shutdownSurface.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(shutdownPage.isNull(), 5'000);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
}

void BrowserShellTest::reloadBeforeQueuedRendererCleanupCannotReuseFailedSurface()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();

    const QString failedId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const failedSurface = window.webSurface();
    QVERIFY(waitForWebLoad(failedSurface,
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    QPointer<WebSurface> failedSurfaceGuard(failedSurface);
    QPointer<QWebEnginePage> failedPage(failedSurface->page());
    QPointer<QWebEngineView> failedView(failedSurface->view());

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString siblingId = model->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    WebSurface *const siblingSurface = window.webSurface();
    QVERIFY(waitForWebLoad(siblingSurface,
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QWebEnginePage *const siblingPage = siblingSurface->page();
    QWebEngineView *const siblingView = siblingSurface->view();

    activateTab(window, failedId);
    TabController *const controller = window.tabController(failedId);
    QVERIFY(controller != nullptr);
    const quint64 failedIncarnation = controller->incarnation();
    QVERIFY(QMetaObject::invokeMethod(
        failedPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 97)));

    window.browserChrome()->dispatchCommand(BrowserCommand::Reload);
    QCOMPARE(controller->webSurface(), failedSurface);
    QVERIFY(controller->incarnation() > failedIncarnation);
    QVERIFY(!failedSurfaceGuard.isNull());
    QVERIFY(!failedPage.isNull());
    QVERIFY(!failedView.isNull());

    QTRY_COMPARE_WITH_TIMEOUT(controller->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(failedSurfaceGuard.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(failedPage.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(failedView.isNull(), 5'000);
    QCOMPARE(controller->webSurface(), nullptr);
    QCOMPARE(model->lifecycleAt(model->indexOfId(failedId)),
             BrowserTabLifecycle::TrustedError);
    QCOMPARE(window.surfaceStack()->currentWidget(),
             controller->currentSurface());
    QCOMPARE(window.tabController(siblingId)->webSurface(), siblingSurface);
    QCOMPARE(siblingSurface->page(), siblingPage);
    QCOMPARE(siblingSurface->view(), siblingView);

    const quint64 errorIncarnation = controller->incarnation();
    window.browserChrome()->dispatchCommand(BrowserCommand::Reload);
    QPointer<WebSurface> freshSurface(controller->webSurface());
    QVERIFY(!freshSurface.isNull());
    QPointer<QWebEnginePage> freshPage(freshSurface->page());
    QPointer<QWebEngineView> freshView(freshSurface->view());
    QVERIFY(!freshPage.isNull());
    QVERIFY(!freshView.isNull());
    QVERIFY(controller->incarnation() > errorIncarnation);
    QVERIFY(waitForWebLoad(freshSurface.data(),
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    QCOMPARE(controller->surfaceKind(), HostSurfaceKind::Web);
    QCOMPARE(controller->webSurface(), freshSurface.data());
    QCOMPARE(freshSurface->page(), freshPage.data());
    QCOMPARE(freshSurface->view(), freshView.data());
    QCOMPARE(window.tabController(siblingId)->webSurface(), siblingSurface);
    QCOMPARE(siblingSurface->page(), siblingPage);
    QCOMPARE(siblingSurface->view(), siblingView);
}

void BrowserShellTest::lateRendererFailureCannotRetireFreshIncarnation()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());

    const QString stableId = window.tabModel()->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const originalSurface = window.webSurface();
    QVERIFY(waitForWebLoad(originalSurface,
                           server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    TabController *const controller = window.tabController(stableId);
    QVERIFY(controller != nullptr);
    const quint64 failedIncarnation = controller->incarnation();
    QPointer<WebSurface> retiredSurface(originalSurface);
    QPointer<QWebEnginePage> retiredPage(originalSurface->page());

    QVERIFY(QMetaObject::invokeMethod(
        retiredPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 95)));

    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/beta")));
    QTRY_VERIFY_WITH_TIMEOUT(retiredSurface.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(retiredPage.isNull(), 5'000);
    QCOMPARE(window.tabController(stableId), controller);
    QVERIFY(controller->incarnation() > failedIncarnation);
    QPointer<WebSurface> freshSurface(controller->webSurface());
    QVERIFY(!freshSurface.isNull());
    QPointer<QWebEnginePage> freshPage(freshSurface->page());
    QVERIFY(!freshPage.isNull());

    QVERIFY(waitForWebLoad(freshSurface.data(),
                           server.url(QStringLiteral("beta")),
                           QStringLiteral("Beta page")));
    QCOMPARE(window.tabController(stableId), controller);
    QCOMPARE(controller->surfaceKind(), HostSurfaceKind::Web);
    QCOMPARE(controller->webSurface(), freshSurface.data());
    QCOMPARE(freshSurface->page(), freshPage.data());
    QCOMPARE(window.tabModel()->presentationAt(
                 window.tabModel()->indexOfId(stableId)).visualState,
             BrowserVisualState::Normal);
}

void BrowserShellTest::rendererFailureAtPageLimitReleasesSlotBeforeReload()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    BrowserTabModel *const model = window.tabModel();

    for (qsizetype index = 0;
         index < WebSessionProfile::maximumPageCount; ++index) {
        if (index != 0) {
            window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
        }
        QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
        QCOMPARE(window.webSessionProfile()->registeredPageCount(), index + 1);
    }
    QCOMPARE(model->count(), BrowserTabModel::MaxOpenTabs);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(),
             WebSessionProfile::maximumPageCount);

    const QString failedId = model->activeId();
    TabController *const controller = window.tabController(failedId);
    QVERIFY(controller != nullptr);
    const quint64 crashedIncarnation = controller->incarnation();
    WebSurface *const failedSurface = controller->webSurface();
    QVERIFY(failedSurface != nullptr);
    QPointer<WebSurface> failedSurfaceGuard(failedSurface);
    QPointer<QWebEnginePage> failedPage(failedSurface->page());
    QVERIFY(!failedPage.isNull());

    QVERIFY(QMetaObject::invokeMethod(
        failedPage.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 96)));
    QTRY_COMPARE_WITH_TIMEOUT(controller->surfaceKind(),
                              HostSurfaceKind::TrustedError, 5'000);
    QVERIFY(controller->incarnation() > crashedIncarnation);
    QTRY_VERIFY_WITH_TIMEOUT(failedSurfaceGuard.isNull(), 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(failedPage.isNull(), 5'000);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(),
             WebSessionProfile::maximumPageCount - 1);

    const quint64 errorIncarnation = controller->incarnation();
    window.browserChrome()->dispatchCommand(BrowserCommand::Reload);
    WebSurface *const replacement = controller->webSurface();
    QVERIFY(replacement != nullptr);
    QVERIFY(replacement->page() != nullptr);
    QVERIFY(controller->incarnation() > errorIncarnation);
    QCOMPARE(window.webSessionProfile()->registeredPageCount(),
             WebSessionProfile::maximumPageCount);
}

void BrowserShellTest::windowShutdownBeforeQueuedRendererCleanupCancelsIt()
{
    BrowserServer server;
    QVERIFY(server.listen());
    MainWindow window(browserRoutes(server), server.origin());
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web/alpha")));
    WebSurface *const surface = window.webSurface();
    QVERIFY(waitForWebLoad(surface, server.url(QStringLiteral("alpha")),
                           QStringLiteral("Alpha page")));
    QPointer<WebSurface> surfaceGuard(surface);
    QPointer<QWebEnginePage> pageGuard(surface->page());
    QVERIFY(!pageGuard.isNull());

    QVERIFY(QMetaObject::invokeMethod(
        pageGuard.data(), "renderProcessTerminated", Qt::DirectConnection,
        Q_ARG(QWebEnginePage::RenderProcessTerminationStatus,
              QWebEnginePage::CrashedTerminationStatus),
        Q_ARG(int, 97)));
    QCOMPARE(window.webSurface(), surface);
    QVERIFY(window.shutdown());
    QVERIFY(window.isShutdownComplete());
    QVERIFY(surfaceGuard.isNull());
    QVERIFY(pageGuard.isNull());
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-shell-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    BrowserShellTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_browser_shell.moc"
