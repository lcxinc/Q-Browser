#include "HostApplication.h"
#include "BrowserChrome.h"
#include "BrowserCommand.h"
#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "HostOwnedFileAuthority.h"
#include "MainWindow.h"
#include "NavigationBar.h"
#include "HostWorkerSessionController.h"
#include "HostWorkerSessionTestHooks.h"
#include "ProtocolMessage.h"
#include "RouteRegistry.h"
#include "SignatureVerifier.h"
#include "WebSurface.h"
#include "WorkerSurface.h"
#include "WorkerTestEnvironment.h"
#include "WorkerRetirementManager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLineEdit>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTabBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWebEnginePage>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#ifdef Q_OS_WIN
DWORD processHandleCount()
{
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
}

bool protectPath(const QString &path, const bool container = false)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    if (queried != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemBytes = sizeof(systemBuffer);
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer,
                           &systemBytes) == FALSE) {
        LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESSW entries[2]{};
    for (EXPLICIT_ACCESSW &entry : entries) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = container
            ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(owner);
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemBuffer);
    PACL dacl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(2, entries, nullptr, &dacl);
    const DWORD applied = aclResult == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : aclResult;
    if (dacl != nullptr) LocalFree(dacl);
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
}
#endif

QString currentExecutablePath()
{
#ifdef Q_OS_WIN
    std::vector<wchar_t> buffer(32U * 1024U);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(GetCurrentProcess(), 0U, buffer.data(),
                                   &length) == FALSE
        || length == 0U || length >= buffer.size()) {
        return {};
    }
    return QString::fromWCharArray(buffer.data(),
                                   static_cast<qsizetype>(length));
#else
    return QCoreApplication::applicationFilePath();
#endif
}

bool copyPlainTree(const QString &source, const QString &destination)
{
    const QFileInfo sourceInfo(source);
    const QString canonicalSource = sourceInfo.canonicalFilePath();
    if (!sourceInfo.isDir() || sourceInfo.isSymLink()
        || canonicalSource.isEmpty() || !QDir().mkpath(destination)) {
        return false;
    }
    QDirIterator entries(canonicalSource,
                         QDir::AllEntries | QDir::Hidden | QDir::System
                             | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    const QDir sourceDirectory(canonicalSource);
    while (entries.hasNext()) {
        const QString path = entries.next();
        const QFileInfo information(path);
        const QString target = QDir(destination).filePath(
            sourceDirectory.relativeFilePath(path));
        if (information.isDir()) {
            if (!QDir().mkpath(target)) return false;
        } else if (!information.isFile()
                   || !QDir().mkpath(QFileInfo(target).absolutePath())
                   || !QFile::copy(path, target)) {
            return false;
        }
    }
    return true;
}

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

using TestNavigationCallback =
    std::function<bool(const QString &appId, const QString &route)>;

template <typename Controller>
std::unique_ptr<Controller> makeSessionController(
    MainWindow *const legacyWindow, TestNavigationCallback navigate)
{
    if constexpr (std::is_constructible_v<Controller, TestNavigationCallback>) {
        return std::make_unique<Controller>(std::move(navigate));
    } else {
        Q_UNUSED(navigate)
        return std::make_unique<Controller>(legacyWindow);
    }
}

template <typename Controller>
bool requestOwnedRoute(Controller &controller, MainWindow &legacyWindow,
                       const QString &appId, const QString &route)
{
    if constexpr (requires { controller.requestRouteLoad(route); }) {
        return controller.requestRouteLoad(route);
    } else {
        return QMetaObject::invokeMethod(
            &legacyWindow, "workerRouteRequested", Qt::DirectConnection,
            Q_ARG(QString, appId),
            Q_ARG(QString, QStringLiteral("qml/Main.qml")),
            Q_ARG(QVariantMap, QVariantMap{}),
            Q_ARG(QUrl, QUrl(QStringLiteral("app://pilot") + route)));
    }
}

template <typename Controller>
quint64 sessionGeneration(const Controller &controller)
{
    if constexpr (requires { controller.generation(); }) {
        return controller.generation();
    } else {
        return 0;
    }
}

class ControllerMetaCallProbe;

struct DestroyingNavigationCallbackState final {
    std::unique_ptr<HostWorkerSessionController> *owner = nullptr;
    const ControllerMetaCallProbe *controllerMetaCallProbe = nullptr;
    int nextTargetId = 0;
    int executingTargetId = 0;
    int invocationCount = 0;
    bool executingTargetDestroyed = false;
    bool invokedFromControllerMetaCallDelivery = false;
    bool ownerDestroyedByCallback = false;
};

class ControllerMetaCallProbe final : public QObject
{
public:
    void arm() noexcept { armed_ = true; }

    bool hasObservedDelivery() const noexcept { return deliveryCount_ != 0; }
    int deliveryCount() const noexcept { return deliveryCount_; }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (armed_ && event->type() == QEvent::MetaCall) ++deliveryCount_;
        return false;
    }

private:
    bool armed_ = false;
    int deliveryCount_ = 0;
};

class DestroyingNavigationCallback final
{
public:
    explicit DestroyingNavigationCallback(
        std::shared_ptr<DestroyingNavigationCallbackState> state)
        : state_(std::move(state)), targetId_(++state_->nextTargetId)
    {
    }

    DestroyingNavigationCallback(const DestroyingNavigationCallback &other)
        : state_(other.state_), targetId_(++state_->nextTargetId)
    {
    }

    DestroyingNavigationCallback(DestroyingNavigationCallback &&other) noexcept
        : state_(std::move(other.state_)), targetId_(std::exchange(other.targetId_, 0))
    {
    }

    ~DestroyingNavigationCallback()
    {
        if (state_ != nullptr && targetId_ != 0
            && state_->executingTargetId == targetId_) {
            state_->executingTargetDestroyed = true;
        }
    }

    bool operator()(const QString &, const QString &) const
    {
        const std::shared_ptr<DestroyingNavigationCallbackState> state = state_;
        const int executingTargetId = targetId_;
        state->executingTargetId = executingTargetId;
        ++state->invocationCount;
        state->invokedFromControllerMetaCallDelivery =
            state->controllerMetaCallProbe != nullptr
            && state->controllerMetaCallProbe->hasObservedDelivery();
        if (state->owner != nullptr && *state->owner != nullptr) {
            (void)(*state->owner)->shutdown(
                QStringLiteral("navigation.callback.destroy"));
            if (!state->invokedFromControllerMetaCallDelivery) {
                state->owner->reset();
                state->ownerDestroyedByCallback = true;
            }
        }
        state->executingTargetId = 0;
        return true;
    }

private:
    std::shared_ptr<DestroyingNavigationCallbackState> state_;
    int targetId_ = 0;
};

} // namespace

class UnifiedNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void routeRegistryAloneSelectsOneActiveSurfaceAndStableHistory();
    void tabKeyedWorkerSurfacesStayIndependent();
    void packageNavigationPublishesTabLaunchRequest();
    void legacyWorkerAdapterRejectsASecondAppTab();
    void legacyWorkerDetachDuringRouteStartWinsTransition();
    void workerNavigationIsSameAppAndHistoryAware();
    void hostApplicationOwnsAttachableWorkerSessionController();
    void hostApplicationBindsWorkerContextLifecycle();
    void failedWorkerContextAttachmentConsumesSurfaceExactlyOnce();
    void hostWorkerRoutesAreTrackedWithoutDuplicateWorkerNavigation();
    void stalledWorkerReaderNeverBlocksTheGuiThread();
    void capabilityPendingKeepsHeartbeatAndBoundsSecondRequest();
    void gracefulShutdownCleansIoBeforeReattach();
    void failedSessionCanReattachBeforeOldCallbacksDrain();
    void reattachAfterIoThreadFinishedBeforeGuiCleanup();
    void destroyAfterIoThreadFinishedBeforeGuiCleanup();
    void navigationTransactionsRejectReentrantCommands();
    void trustedShellRuntimeConfigKeepsPilotRouteIdentity();
    void pageMetadataUsesOnlyCurrentGenerationAndResanitizes();
    void preinstalledPageMetadataHandlerIsClearedBeforeIoTransfer();
    void sameAppSessionsRouteOnlyTheirOwnTab();
    void backgroundWorkerNavigationUpdatesOnlyOwningHistory();
    void retiredSessionRejectsLateTabNavigation();
    void sessionDetachedMarksSafeRebindPoint();
    void pageMetadataFromOneSessionUpdatesOnlyOwningTab();
    void navigationCallbackExceptionFailsClosedWithoutAffectingSibling();
    void navigationCallbackMayDestroyOwningController();
};

void UnifiedNavigationTest::trustedShellRuntimeConfigKeepsPilotRouteIdentity()
{
    const HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell")});
    QVERIFY(parsed.value.has_value());
    QCOMPARE(parsed.value->mode(), HostRuntimeMode::TrustedShell);
    QVERIFY(parsed.value->appId().isEmpty());
    HostApplication host(std::move(*parsed.value));
    QVERIFY(host.start());
    QVERIFY(host.mainWindow() != nullptr);
}

void UnifiedNavigationTest::pageMetadataUsesOnlyCurrentGenerationAndResanitizes()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(
        [&window](const QString &appId, const QString &route) {
            return window.navigateFromWorker(appId, route);
        });
    QSignalSpy metadataSpy(&controller,
                           &HostWorkerSessionController::pageMetadataChanged);

    auto first = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(first.has_value());
    QVERIFY(controller.attach(std::move(first->host)));
    QVERIFY(first->worker->send(ProtocolMessage::ready()));
    QVERIFY(first->worker->send(*ProtocolMessage::pageMetadata(
        QStringLiteral("Orders"), QStringLiteral("ready"))));
    QTRY_COMPARE_WITH_TIMEOUT(metadataSpy.count(), 1, 3000);
    QCOMPARE(metadataSpy.at(0).at(0).toULongLong(), quint64(1));
    QCOMPARE(metadataSpy.at(0).at(1).toString(), QStringLiteral("Orders"));
    QCOMPARE(metadataSpy.at(0).at(2).toString(), QStringLiteral("ready"));

    QString recanonicalized = QStringLiteral("Safe");
    recanonicalized.append(QChar(0x202e));
    recanonicalized.append(QChar(0x206f));
    recanonicalized.append(QStringLiteral(" title"));
    QVERIFY(QMetaObject::invokeMethod(
        &controller, "handlePageMetadata", Qt::DirectConnection,
        Q_ARG(quint64, quint64(1)), Q_ARG(QString, recanonicalized),
        Q_ARG(QString, QStringLiteral("loading"))));
    QCOMPARE(metadataSpy.count(), 2);
    QCOMPARE(metadataSpy.at(1).at(1).toString(), QStringLiteral("Safe title"));
    QVERIFY(QMetaObject::invokeMethod(
        &controller, "handlePageMetadata", Qt::DirectConnection,
        Q_ARG(quint64, quint64(1)),
        Q_ARG(QString, QStringLiteral("<b>Forged</b>")),
        Q_ARG(QString, QStringLiteral("ready"))));
    QCOMPARE(metadataSpy.count(), 2);
    for (const QString &status : {QStringLiteral("admin"),
                                  QStringLiteral("trusted"),
                                  QStringLiteral("loading-1"),
                                  QStringLiteral("READY"),
                                  QStringLiteral("Loading")}) {
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "handlePageMetadata", Qt::DirectConnection,
            Q_ARG(quint64, quint64(1)),
            Q_ARG(QString, QStringLiteral("Forged status")),
            Q_ARG(QString, status)));
        QCOMPARE(metadataSpy.count(), 2);
    }

    auto replacement = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(replacement.has_value());
    QVERIFY(controller.attach(std::move(replacement->host)));
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Running, 5000);
    QVERIFY(replacement->worker->send(ProtocolMessage::ready()));
    QVERIFY(replacement->worker->send(*ProtocolMessage::pageMetadata(
        QStringLiteral("Customers"))));
    QTRY_COMPARE_WITH_TIMEOUT(metadataSpy.count(), 3, 3000);
    QCOMPARE(metadataSpy.at(2).at(0).toULongLong(), quint64(2));
    QCOMPARE(metadataSpy.at(2).at(1).toString(), QStringLiteral("Customers"));

    QVERIFY(QMetaObject::invokeMethod(
        &controller, "handlePageMetadata", Qt::DirectConnection,
        Q_ARG(quint64, quint64(1)), Q_ARG(QString, QStringLiteral("Stale")),
        Q_ARG(QString, QString())));
    QCOMPARE(metadataSpy.count(), 3);
    replacement->worker->close();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Failed, 3000);
    QVERIFY(QMetaObject::invokeMethod(
        &controller, "handlePageMetadata", Qt::DirectConnection,
        Q_ARG(quint64, quint64(2)), Q_ARG(QString, QStringLiteral("Closed")),
        Q_ARG(QString, QString())));
    QCOMPARE(metadataSpy.count(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasIoThread(), 6000);
}

void UnifiedNavigationTest::preinstalledPageMetadataHandlerIsClearedBeforeIoTransfer()
{
    HostWorkerSessionController controller(
        [](const QString &, const QString &) { return false; });
    QThread *const controllerThread = controller.thread();
    std::atomic<int> oldHandlerCount{0};
    std::atomic<QThread *> oldHandlerThread{nullptr};
    int metadataSignalCount = 0;
    QThread *metadataSignalThread = nullptr;
    quint64 metadataGeneration = 0;
    QString metadataTitle;
    QString metadataStatus;
    connect(&controller, &HostWorkerSessionController::pageMetadataChanged,
            &controller,
            [&](const quint64 generation, const QString &title,
                const QString &status) {
                ++metadataSignalCount;
                metadataSignalThread = QThread::currentThread();
                metadataGeneration = generation;
                metadataTitle = title;
                metadataStatus = status;
            },
            Qt::DirectConnection);
    QSignalSpy heartbeatSpy(&controller,
                            &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(heartbeatSpy.isValid());

    auto sessions = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(sessions.has_value());
    sessions->host->setPageMetadataHandler(
        [&](const QString &, const QString &) {
            oldHandlerThread.store(QThread::currentThread(),
                                   std::memory_order_relaxed);
            oldHandlerCount.fetch_add(1, std::memory_order_relaxed);
        });
    QVERIFY(controller.attach(std::move(sessions->host)));
    const quint64 attachedGeneration = controller.generation();

    QVERIFY(sessions->worker->send(ProtocolMessage::ready()));
    const auto metadata = ProtocolMessage::pageMetadata(
        QStringLiteral("Transferred Session"), QStringLiteral("ready"));
    QVERIFY(metadata.has_value());
    QVERIFY(sessions->worker->send(*metadata));
    QVERIFY(sessions->worker->send(ProtocolMessage::heartbeat(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(metadataSignalCount, 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(heartbeatSpy.count(), 1, 3000);

    sessions->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasIoThread(), 6000);
    QCOMPARE(metadataSignalCount, 1);
    QCOMPARE(metadataSignalThread, controllerThread);
    QCOMPARE(metadataGeneration, attachedGeneration);
    QCOMPARE(metadataTitle, QStringLiteral("Transferred Session"));
    QCOMPARE(metadataStatus, QStringLiteral("ready"));
    QCOMPARE(oldHandlerCount.load(std::memory_order_relaxed), 0);
    QCOMPARE(oldHandlerThread.load(std::memory_order_relaxed), nullptr);
}

void UnifiedNavigationTest::sameAppSessionsRouteOnlyTheirOwnTab()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    auto firstController = makeSessionController<HostWorkerSessionController>(
        &window, [](const QString &, const QString &) { return false; });
    auto secondController = makeSessionController<HostWorkerSessionController>(
        &window, [](const QString &, const QString &) { return false; });
    auto first = authenticatedSessions(appId);
    auto second = authenticatedSessions(appId);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(firstController->attach(std::move(first->host)));
    QVERIFY(secondController->attach(std::move(second->host)));

    QSignalSpy firstAcknowledged(
        firstController.get(),
        &HostWorkerSessionController::routeLoadAcknowledged);
    QSignalSpy firstHeartbeats(
        firstController.get(),
        &HostWorkerSessionController::heartbeatObserved);
    QSignalSpy secondHeartbeats(
        secondController.get(),
        &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(firstAcknowledged.isValid());
    QVERIFY(firstHeartbeats.isValid());
    QVERIFY(secondHeartbeats.isValid());

    const QString route = QStringLiteral("/orders");
    QVERIFY(requestOwnedRoute(*firstController, window, appId, route));
    const SessionReceiveResult firstRoute = first->worker->receive(1000);
    QCOMPARE(firstRoute.status, SessionStatus::MessageReady);
    QVERIFY(firstRoute.message.has_value());
    QCOMPARE(firstRoute.message->type(), ProtocolType::RouteLoad);
    QCOMPARE(firstRoute.message->payload()
                 .value(QStringLiteral("route")).toString(),
             route);

    QTest::qWait(100);
    const SessionReceiveResult siblingRoute = second->worker->poll(0);
    QCOMPARE(siblingRoute.status, SessionStatus::TimedOut);
    QVERIFY(!second->worker->isClosed());

    QCOMPARE(firstController->pendingRouteLoadCount(), qsizetype(1));
    QVERIFY(first->worker->send(ProtocolMessage::heartbeat(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(firstHeartbeats.count(), 1, 3000);
    QCOMPARE(firstHeartbeats.at(0).at(0).toULongLong(),
             sessionGeneration(*firstController));
    QCOMPARE(firstController->pendingRouteLoadCount(), qsizetype(1));

    const auto routeAcknowledgement = ProtocolMessage::successResponse(
        firstRoute.message->requestId(),
        QJsonObject{{QStringLiteral("route"), route}});
    QVERIFY(routeAcknowledgement.has_value());
    QVERIFY(first->worker->send(*routeAcknowledgement, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(firstAcknowledged.count(), 1, 3000);
    QCOMPARE(firstAcknowledged.at(0).at(0).toString(), route);
    QCOMPARE(firstAcknowledged.at(0).at(1).toULongLong(),
             sessionGeneration(*firstController));

    QVERIFY(firstController->shutdown(QStringLiteral("first.retired")));
    const SessionReceiveResult firstShutdown = first->worker->receive(1000);
    QCOMPARE(firstShutdown.status, SessionStatus::MessageReady);
    QCOMPARE(firstShutdown.message->type(), ProtocolType::Shutdown);
    const auto firstShutdownAck = ProtocolMessage::shutdown(
        QStringLiteral("first.retired.ack"));
    QVERIFY(firstShutdownAck.has_value());
    QVERIFY(first->worker->send(*firstShutdownAck, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(firstController->state(),
                              HostWorkerSessionState::Detached, 3000);
    QCOMPARE(secondController->state(), HostWorkerSessionState::Running);
    QVERIFY(second->worker->send(ProtocolMessage::heartbeat(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(secondHeartbeats.count(), 1, 3000);
    QCOMPARE(secondHeartbeats.at(0).at(0).toULongLong(),
             sessionGeneration(*secondController));

    QVERIFY(secondController->shutdown(QStringLiteral("second.complete")));
    const SessionReceiveResult secondShutdown = second->worker->receive(1000);
    QCOMPARE(secondShutdown.status, SessionStatus::MessageReady);
    QCOMPARE(secondShutdown.message->type(), ProtocolType::Shutdown);
    const auto secondShutdownAck = ProtocolMessage::shutdown(
        QStringLiteral("second.complete.ack"));
    QVERIFY(secondShutdownAck.has_value());
    QVERIFY(second->worker->send(*secondShutdownAck, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(secondController->state(),
                              HostWorkerSessionState::Detached, 3000);
}

void UnifiedNavigationTest::backgroundWorkerNavigationUpdatesOnlyOwningHistory()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow legacyWindow(routes(server.helpUrl()), server.origin());
    BrowserTabModel model;
    const QString firstTab = model.createTab(
        BrowserTabKind::App, QStringLiteral("First"),
        QStringLiteral("app://pilot/web-shaped-worker/1"));
    const QString secondTab = model.createTab(
        BrowserTabKind::App, QStringLiteral("Second"),
        QStringLiteral("app://pilot/web-shaped-worker/2"));
    QVERIFY(!firstTab.isEmpty());
    QVERIFY(!secondTab.isEmpty());
    QCOMPARE(model.activeId(), secondTab);
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    auto firstController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow,
        [&model, firstTab, appId](const QString &callbackAppId,
                                  const QString &route) {
            return callbackAppId == appId
                && model.navigateTab(firstTab, BrowserTabKind::App,
                                     QStringLiteral("app://pilot") + route);
        });
    auto secondController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow,
        [&model, secondTab, appId](const QString &callbackAppId,
                                   const QString &route) {
            return callbackAppId == appId
                && model.navigateTab(secondTab, BrowserTabKind::App,
                                     QStringLiteral("app://pilot") + route);
        });
    auto first = authenticatedSessions(appId);
    auto second = authenticatedSessions(appId);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(firstController->attach(std::move(first->host)));
    QVERIFY(secondController->attach(std::move(second->host)));

    const BrowserTabSnapshot firstBefore = model.snapshotAt(
        model.indexOfId(firstTab));
    const BrowserTabSnapshot secondBefore = model.snapshotAt(
        model.indexOfId(secondTab));
    const QString route = QStringLiteral("/orders");
    QVERIFY(first->worker->sendNavigationRequest(
        QStringLiteral("background-navigation"), route, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(
        model.snapshotAt(model.indexOfId(firstTab)).address,
        QStringLiteral("app://pilot/orders"), 3000);

    const SessionReceiveResult navigationResponse = first->worker->receive(1000);
    QCOMPARE(navigationResponse.status, SessionStatus::MessageReady);
    QCOMPARE(navigationResponse.message->type(), ProtocolType::Response);
    QVERIFY(navigationResponse.message->payload()
                .value(QStringLiteral("ok")).toBool(false));
    const SessionReceiveResult routeLoad = first->worker->receive(1000);
    QCOMPARE(routeLoad.status, SessionStatus::MessageReady);
    QCOMPARE(routeLoad.message->type(), ProtocolType::RouteLoad);
    QCOMPARE(routeLoad.message->payload()
                 .value(QStringLiteral("route")).toString(),
             route);
    const auto routeAcknowledgement = ProtocolMessage::successResponse(
        routeLoad.message->requestId(),
        QJsonObject{{QStringLiteral("route"), route}});
    QVERIFY(routeAcknowledgement.has_value());
    QVERIFY(first->worker->send(*routeAcknowledgement, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(firstController->pendingRouteLoadCount(),
                              qsizetype(0), 3000);

    const BrowserTabSnapshot firstAfter = model.snapshotAt(
        model.indexOfId(firstTab));
    const BrowserTabSnapshot secondAfter = model.snapshotAt(
        model.indexOfId(secondTab));
    QCOMPARE(firstAfter.history.size(), firstBefore.history.size() + 1);
    QCOMPARE(firstAfter.history.constLast(),
             QStringLiteral("app://pilot/orders"));
    QCOMPARE(secondAfter, secondBefore);
    QCOMPARE(model.activeId(), secondTab);

    first->worker->close();
    second->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!firstController->hasIoThread(), 6000);
    QTRY_VERIFY_WITH_TIMEOUT(!secondController->hasIoThread(), 6000);
}

void UnifiedNavigationTest::retiredSessionRejectsLateTabNavigation()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow legacyWindow(routes(server.helpUrl()), server.origin());
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    int navigationCount = 0;
    auto controller = makeSessionController<HostWorkerSessionController>(
        &legacyWindow,
        [&navigationCount, appId](const QString &callbackAppId,
                                  const QString &) {
            if (callbackAppId != appId) return false;
            ++navigationCount;
            return true;
        });
    auto retired = authenticatedSessions(appId);
    auto current = authenticatedSessions(appId);
    QVERIFY(retired.has_value());
    QVERIFY(current.has_value());
    QVERIFY(!controller->requestRouteLoad(QStringLiteral("/orders")));
    QVERIFY(controller->attach(std::move(retired->host)));
    const quint64 retiredGeneration = sessionGeneration(*controller);
    QCOMPARE(retiredGeneration, quint64(1));

    const QString repeatedId = QStringLiteral("same-navigation-id");
    QVERIFY(retired->worker->sendNavigationRequest(
        repeatedId, QStringLiteral("/orders"), 1000));
    QVERIFY(controller->shutdown(QStringLiteral("generation.retired")));
    QVERIFY(!controller->requestRouteLoad(QStringLiteral("/orders")));
    QVERIFY(controller->attach(std::move(current->host)));
    retired->worker->close();
    QTRY_COMPARE_WITH_TIMEOUT(controller->state(),
                              HostWorkerSessionState::Running, 6000);
    QCOMPARE(sessionGeneration(*controller), retiredGeneration + 1);
    QTest::qWait(100);
    QCOMPARE(navigationCount, 0);

    QVERIFY(QMetaObject::invokeMethod(
        controller.get(), "handleNavigationRequest", Qt::DirectConnection,
        Q_ARG(quint64, retiredGeneration), Q_ARG(QString, repeatedId),
        Q_ARG(QString, QStringLiteral("/orders"))));
    QCOMPARE(navigationCount, 0);
    QCOMPARE(current->worker->poll(0).status, SessionStatus::TimedOut);
    QVERIFY(!current->worker->isClosed());

    const QString route = QStringLiteral("/orders");
    QVERIFY(current->worker->sendNavigationRequest(repeatedId, route, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(navigationCount, 1, 3000);
    const SessionReceiveResult navigationResponse = current->worker->receive(1000);
    QCOMPARE(navigationResponse.status, SessionStatus::MessageReady);
    QCOMPARE(navigationResponse.message->type(), ProtocolType::Response);
    QVERIFY(navigationResponse.message->payload()
                .value(QStringLiteral("ok")).toBool(false));
    const SessionReceiveResult routeLoad = current->worker->receive(1000);
    QCOMPARE(routeLoad.status, SessionStatus::MessageReady);
    QCOMPARE(routeLoad.message->type(), ProtocolType::RouteLoad);
    const auto routeAcknowledgement = ProtocolMessage::successResponse(
        routeLoad.message->requestId(),
        QJsonObject{{QStringLiteral("route"), route}});
    QVERIFY(routeAcknowledgement.has_value());
    QVERIFY(current->worker->send(*routeAcknowledgement, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(controller->pendingRouteLoadCount(),
                              qsizetype(0), 3000);
    current->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->hasIoThread(), 6000);
}

void UnifiedNavigationTest::sessionDetachedMarksSafeRebindPoint()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    QSignalSpy detachedSpy(&controller,
                           &HostWorkerSessionController::sessionDetached);

    auto first = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(first.has_value());
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

    QVERIFY(controller.shutdown(QStringLiteral("rebind")));
    QVERIFY(!controller.canAttachImmediately());
    QTRY_COMPARE_WITH_TIMEOUT(detachedSpy.count(), 1, 6000);
    QVERIFY(controller.canAttachImmediately());
    QCOMPARE(detachedSpy.at(0).at(0).toULongLong(), quint64(1));
    firstPeer.join();

    auto replacement = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(replacement.has_value());
    QVERIFY(controller.attach(std::move(replacement->host)));
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QVERIFY(!controller.canAttachImmediately());
    std::thread replacementPeer(
        [worker = std::move(replacement->worker)]() mutable {
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
    QVERIFY(controller.shutdown(QStringLiteral("rebind.done")));
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(),
                              HostWorkerSessionState::Detached, 6000);
    replacementPeer.join();
}

void UnifiedNavigationTest::pageMetadataFromOneSessionUpdatesOnlyOwningTab()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow legacyWindow(routes(server.helpUrl()), server.origin());
    BrowserTabModel model;
    const QString firstTab = model.createTab(
        BrowserTabKind::App, QStringLiteral("First"),
        QStringLiteral("app://pilot/web-shaped-worker/1"));
    const QString secondTab = model.createTab(
        BrowserTabKind::App, QStringLiteral("Second"),
        QStringLiteral("app://pilot/web-shaped-worker/2"));
    QVERIFY(!firstTab.isEmpty());
    QVERIFY(!secondTab.isEmpty());
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    auto firstController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, [](const QString &, const QString &) { return false; });
    auto secondController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, [](const QString &, const QString &) { return false; });
    auto first = authenticatedSessions(appId);
    auto second = authenticatedSessions(appId);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(firstController->attach(std::move(first->host)));
    QVERIFY(secondController->attach(std::move(second->host)));
    QCOMPARE(sessionGeneration(*firstController), quint64(1));
    QCOMPARE(sessionGeneration(*secondController), quint64(1));

    connect(firstController.get(),
            &HostWorkerSessionController::pageMetadataChanged, &model,
            [&model, firstTab, controller = firstController.get()](
                const quint64 generation, const QString &title,
                const QString &) {
                if (generation == sessionGeneration(*controller)) {
                    (void)model.setTitle(firstTab, title);
                }
            });
    connect(secondController.get(),
            &HostWorkerSessionController::pageMetadataChanged, &model,
            [&model, secondTab, controller = secondController.get()](
                const quint64 generation, const QString &title,
                const QString &) {
                if (generation == sessionGeneration(*controller)) {
                    (void)model.setTitle(secondTab, title);
                }
            });
    QSignalSpy firstMetadata(
        firstController.get(),
        &HostWorkerSessionController::pageMetadataChanged);
    QSignalSpy secondMetadata(
        secondController.get(),
        &HostWorkerSessionController::pageMetadataChanged);
    QVERIFY(firstMetadata.isValid());
    QVERIFY(secondMetadata.isValid());

    QVERIFY(first->worker->send(ProtocolMessage::ready()));
    QVERIFY(second->worker->send(ProtocolMessage::ready()));
    const auto metadata = ProtocolMessage::pageMetadata(
        QStringLiteral("First orders"), QStringLiteral("ready"));
    QVERIFY(metadata.has_value());
    QVERIFY(first->worker->send(*metadata, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(firstMetadata.count(), 1, 3000);
    QCOMPARE(secondMetadata.count(), 0);
    QCOMPARE(model.snapshotAt(model.indexOfId(firstTab)).title,
             QStringLiteral("First orders"));
    QCOMPARE(model.snapshotAt(model.indexOfId(secondTab)).title,
             QStringLiteral("Second"));

    first->worker->close();
    second->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!firstController->hasIoThread(), 6000);
    QTRY_VERIFY_WITH_TIMEOUT(!secondController->hasIoThread(), 6000);
}

void UnifiedNavigationTest::navigationCallbackExceptionFailsClosedWithoutAffectingSibling()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow legacyWindow(routes(server.helpUrl()), server.origin());
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    auto throwingController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, [](const QString &, const QString &) -> bool {
            throw std::runtime_error("navigation callback failure");
        });
    auto siblingController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, [](const QString &, const QString &) { return false; });
    auto throwingSession = authenticatedSessions(appId);
    auto siblingSession = authenticatedSessions(appId);
    QVERIFY(throwingSession.has_value());
    QVERIFY(siblingSession.has_value());
    QVERIFY(throwingController->attach(std::move(throwingSession->host)));
    QVERIFY(siblingController->attach(std::move(siblingSession->host)));

    QSignalSpy failures(throwingController.get(),
                        &HostWorkerSessionController::failed);
    QSignalSpy siblingHeartbeats(siblingController.get(),
                                 &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(failures.isValid());
    QVERIFY(siblingHeartbeats.isValid());
    const quint64 throwingGeneration = throwingController->generation();

    bool callbackEscaped = false;
    bool invoked = false;
    try {
        invoked = QMetaObject::invokeMethod(
            throwingController.get(), "handleNavigationRequest",
            Qt::DirectConnection, Q_ARG(quint64, throwingGeneration),
            Q_ARG(QString, QStringLiteral("throwing-navigation")),
            Q_ARG(QString, QStringLiteral("/orders")));
    } catch (...) {
        callbackEscaped = true;
    }

    QVERIFY2(!callbackEscaped,
             "NavigationCallback exceptions must not escape the Qt handler");
    QVERIFY(invoked);
    QCOMPARE(throwingController->state(), HostWorkerSessionState::Failed);
    QCOMPARE(throwingController->lastErrorCode(),
             QStringLiteral("host.worker_session.navigation_callback_failed"));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(failures.at(0).at(0).toString(),
             QStringLiteral("host.worker_session.navigation_callback_failed"));
    QCOMPARE(failures.at(0).at(1).toULongLong(), throwingGeneration);
    QTRY_VERIFY_WITH_TIMEOUT(!throwingController->hasIoThread(), 6000);

    QCOMPARE(siblingController->state(), HostWorkerSessionState::Running);
    QVERIFY(siblingSession->worker->send(ProtocolMessage::heartbeat(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(siblingHeartbeats.count(), 1, 3000);
    QCOMPARE(siblingHeartbeats.at(0).at(0).toULongLong(),
             siblingController->generation());
    siblingSession->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!siblingController->hasIoThread(), 6000);
}

void UnifiedNavigationTest::navigationCallbackMayDestroyOwningController()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow legacyWindow(routes(server.helpUrl()), server.origin());
    const QString appId = QStringLiteral("com.qbrowser.pilot");
    ControllerMetaCallProbe controllerMetaCallProbe;
    auto callbackState = std::make_shared<DestroyingNavigationCallbackState>();
    callbackState->controllerMetaCallProbe = &controllerMetaCallProbe;
    std::unique_ptr<HostWorkerSessionController> owningController;
    callbackState->owner = &owningController;
    owningController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, DestroyingNavigationCallback(callbackState));
    owningController->installEventFilter(&controllerMetaCallProbe);
    QPointer<HostWorkerSessionController> owningGuard(owningController.get());
    auto siblingController = makeSessionController<HostWorkerSessionController>(
        &legacyWindow, [](const QString &, const QString &) { return false; });
    auto owningSession = authenticatedSessions(appId);
    auto siblingSession = authenticatedSessions(appId);
    QVERIFY(owningSession.has_value());
    QVERIFY(siblingSession.has_value());
    QVERIFY(owningController->attach(std::move(owningSession->host)));
    QVERIFY(siblingController->attach(std::move(siblingSession->host)));
    QSignalSpy siblingHeartbeats(siblingController.get(),
                                 &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(siblingHeartbeats.isValid());
    QCoreApplication::sendPostedEvents(owningController.get(), QEvent::MetaCall);
    controllerMetaCallProbe.arm();

    QVERIFY(owningSession->worker->sendNavigationRequest(
        QStringLiteral("destroying-navigation"), QStringLiteral("/orders"), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(callbackState->invocationCount, 1, 3000);
    if (callbackState->invokedFromControllerMetaCallDelivery
        && owningController != nullptr) {
        owningController.reset();
    }
    QVERIFY(owningGuard.isNull());
    QVERIFY(owningController == nullptr);
    QVERIFY2(!callbackState->executingTargetDestroyed,
             "The executing NavigationCallback target was destroyed in-place");

    bool sawNavigationReply = false;
    bool owningSessionClosed = false;
    QElapsedTimer closeWait;
    closeWait.start();
    while (closeWait.elapsed() < 3000 && !owningSessionClosed) {
        const SessionReceiveResult received = owningSession->worker->poll(0);
        if (received.status == SessionStatus::MessageReady
            && received.message.has_value()) {
            sawNavigationReply = sawNavigationReply
                || received.message->type() == ProtocolType::Response
                || received.message->type() == ProtocolType::RouteLoad;
        } else if (received.status == SessionStatus::PeerClosed
                   || received.status == SessionStatus::Failed) {
            owningSessionClosed = true;
        }
        if (!owningSessionClosed) QTest::qWait(10);
    }
    QVERIFY(!sawNavigationReply);
    QVERIFY(owningSessionClosed);

    QCOMPARE(siblingController->state(), HostWorkerSessionState::Running);
    QVERIFY(siblingSession->worker->send(ProtocolMessage::heartbeat(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(siblingHeartbeats.count(), 1, 3000);
    QCOMPARE(siblingHeartbeats.at(0).at(0).toULongLong(),
             siblingController->generation());
    siblingSession->worker->close();
    QTRY_VERIFY_WITH_TIMEOUT(!siblingController->hasIoThread(), 6000);
    QVERIFY2(!callbackState->invokedFromControllerMetaCallDelivery,
             "NavigationCallback ran while its owning controller was the "
             "QMetaCallEvent receiver");
    QVERIFY2(callbackState->ownerDestroyedByCallback,
             "NavigationCallback did not synchronously destroy its owner");
    QCOMPARE(controllerMetaCallProbe.deliveryCount(), 0);
}

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
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(workerRouteSpy.count(), 1);
    QCOMPARE(workerRouteSpy.first().at(1).toString(), QStringLiteral("qml/Main.qml"));
    QCOMPARE(workerRouteSpy.first().at(2).toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("order 42"));
    QCOMPARE(window.surfaceStack()->currentWidget(), workerSurface);
    QVERIFY(workerSurface->isVisible());
    QCOMPARE(window.webSurface(), nullptr);

    const int workerCurrentUrlSignals = currentUrlSpy.count();
    address->setText(QStringLiteral("https://example.com/not-an-app-route"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(address->text(), workerAppUrl);
    QVERIFY(backButton->isEnabled());
    QVERIFY(!forwardButton->isEnabled());
    QCOMPARE(currentUrlSpy.count(), workerCurrentUrlSignals);

    QVERIFY(window.navigate(workerAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(window.historyIndex(), 1);

    const QString webAppUrl = QStringLiteral("app://pilot/worker-shaped-web");
    QVERIFY(window.navigate(webAppUrl));
    QSignalSpy webNavigationSpy(window.webSurface(), &WebSurface::navigationFinished);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.activeSurfaceCount(), 1);
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
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
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
    QVERIFY(window.trustedErrorText().contains(QStringLiteral("not found"),
                                               Qt::CaseInsensitive));
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QVERIFY(window.webSurface() != nullptr);
    QVERIFY(!window.webSurface()->page()->isVisible());

    QVERIFY(window.goBack());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 1);
    QVERIFY(window.goBack());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Host);
    QCOMPARE(window.currentAppUrl(), QStringLiteral("qbrowser://newtab"));
    QCOMPARE(window.historyIndex(), 0);
    QCOMPARE(window.webSurface(), nullptr);
    QVERIFY(!window.goBack());
    QVERIFY(window.goForward());
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyIndex(), 1);

    address->setText(missingAppUrl);
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(address->text(), workerAppUrl);

    const int stableHistoryCount = window.historyCount();
    address->setText(QStringLiteral("https://example.com/"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.historyCount(), stableHistoryCount);
    QCOMPARE(window.currentAppUrl(), workerAppUrl);
    QCOMPARE(address->text(), workerAppUrl);
    QVERIFY(currentUrlSpy.count() >= 5);

    window.close();
    launch->hostSession.close();
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void UnifiedNavigationTest::tabKeyedWorkerSurfacesStayIndependent()
{
    HelpServer server;
    QVERIFY(server.listen());
    WorkerTestEnvironment firstEnvironment;
    WorkerTestEnvironment secondEnvironment;
    QVERIFY2(firstEnvironment.isValid(), qPrintable(firstEnvironment.error()));
    QVERIFY2(secondEnvironment.isValid(), qPrintable(secondEnvironment.error()));

    auto firstLaunch = firstEnvironment.launch(QStringLiteral("tab-one"),
                                               QStringLiteral("tab-one"), 100);
    auto secondLaunch = secondEnvironment.launch(QStringLiteral("tab-two"),
                                                 QStringLiteral("tab-two"), 100);
    QVERIFY2(firstLaunch.has_value(), qPrintable(firstEnvironment.error()));
    QVERIFY2(secondLaunch.has_value(), qPrintable(secondEnvironment.error()));
    QCOMPARE(receiveUntil(firstLaunch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto firstSurfaceReady = receiveUntil(firstLaunch->hostSession,
                                                ProtocolType::SurfaceReady);
    QCOMPARE(firstSurfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(firstLaunch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(secondLaunch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto secondSurfaceReady = receiveUntil(secondLaunch->hostSession,
                                                 ProtocolType::SurfaceReady);
    QCOMPARE(secondSurfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(secondLaunch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);

    auto firstSurface = std::unique_ptr<WorkerSurface>(WorkerSurface::create(
        firstSurfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        firstLaunch->process.nativeProcessHandle(), WorkerAttemptId{201}));
    auto secondSurface = std::unique_ptr<WorkerSurface>(WorkerSurface::create(
        secondSurfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        secondLaunch->process.nativeProcessHandle(), WorkerAttemptId{202}));
    QVERIFY(firstSurface != nullptr);
    QVERIFY(secondSurface != nullptr);

    MainWindow window(routes(server.helpUrl()), server.origin());
    const QString firstId = window.tabModel()->activeId();
    QVERIFY(window.attachWorkerSurface(firstId, std::move(firstSurface)));
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/one")));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString secondId = window.tabModel()->activeId();
    QVERIFY(secondId != firstId);
    QVERIFY(window.attachWorkerSurface(secondId, std::move(secondSurface)));
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/two")));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QVERIFY(window.workerSurface(firstId) != nullptr);
    QVERIFY(window.workerSurface(secondId) != nullptr);

    const int firstHistory = window.tabModel()->snapshotAt(
        window.tabModel()->indexOfId(firstId)).history.size();
    QVERIFY(window.navigateFromWorker(firstId, QStringLiteral("com.qbrowser.pilot"),
                                      "/orders"));
    QCOMPARE(window.tabModel()->snapshotAt(window.tabModel()->indexOfId(firstId))
                 .history.size(),
             firstHistory + 1);
    QCOMPARE(window.tabModel()->snapshotAt(window.tabModel()->indexOfId(secondId))
                 .history.size(),
             2);
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/web-shaped-worker/two"));
    window.close();
    firstLaunch->hostSession.close();
    secondLaunch->hostSession.close();
    firstLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    secondLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(firstLaunch->process.waitForFinished(5000));
    QVERIFY(secondLaunch->process.waitForFinished(5000));
    const auto firstClosed = firstLaunch->process.close();
    const auto secondClosed = secondLaunch->process.close();
    QVERIFY2(firstClosed.value.has_value(), qPrintable(firstClosed.errorCode));
    QVERIFY2(secondClosed.value.has_value(), qPrintable(secondClosed.errorCode));
}

void UnifiedNavigationTest::packageNavigationPublishesTabLaunchRequest()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    window.setPackageRuntimeEnabled(true);
    QSignalSpy launchSpy(&window, &MainWindow::appLaunchRequested);

    const QString tabId = window.tabModel()->activeId();
    QVERIFY(window.navigate(QStringLiteral("app://pilot/orders")));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(launchSpy.count(), 1);
    QCOMPARE(launchSpy.at(0).at(0).toString(), tabId);
    QCOMPARE(launchSpy.at(0).at(2).toString(),
             QStringLiteral("com.qbrowser.pilot"));
    QCOMPARE(launchSpy.at(0).at(3).toString(), QStringLiteral("/orders"));
    QVERIFY(window.workerSurface(tabId) == nullptr);
    window.close();
}

void UnifiedNavigationTest::legacyWorkerAdapterRejectsASecondAppTab()
{
    HelpServer server;
    QVERIFY(server.listen());

    WorkerTestEnvironment workerEnvironment;
    QVERIFY2(workerEnvironment.isValid(), qPrintable(workerEnvironment.error()));
    auto launch = workerEnvironment.launch(QStringLiteral("legacy-owner-nonce"),
                                           QStringLiteral("legacy-owner-nonce"),
                                           100);
    QVERIFY2(launch.has_value(), qPrintable(workerEnvironment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(
        launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);

    WorkerSurface *const workerSurface = WorkerSurface::create(
        surfaceReady.message->payload()
            .value(QStringLiteral("windowHandle"))
            .toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{102});
    QVERIFY(workerSurface != nullptr);

    MainWindow window(routes(server.helpUrl()), server.origin(), workerSurface);
    QSignalSpy workerRouteSpy(&window, &MainWindow::workerRouteRequested);
    const QString ownerId = window.tabModel()->activeId();
    const QString firstRoute = QStringLiteral(
        "app://pilot/web-shaped-worker/owner");
    QVERIFY(window.navigate(firstRoute));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.workerSurface(), workerSurface);
    QCOMPARE(workerRouteSpy.count(), 1);

    window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
    const QString rejectedId = window.tabModel()->activeId();
    QVERIFY(rejectedId != ownerId);
    QVERIFY(!window.navigate(firstRoute));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.workerSurface(), nullptr);
    QCOMPARE(workerRouteSpy.count(), 1);
    QCOMPARE(window.tabController(ownerId)->workerSurface(), workerSurface);
    QCOMPARE(window.tabController(ownerId)->surfaceKind(),
             HostSurfaceKind::Worker);

    window.browserChrome()->tabBar()->setCurrentIndex(
        window.tabModel()->indexOfId(ownerId));
    QTRY_COMPARE_WITH_TIMEOUT(window.tabModel()->activeId(), ownerId, 2'000);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Worker);
    QCOMPARE(window.workerSurface(), workerSurface);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/orders")));
    QCOMPARE(window.workerSurface(), workerSurface);
    QCOMPARE(workerRouteSpy.count(), 2);
    QCOMPARE(workerRouteSpy.last().at(1).toString(),
             QStringLiteral("qml/Main.qml"));

    bool retirementObserved = false;
    bool retirementSawClosingController = false;
    bool retirementReentryChangedModel = false;
    connect(&window, &MainWindow::legacyWorkerRetirementRequested, this,
            [&](const QString &retiringId) {
                retirementObserved = retiringId == ownerId;
                TabController *const retiring = window.tabController(ownerId);
                retirementSawClosingController = retiring != nullptr
                    && retiring->lifecycle() == BrowserTabLifecycle::Closing;
                const int tabCountBeforeReentry = window.tabModel()->count();
                window.browserChrome()->dispatchCommand(BrowserCommand::NewTab);
                retirementReentryChangedModel =
                    window.tabModel()->count() != tabCountBeforeReentry;
            }, Qt::DirectConnection);
    window.browserChrome()->dispatchCommand(BrowserCommand::CloseTab);
    QVERIFY(retirementObserved);
    QVERIFY(retirementSawClosingController);
    QVERIFY(!retirementReentryChangedModel);
    QCOMPARE(window.tabModel()->count(), 1);
    QCOMPARE(window.tabController(ownerId), nullptr);

    window.close();
    launch->hostSession.close();
    launch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void UnifiedNavigationTest::legacyWorkerDetachDuringRouteStartWinsTransition()
{
    HelpServer server;
    QVERIFY(server.listen());
    WorkerTestEnvironment workerEnvironment;
    QVERIFY2(workerEnvironment.isValid(), qPrintable(workerEnvironment.error()));
    auto launch = workerEnvironment.launch(QStringLiteral("legacy-detach-nonce"),
                                           QStringLiteral("legacy-detach-nonce"),
                                           100);
    QVERIFY2(launch.has_value(), qPrintable(workerEnvironment.error()));
    [[maybe_unused]] const auto cleanup = qScopeGuard([&] {
        launch->hostSession.close();
        if (launch->process.isValid()) {
            launch->process.terminate(ERROR_PROCESS_ABORTED);
            (void)launch->process.waitForFinished(5'000);
            (void)launch->process.close();
        }
    });
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    const auto surfaceReady = receiveUntil(
        launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);

    WorkerSurface *const workerSurface = WorkerSurface::create(
        surfaceReady.message->payload()
            .value(QStringLiteral("windowHandle"))
            .toString(),
        launch->process.nativeProcessHandle(), WorkerAttemptId{104});
    QVERIFY(workerSurface != nullptr);
    MainWindow window(routes(server.helpUrl()), server.origin(), workerSurface);
    const QString tabId = window.tabModel()->activeId();
    TabController *const controller = window.tabController(tabId);
    QVERIFY(controller != nullptr);
    bool detachObserved = false;
    connect(&window, &MainWindow::workerRouteRequested, this,
            [&](const QString &, const QString &, const QVariantMap &,
                const QUrl &) {
                detachObserved = true;
                window.detachWorkerSurface();
            }, Qt::DirectConnection);

    QVERIFY(!window.navigate(QStringLiteral("app://pilot/orders")));
    QVERIFY(detachObserved);
    QCOMPARE(window.workerSurface(), nullptr);
    QCOMPARE(controller->workerSurface(), nullptr);
    QCOMPARE(controller->surfaceKind(), HostSurfaceKind::TrustedError);
    QCOMPARE(controller->lifecycle(), BrowserTabLifecycle::TrustedError);
    QCOMPARE(window.tabModel()->lifecycleAt(
                 window.tabModel()->indexOfId(tabId)),
             BrowserTabLifecycle::TrustedError);
    QVERIFY(!window.trustedErrorText().isEmpty());
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
    QCOMPARE(window.historyCount(), 2);
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QTRY_COMPARE_WITH_TIMEOUT(window.currentAppUrl(), QStringLiteral("app://pilot/orders"),
                              5000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.pendingRouteLoadCount(), qsizetype(0), 5000);
    QCOMPARE(routeLoadSpy.count(), 1);
    QCOMPARE(routeLoadSpy.at(0).at(0).toString(), QStringLiteral("/orders"));
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);
    QCOMPARE(window.currentAppUrl(), QStringLiteral("app://pilot/orders"));
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
    QVERIFY(!window.navigateFromWorker(QStringLiteral("com.qbrowser.other"),
                                       QStringLiteral("/orders")));
    QVERIFY(!window.navigateFromWorker(workerEnvironment.appId(),
                                       QStringLiteral("https://evil.test/orders")));
    QVERIFY(!window.navigateFromWorker(workerEnvironment.appId(),
                                       QStringLiteral("/worker-shaped-web")));
    QCOMPARE(window.historyCount(), 3);
    QVERIFY(window.goBack());
    QVERIFY(controller.requestRouteLoad(
        QStringLiteral("/web-shaped-worker/42")));
    QTRY_COMPARE_WITH_TIMEOUT(routeLoadSpy.count(), 2, 5000);
    QCOMPARE(routeLoadSpy.at(1).at(0).toString(),
             QStringLiteral("/web-shaped-worker/42"));
    QCOMPARE(window.currentAppUrl(), initial);
    QVERIFY(window.goForward());
    QVERIFY(controller.requestRouteLoad(QStringLiteral("/orders")));
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
    HostWorkerSessionController controller(
        [&window](const QString &appId, const QString &route) {
            return window.navigateFromWorker(appId, route);
        });
    QVERIFY(controller.attach(std::make_unique<IpcSession>(
        std::move(launch->hostSession))));
    QSignalSpy acknowledged(&controller,
                            &HostWorkerSessionController::routeLoadAcknowledged);

    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/7")));
    QVERIFY(controller.requestRouteLoad(
        QStringLiteral("/web-shaped-worker/7")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 1, 5000);
    QCOMPARE(acknowledged.at(0).at(0).toString(),
             QStringLiteral("/web-shaped-worker/7"));
    QVERIFY(window.navigate(QStringLiteral("app://pilot/orders")));
    QVERIFY(controller.requestRouteLoad(QStringLiteral("/orders")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 2, 5000);
    QCOMPARE(acknowledged.at(1).at(0).toString(), QStringLiteral("/orders"));
    QVERIFY(window.goBack());
    QVERIFY(controller.requestRouteLoad(
        QStringLiteral("/web-shaped-worker/7")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 3, 5000);
    QCOMPARE(acknowledged.at(2).at(0).toString(),
             QStringLiteral("/web-shaped-worker/7"));
    QVERIFY(window.goForward());
    QVERIFY(controller.requestRouteLoad(QStringLiteral("/orders")));
    QTRY_COMPARE_WITH_TIMEOUT(acknowledged.count(), 4, 5000);
    QCOMPARE(acknowledged.at(3).at(0).toString(), QStringLiteral("/orders"));

    bool reentered = false;
    const QMetaObject::Connection reentry = connect(
        &controller, &HostWorkerSessionController::routeLoadAcknowledged,
        &window, [&](const QString &route) {
            if (!reentered && route == QStringLiteral("/web-shaped-worker/9")) {
                reentered = true;
                QVERIFY(window.goBack());
                QVERIFY(controller.requestRouteLoad(QStringLiteral("/orders")));
            }
        }, Qt::DirectConnection);
    QVERIFY(window.navigate(QStringLiteral("app://pilot/web-shaped-worker/9")));
    QVERIFY(controller.requestRouteLoad(
        QStringLiteral("/web-shaped-worker/9")));
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

void UnifiedNavigationTest::capabilityPendingKeepsHeartbeatAndBoundsSecondRequest()
{
    HelpServer server;
    QVERIFY(server.listen());
    MainWindow window(routes(server.helpUrl()), server.origin());
    HostWorkerSessionController controller(&window);
    auto sessions = authenticatedSessions(QStringLiteral("com.qbrowser.pilot"));
    QVERIFY(sessions.has_value());

    QTcpServer silentServer;
    QVERIFY(silentServer.listen(QHostAddress::LocalHost));
    const QUrl origin(QStringLiteral("http://127.0.0.1:%1/")
                          .arg(silentServer.serverPort()));
    ManifestPermissions permissions;
    permissions.network.hosts = {QStringLiteral("127.0.0.1")};
    permissions.network.methods = {QStringLiteral("GET")};
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    auto router = HostGestureRouter::createForTesting(1);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority capabilityAuthority{
        QStringLiteral("tab-capability-pending"), 1,
        QStringLiteral("com.qbrowser.pilot"), 41, 401,
        controller.generation() + 1, 11};
    auto admissionToken = std::make_shared<AuthorityAdmissionToken>();
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        capabilityAuthority, admissionToken, router.get(), permissions, origin,
        storage.path(), 1, &error, nullptr);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    auto cleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(runtime, {}));
        if (sessions.has_value() && sessions->worker != nullptr)
            sessions->worker->close();
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(runtime->isWorkerInitializationComplete(), 5'000);
    QVERIFY2(runtime->isWorkerReady(),
             qPrintable(runtime->workerInitializationError()));
    QVERIFY(controller.attach(std::move(sessions->host), runtime.get()));
    QSignalSpy heartbeats(&controller,
                          &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(heartbeats.isValid());

    const auto slow = ProtocolMessage::request(
        QStringLiteral("slow"), QStringLiteral("network"),
        QStringLiteral("request"),
        QJsonObject{{QStringLiteral("method"), QStringLiteral("GET")},
                    {QStringLiteral("url"),
                     origin.resolved(QUrl(QStringLiteral("api/dashboard")))
                         .toString(QUrl::FullyEncoded)},
                    {QStringLiteral("bodyBase64"), QString{}}});
    QVERIFY(slow.has_value());
    QVERIFY(sessions->worker->sendRequest(
        slow->requestId(), QStringLiteral("network"), QStringLiteral("request"),
        slow->payload().value(QStringLiteral("payload")).toObject(), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(controller.pendingCapabilityCount(), qsizetype(1),
                              3000);
    QVERIFY(sessions->worker->send(ProtocolMessage::heartbeat(), 1000));
    const auto second = ProtocolMessage::request(
        QStringLiteral("second"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{{QStringLiteral("key"),
                                             QStringLiteral("bounded")}});
    QVERIFY(second.has_value());
    QVERIFY(sessions->worker->sendRequest(
        second->requestId(), QStringLiteral("storage"), QStringLiteral("get"),
        second->payload().value(QStringLiteral("payload")).toObject(), 1000));
    QTRY_VERIFY_WITH_TIMEOUT(heartbeats.count() > 0, 3000);
    QCOMPARE(heartbeats.constLast().at(0).toULongLong(),
             controller.generation());

    std::optional<ProtocolMessage> busy;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        const SessionReceiveResult received = sessions->worker->poll(0);
        if (received.status == SessionStatus::MessageReady
            && received.message.has_value()
            && received.message->type() == ProtocolType::Response
            && received.message->requestId() == QStringLiteral("second")) {
            busy = *received.message;
        }
        return busy.has_value();
    })(), 3000);
    QVERIFY(!busy->payload().value(QStringLiteral("ok")).toBool(true));
    QCOMPARE(busy->payload().value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("capability.busy"));
    QCOMPARE(controller.pendingCapabilityCount(), qsizetype(1));

    controller.unbindCapabilityRuntime(capabilityAuthority);
    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    sessions->worker->close();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), HostWorkerSessionState::Failed,
                              3000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasIoThread(), 6000);
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    cleanup.dismiss();
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
    std::unique_ptr<WorkerSurface> surface(WorkerSurface::create(
        surfaceReady.message->payload().value(QStringLiteral("windowHandle")).toString(),
        process->nativeProcessHandle(), WorkerAttemptId{104}));
    QVERIFY(surface != nullptr);
    WorkerSurface *const surfacePointer = surface.get();

    QTemporaryDir authority;
    QTemporaryDir hostFixture;
    QVERIFY(authority.isValid());
    QVERIFY(hostFixture.isValid());
    const QString store = authority.filePath(QStringLiteral("store"));
    const QString sandbox = authority.filePath(QStringLiteral("sandbox"));
    const QString telemetry = authority.filePath(QStringLiteral("telemetry"));
    const QString storage = authority.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(store));
    QVERIFY(QDir().mkpath(sandbox));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    const QString deployment = hostFixture.filePath(
        QStringLiteral("deployment"));
    const QString browserState = hostFixture.filePath(
        QStringLiteral("browser-state"));
    const QString runtime = QDir(deployment).filePath(QStringLiteral("runtime"));
    const QString hostDirectory = QDir(deployment).filePath(QStringLiteral("host"));
    const QString trustDirectory = QDir(deployment).filePath(QStringLiteral("trust"));
    const QString packagesDirectory = QDir(deployment).filePath(
        QStringLiteral("packages"));
    QVERIFY(QDir().mkpath(browserState));
    QVERIFY(QDir().mkpath(hostDirectory));
    QVERIFY(QDir().mkpath(trustDirectory));
    QVERIFY(QDir().mkpath(packagesDirectory));
    QVERIFY(copyPlainTree(environment.runtimeRoot(), runtime));
    const QString workerRelative = QDir(environment.runtimeRoot())
        .relativeFilePath(environment.workerExecutable());
    const QString stagedWorker = QDir(runtime).filePath(workerRelative);
    QVERIFY(QFileInfo(stagedWorker).isFile());
    const QString sourceHost = currentExecutablePath();
    QVERIFY(!sourceHost.isEmpty());
    const QString stagedHost = QDir(hostDirectory).filePath(
        QFileInfo(sourceHost).fileName());
    QVERIFY(QFile::copy(sourceHost, stagedHost));
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = QDir(trustDirectory).filePath(
        QStringLiteral("trusted.pem"));
    QFile keyFile(publicKey);
    QVERIFY(keyFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(keyFile.write(keys.value().publicKeyPem),
             keys.value().publicKeyPem.size());
    keyFile.close();
#ifdef Q_OS_WIN
    const QStringList protectedPaths{deployment, browserState, runtime,
                                     QFileInfo(stagedWorker).absolutePath(),
                                     stagedWorker, hostDirectory, stagedHost,
                                     trustDirectory, publicKey};
    for (const QString &path : protectedPaths) {
        QVERIFY(protectPath(path, QFileInfo(path).isDir()));
    }
#endif
    HostRuntimeParseContext parseContext;
    parseContext.currentHostExecutable = HostOwnedFileAuthority::open(stagedHost);
    QVERIFY(parseContext.currentHostExecutable);
    const HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments({
        QStringLiteral("--package-mode"),
        QStringLiteral("--mock-origin=")
            + server.origin().toString(QUrl::FullyEncoded),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + store,
        QStringLiteral("--sandbox-temp=") + sandbox,
        QStringLiteral("--runtime-root=") + runtime,
        QStringLiteral("--worker-executable=") + stagedWorker,
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--storage-directory=") + storage,
        QStringLiteral("--deployment-root=") + deployment,
        QStringLiteral("--browser-state-directory=") + browserState,
    }, parseContext);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));
    HostApplication application(std::move(*parsed.value));
    QVERIFY(application.start());
    HostWorkerAttachContext context;
    context.session = std::make_unique<IpcSession>(std::move(launch->hostSession));
    context.surface = std::move(surface);
    context.processLifetime = process;
    context.launchRequest = WorkerLaunchRequest{
        application.mainWindow()->tabModel()->activeId(),
        91,
        QStringLiteral("/"),
        VerifiedPackageLease{
            environment.appId(),
            QStringLiteral("1.0.0"),
            QStringLiteral("1.0.0-") + QString(64, QLatin1Char('a')),
            store,
            QStringLiteral("qml/Main.qml"),
            {},
            QByteArray(64, 'a'),
            1,
            92},
        std::make_shared<AuthorityAdmissionToken>(),
        WorkerAttemptKey{WorkerActivationId{1}, WorkerAttemptId{104}},
        PackageRevalidationMode::CurrentActivation,
        false};
    context.processId = process->processId();
    context.stopProcess = [process] { process->terminate(ERROR_PROCESS_ABORTED); };
    QCOMPARE(application.attachWorkerContextForTesting(std::move(context)),
             InstalledPackageWorkerLauncher::AttachResult::Attached);
    QTRY_VERIFY_WITH_TIMEOUT(application.hasWorkerContext(), 5'000);
    QCOMPARE(application.mainWindow()->workerSurface(), surfacePointer);
    QCOMPARE(application.workerSessionController()->state(),
             HostWorkerSessionState::Running);

    QSignalSpy retirementRequested(
        application.mainWindow(),
        &MainWindow::legacyWorkerRetirementRequested);
    QVERIFY(application.mainWindow()->shutdown());
    QCOMPARE(retirementRequested.count(), 1);
    QVERIFY(!application.hasWorkerContext());
    QCOMPARE(application.mainWindow()->workerSurface(), nullptr);
    QVERIFY(process->waitForFinished(5000));
    const auto closed = process->close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
    application.mainWindow()->close();
}

void UnifiedNavigationTest::failedWorkerContextAttachmentConsumesSurfaceExactlyOnce()
{
    HelpServer server;
    QVERIFY(server.listen());
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));

    const auto runFailure = [&] {
        auto launch = environment.launch(
            QStringLiteral("session-failure"),
            QStringLiteral("session-failure"),
            100);
        QVERIFY2(launch.has_value(), qPrintable(environment.error()));
        QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
                 SessionStatus::MessageReady);
        const auto surfaceReady = receiveUntil(
            launch->hostSession, ProtocolType::SurfaceReady);
        QCOMPARE(surfaceReady.status, SessionStatus::MessageReady);
        QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
                 SessionStatus::MessageReady);
        auto process = std::make_shared<SandboxProcess>(std::move(launch->process));
        std::unique_ptr<WorkerSurface> surface(WorkerSurface::create(
            surfaceReady.message->payload()
                .value(QStringLiteral("windowHandle")).toString(),
            process->nativeProcessHandle(), WorkerAttemptId{105}));
        QVERIFY(surface != nullptr);
        int destroyed = 0;
        connect(surface.get(), &QObject::destroyed, this,
                [&destroyed] { ++destroyed; });

        HostApplication application(server.origin());
        QVERIFY(application.start());
        HostWorkerAttachContext context;
        context.session = std::make_unique<IpcSession>(
            WinPipeTransport{}, IpcRole::Host,
            HostLaunchContext{QStringLiteral("invalid"),
                              QStringLiteral("com.qbrowser.pilot")});
        context.surface = std::move(surface);
        context.processLifetime = process;
        context.stopProcess = [process] {
            process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        };
        QCOMPARE(application.attachWorkerContextForTesting(std::move(context)),
                 InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure);
        QCOMPARE(destroyed, 1);
        QCOMPARE(application.mainWindow()->workerSurface(), nullptr);
        QVERIFY(!application.hasWorkerContext());
        process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
        QVERIFY(process->waitForFinished(5000));
        const auto closed = process->close();
        QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
        application.mainWindow()->close();
    };

    runFailure();
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
    QCOMPARE(window.historyCount(), 2);

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
                && window.historyCount() == 3
                && window.historyIndex() == 2
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantNavigateResult = window.navigate(unavailableWorkerAppUrl);
        },
        Qt::DirectConnection);

    QVERIFY(!window.navigate(unavailableWorkerAppUrl));
    QVERIFY(navigateReceiverRan);
    QVERIFY(navigateReceiverSawCommittedState);
    QVERIFY(!reentrantNavigateResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 3);
    QCOMPARE(window.historyIndex(), 2);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    disconnect(navigateConnection);

    const int historyBeforeDuplicate = window.historyCount();
    QVERIFY(!window.navigate(unavailableWorkerAppUrl));
    QCOMPARE(window.historyCount(), historyBeforeDuplicate);
    QCOMPARE(window.historyIndex(), historyBeforeDuplicate - 1);

    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.historyCount(), 4);
    QCOMPARE(window.historyIndex(), 3);

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
                && window.historyCount() == 4
                && window.historyIndex() == 2
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantBackResult = window.goBack();
        },
        Qt::DirectConnection);

    QVERIFY(window.goBack());
    QVERIFY(backReceiverRan);
    QVERIFY(backReceiverSawCommittedState);
    QVERIFY(!reentrantBackResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 4);
    QCOMPARE(window.historyIndex(), 2);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    disconnect(backConnection);

    QVERIFY(window.goBack());
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);

    QSignalSpy invalidInputUrlSpy(&window, &MainWindow::currentUrlChanged);
    address->setText(QStringLiteral("file:///C:/Windows/win.ini"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(window.activeSurface(), HostSurfaceKind::TrustedError);
    QCOMPARE(window.surfaceStack()->currentWidget()->objectName(),
             QStringLiteral("trusted-error-surface"));
    QCOMPARE(window.currentAppUrl(), webAppUrl);
    QCOMPARE(window.historyCount(), 4);
    QCOMPARE(window.historyIndex(), 1);
    QCOMPARE(address->text(), webAppUrl);
    QVERIFY(backButton->isEnabled());
    QVERIFY(forwardButton->isEnabled());
    QCOMPARE(invalidInputUrlSpy.count(), 0);

    QVERIFY(window.navigate(webAppUrl));
    QCOMPARE(window.activeSurface(), HostSurfaceKind::Web);
    QCOMPARE(window.historyCount(), 4);
    QCOMPARE(window.historyIndex(), 1);
    QVERIFY(backButton->isEnabled());
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
                && window.historyCount() == 4
                && window.historyIndex() == 2
                && window.activeSurface() == HostSurfaceKind::TrustedError;
            reentrantForwardResult = window.goForward();
        },
        Qt::DirectConnection);

    QVERIFY(window.goForward());
    QVERIFY(forwardReceiverRan);
    QVERIFY(forwardReceiverSawCommittedState);
    QVERIFY(!reentrantForwardResult);
    QCOMPARE(window.currentAppUrl(), unavailableWorkerAppUrl);
    QCOMPARE(window.historyCount(), 4);
    QCOMPARE(window.historyIndex(), 2);
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
