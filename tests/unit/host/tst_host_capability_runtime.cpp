#include "AuthorityAdmissionToken.h"
#include "BrowserCommand.h"
#include "HostApplication.h"
#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "MainWindow.h"
#include "TabCapabilityAuthority.h"
#include "WorkerRetirementManager.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <chrono>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace {
ManifestPermissions networkPermission(const quint16 port)
{
    Q_UNUSED(port)
    ManifestPermissions permissions;
    permissions.network.hosts = {QStringLiteral("127.0.0.1")};
    permissions.network.methods = {QStringLiteral("GET")};
    return permissions;
}

ManifestPermissions clipboardPermission()
{
    ManifestPermissions permissions;
    permissions.clipboardRead = ClipboardReadPermission::UserGesture;
    return permissions;
}

ManifestPermissions clipboardAndStoragePermissions()
{
    ManifestPermissions permissions = clipboardPermission();
    permissions.storage = StoragePermission::AppPrivate;
    return permissions;
}

TabCapabilityAuthority authority(
    const QString &tabId,
    const quint64 runtimeIncarnation,
    const quint32 workerProcessId,
    const quintptr workerWindowId,
    const quint64 sessionGeneration,
    const quint64 leaseAuthorityEpoch,
    const QString &appIdentity = QStringLiteral("com.qbrowser.same"))
{
    return {tabId, runtimeIncarnation, appIdentity, workerProcessId,
            workerWindowId, sessionGeneration, leaseAuthorityEpoch};
}

HostGestureSystemEvidence validEvidence(
    const TabCapabilityAuthority &binding, const quint32 now)
{
    return {now, binding.workerWindowId, binding.workerProcessId,
            binding.workerProcessId, true, true};
}

qint64 monotonicDeadlineAfter(const std::chrono::milliseconds delay)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() + delay)
        .count();
}

QJsonObject requestPayload(const QUrl &url)
{
    return {{QStringLiteral("method"), QStringLiteral("GET")},
            {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("bodyBase64"), QString{}}};
}
}

class HostCapabilityRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rejectsNonCanonicalMockOrigins_data();
    void rejectsNonCanonicalMockOrigins();
    void validatesTrustedWorkerGestureEvidence();
    void clipboardRuntimesCanCoexist();
    void onlyActiveAuthorityCanIssueAndConsumeGesture();
    void authorityTransitionsRevokeUnconsumedEvidence();
    void mainWindowDeactivationRevokesGestureEvidence();
    void backgroundFocusAndDispatchStateCannotAuthorize();
    void browserCommandIsSuppressedAndNeverBecomesGesture();
    void staleQueuedBrowserCommandIsDropped_data();
    void staleQueuedBrowserCommandIsDropped();
    void heldBrowserChordRemainsSuppressedAcrossBindingSwitch();
    void revocationClosesUseAndPublicationUntilGuardDrains();
    void retiringRuntimeLeavesSiblingGestureStorageAndCompletionActive();
    void repeatedRetireClaimsWorkerExactlyOnce();
    void slowRequestRetiresWithoutBlockingOrLateDelivery();
};

void HostCapabilityRuntimeTest::initTestCase()
{
    qRegisterMetaType<BrokerResult>();
    qRegisterMetaType<BrowserCommand>();
    qRegisterMetaType<TabCapabilityAuthority>();
}

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins_data()
{
    QTest::addColumn<QUrl>("origin");
    QTest::newRow("https") << QUrl(QStringLiteral("https://127.0.0.1:8443/"));
    QTest::newRow("hostname") << QUrl(QStringLiteral("http://localhost:8080/"));
    QTest::newRow("missing-port") << QUrl(QStringLiteral("http://127.0.0.1/"));
    QTest::newRow("path") << QUrl(QStringLiteral("http://127.0.0.1:8080/api"));
    QTest::newRow("query") << QUrl(QStringLiteral("http://127.0.0.1:8080/?x=1"));
    QTest::newRow("fragment") << QUrl(QStringLiteral("http://127.0.0.1:8080/#x"));
    QTest::newRow("userinfo") << QUrl(QStringLiteral("http://user@127.0.0.1:8080/"));
}

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins()
{
    QFETCH(QUrl, origin);
    QString error;
    const auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"), {}, origin,
        QStringLiteral("L:/not-opened-for-invalid-config"), 0, 0, 0, &error);
    QVERIFY(runtime == nullptr);
    QCOMPARE(error, QStringLiteral("host.capability.invalid_configuration"));
}

void HostCapabilityRuntimeTest::validatesTrustedWorkerGestureEvidence()
{
    HostWorkerGestureEvidence valid;
    valid.now = 5'000;
    valid.trustedWorkerInput = 4'500;
    valid.lastSystemInput = 4'900;
    valid.workerProcessId = 42;
    valid.focusProcessId = 42;
    valid.foregroundMatchesHostRoot = true;
    valid.focusBelongsToWorkerWindow = true;
    QVERIFY(qbrowser_host_testing::isTrustedWorkerGesture(valid));

    HostWorkerGestureEvidence noGesture = valid;
    noGesture.trustedWorkerInput = 0;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(noGesture));
    HostWorkerGestureEvidence unrelatedRecentInput = valid;
    unrelatedRecentInput.trustedWorkerInput = 0;
    unrelatedRecentInput.lastSystemInput = 4'999;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(
        unrelatedRecentInput));
    HostWorkerGestureEvidence expired = valid;
    expired.trustedWorkerInput = 3'999;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(expired));
    HostWorkerGestureEvidence replay = valid;
    replay.lastGrantedInput = replay.trustedWorkerInput;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(replay));
    HostWorkerGestureEvidence crossApp = valid;
    crossApp.focusProcessId = 43;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossApp));
    HostWorkerGestureEvidence crossWindow = valid;
    crossWindow.focusBelongsToWorkerWindow = false;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossWindow));
}

void HostCapabilityRuntimeTest::clipboardRuntimesCanCoexist()
{
    QTemporaryDir firstStorage;
    QTemporaryDir secondStorage;
    QVERIFY(firstStorage.isValid());
    QVERIFY(secondStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(1);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority firstAuthority = authority(
        QStringLiteral("tab-a"), 1, 3, 2, 7, 11,
        QStringLiteral("com.qbrowser.first"));
    const TabCapabilityAuthority secondAuthority = authority(
        QStringLiteral("tab-b"), 2, 5, 4, 9, 13,
        QStringLiteral("com.qbrowser.second"));
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    QString firstError;
    QString secondError;
    auto first = HostCapabilityRuntime::create(
        firstAuthority, firstToken, router.get(), clipboardPermission(), origin,
        firstStorage.path(), 1, &firstError);
    auto second = HostCapabilityRuntime::create(
        secondAuthority, secondToken, router.get(), clipboardPermission(), origin,
        secondStorage.path(), 1, &secondError);
    const bool firstCreated = first != nullptr;
    const bool secondCreated = second != nullptr;
    HostCapabilityRuntime::retire(std::exchange(first, {}));
    HostCapabilityRuntime::retire(std::exchange(second, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));

    QVERIFY2(firstCreated, qPrintable(firstError));
    QVERIFY2(secondCreated, qPrintable(secondError));
}

void HostCapabilityRuntimeTest::onlyActiveAuthorityCanIssueAndConsumeGesture()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority sibling = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 7, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto siblingStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto siblingSession = siblingStore->openSession(sibling.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(siblingSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        sibling, siblingToken, siblingStore, std::move(*siblingSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));

    QVERIFY(!router->issueGrant(sibling, QStringLiteral("same-request"), 250)
                 .has_value());
    auto firstGrant = router->issueGrant(
        first, QStringLiteral("same-request"), 250);
    QVERIFY(firstGrant.has_value());
    QVERIFY(!siblingStore->consume(*firstGrant,
                                   sibling.appIdentity,
                                   QStringLiteral("same-request")));
    QVERIFY(firstStore->consume(*firstGrant,
                                first.appIdentity,
                                QStringLiteral("same-request")));
    QVERIFY(!firstStore->consume(*firstGrant,
                                 first.appIdentity,
                                 QStringLiteral("same-request")));

    QVERIFY(router->activateBinding(sibling));
    router->setSystemEvidenceForTesting(validEvidence(sibling, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 1'050));
    auto siblingGrant = router->issueGrant(
        sibling, QStringLiteral("same-request"), 250);
    QVERIFY(siblingGrant.has_value());
    QVERIFY(siblingStore->consume(*siblingGrant,
                                  sibling.appIdentity,
                                  QStringLiteral("same-request")));
}

void HostCapabilityRuntimeTest::authorityTransitionsRevokeUnconsumedEvidence()
{
    const auto switchOrDeactivate = [](const bool switchTabs) {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        const TabCapabilityAuthority first = authority(
            QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
        const TabCapabilityAuthority sibling = authority(
            QStringLiteral("tab-b"), 2, 42, 402, 7, 12);
        auto firstToken = std::make_shared<AuthorityAdmissionToken>();
        auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
        auto firstStore = std::make_shared<UserGestureGrantStore>();
        auto siblingStore = std::make_shared<UserGestureGrantStore>();
        auto firstSession = firstStore->openSession(first.appIdentity);
        auto siblingSession = siblingStore->openSession(sibling.appIdentity);
        QVERIFY(firstSession.has_value());
        QVERIFY(siblingSession.has_value());
        QVERIFY(router->registerBinding(
            first, firstToken, firstStore, std::move(*firstSession)));
        QVERIFY(router->registerBinding(
            sibling, siblingToken, siblingStore, std::move(*siblingSession)));
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        if (switchTabs) {
            QVERIFY(router->activateBinding(sibling));
        } else {
            router->hostDeactivated();
        }
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'001));
        QVERIFY(!router->issueGrant(first, QStringLiteral("after-transition"), 250)
                     .has_value());
    };
    switchOrDeactivate(true);
    switchOrDeactivate(false);

    const auto closeBinding = [] {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        const TabCapabilityAuthority first = authority(
            QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
        auto token = std::make_shared<AuthorityAdmissionToken>();
        auto store = std::make_shared<UserGestureGrantStore>();
        auto session = store->openSession(first.appIdentity);
        QVERIFY(session.has_value());
        QVERIFY(router->registerBinding(
            first, token, store, std::move(*session)));
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        auto grant = router->issueGrant(first, QStringLiteral("unconsumed"), 250);
        QVERIFY(grant.has_value());
        router->unregisterBinding(first);
        QVERIFY(!store->consume(*grant, first.appIdentity,
                                QStringLiteral("unconsumed")));
        QVERIFY(!token->tryAcquireUse().has_value());
    };
    closeBinding();

    const TabCapabilityAuthority original = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const QList<TabCapabilityAuthority> replacements{
        authority(QStringLiteral("tab-a"), 2, 41, 401, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 43, 401, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 403, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 8, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 7, 12),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 7, 11,
                  QStringLiteral("com.qbrowser.replacement")),
    };
    for (const TabCapabilityAuthority &replacement : replacements) {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        auto originalToken = std::make_shared<AuthorityAdmissionToken>();
        auto replacementToken = std::make_shared<AuthorityAdmissionToken>();
        auto originalStore = std::make_shared<UserGestureGrantStore>();
        auto replacementStore = std::make_shared<UserGestureGrantStore>();
        auto originalSession = originalStore->openSession(original.appIdentity);
        auto replacementSession = replacementStore->openSession(
            replacement.appIdentity);
        QVERIFY(originalSession.has_value());
        QVERIFY(replacementSession.has_value());
        QVERIFY(router->registerBinding(
            original, originalToken, originalStore,
            std::move(*originalSession)));
        QVERIFY(router->activateBinding(original));
        router->setSystemEvidenceForTesting(validEvidence(original, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        auto grant = router->issueGrant(
            original, QStringLiteral("unconsumed"), 250);
        QVERIFY(grant.has_value());

        QVERIFY(router->registerBinding(
            replacement, replacementToken,
            replacementStore, std::move(*replacementSession)));
        QVERIFY(!originalStore->consume(*grant, original.appIdentity,
                                        QStringLiteral("unconsumed")));
        QVERIFY(!originalToken->tryAcquireUse().has_value());
        QVERIFY(router->activateBinding(replacement));
        router->setSystemEvidenceForTesting(validEvidence(replacement, 1'001));
        QVERIFY(!router->issueGrant(
                         replacement, QStringLiteral("no-reused-evidence"), 250)
                     .has_value());
    }
}

void HostCapabilityRuntimeTest::mainWindowDeactivationRevokesGestureEvidence()
{
    HostApplication host(QUrl(QStringLiteral("http://127.0.0.1:8080/")));
    QVERIFY(host.start());
    QVERIFY(host.mainWindow() != nullptr);
    HostGestureRouter *const router = host.gestureRouterForTesting();
    QVERIFY(router != nullptr);

    const TabCapabilityAuthority binding = authority(
        host.mainWindow()->tabModel()->activeId(), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    auto grant = router->issueGrant(
        binding, QStringLiteral("before-window-deactivate"), 5'000);
    QVERIFY(grant.has_value());

    QEvent deactivate(QEvent::WindowDeactivate);
    QVERIFY(QCoreApplication::sendEvent(host.mainWindow(), &deactivate));
    QVERIFY(!store->consume(*grant, binding.appIdentity,
                            QStringLiteral("before-window-deactivate")));

    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_B), true, 1'050));
    QEvent activate(QEvent::WindowActivate);
    QVERIFY(QCoreApplication::sendEvent(host.mainWindow(), &activate));
    QVERIFY(!store->consume(*grant, binding.appIdentity,
                            QStringLiteral("before-window-deactivate")));
    QVERIFY(!router->issueGrant(
        binding, QStringLiteral("after-window-activate"), 250).has_value());
}

void HostCapabilityRuntimeTest::backgroundFocusAndDispatchStateCannotAuthorize()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));

    HostGestureSystemEvidence background = validEvidence(binding, 1'000);
    background.foregroundMatchesHostRoot = false;
    router->setSystemEvidenceForTesting(background);
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'001));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("background"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 2'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 1'900));
    HostGestureSystemEvidence stolen = validEvidence(binding, 2'001);
    stolen.foregroundMatchesHostRoot = false;
    router->setSystemEvidenceForTesting(stolen);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("stolen"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 2'002));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("stale-after-stolen"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 3'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 2'900));
    HostGestureSystemEvidence wrongPid = validEvidence(binding, 3'001);
    wrongPid.workerWindowProcessId = binding.workerProcessId + 1;
    router->setSystemEvidenceForTesting(wrongPid);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("wrong-pid"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 3'002));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("pid-evidence-revoked"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 4'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 3'900));
    HostGestureSystemEvidence wrongWindow = validEvidence(binding, 4'001);
    wrongWindow.workerWindowId = binding.workerWindowId + 1;
    router->setSystemEvidenceForTesting(wrongWindow);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("wrong-window"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 5'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 4'900, true));
    QVERIFY(!router->issueGrant(
                     binding, QStringLiteral("lower-il-keyboard"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 5'100));
    QVERIFY(!router->observeMouseForTesting(
        binding.workerWindowId, binding.workerProcessId, true, 5'000, true));
    QVERIFY(!router->issueGrant(
                     binding, QStringLiteral("lower-il-mouse"), 250)
                 .has_value());
}

void HostCapabilityRuntimeTest::browserCommandIsSuppressedAndNeverBecomesGesture()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'000));
    QSignalSpy commands(router.get(), &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());
    const QKeyCombination ctrlTab(Qt::ControlModifier, Qt::Key_Tab);

    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Control), true, 890));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 900));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 901));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 902));
    QTRY_COMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(),
             BrowserCommand::NextTab);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("ctrl-tab"), 250)
                 .has_value());

    (void)token->beginRevoke();
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'100));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 1'050));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 1'051));
    QCoreApplication::processEvents();
    QCOMPARE(commands.count(), 0);
    QVERIFY(!router->activateBinding(binding));
}

void HostCapabilityRuntimeTest::staleQueuedBrowserCommandIsDropped_data()
{
    QTest::addColumn<bool>("retireOriginal");
    QTest::newRow("switch") << false;
    QTest::newRow("retire-and-switch") << true;
}

void HostCapabilityRuntimeTest::staleQueuedBrowserCommandIsDropped()
{
    QFETCH(bool, retireOriginal);
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority second = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto secondStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto secondSession = secondStore->openSession(second.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(secondSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        second, secondToken, secondStore, std::move(*secondSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QSignalSpy commands(router.get(),
                        &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());
    const QKeyCombination ctrlTab(Qt::ControlModifier, Qt::Key_Tab);

    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 900));
    QCOMPARE(commands.count(), 0);
    if (retireOriginal) router->unregisterBinding(first);
    QVERIFY(router->activateBinding(second));
    router->setSystemEvidenceForTesting(validEvidence(second, 1'100));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);

    QCOMPARE(commands.count(), 0);
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 1'101));
}

void HostCapabilityRuntimeTest::heldBrowserChordRemainsSuppressedAcrossBindingSwitch()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority second = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto secondStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto secondSession = secondStore->openSession(second.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(secondSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        second, secondToken, secondStore, std::move(*secondSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QSignalSpy commands(router.get(),
                        &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());

    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Control), true, 890));
    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Tab), true, 900));
    QVERIFY(router->activateBinding(second));
    router->setSystemEvidenceForTesting(validEvidence(second, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Control), false, 1'040));

    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), true, 1'050));
    QVERIFY(!router->issueGrant(
        second, QStringLiteral("held-tab-repeat"), 250).has_value());
    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), false, 1'051));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);
    QCOMPARE(commands.count(), 0);

    router->setSystemEvidenceForTesting(validEvidence(second, 1'200));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), true, 1'150));
    QVERIFY(router->issueGrant(
        second, QStringLiteral("new-tab-keydown"), 250).has_value());
}

void HostCapabilityRuntimeTest::revocationClosesUseAndPublicationUntilGuardDrains()
{
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto guard = token->tryAcquireUse();
    QVERIFY(guard.has_value());
    int publications = 0;
    QVERIFY(guard->publishIfStillAdmitted([&publications] {
        ++publications;
        return true;
    }));
    QCOMPARE(publications, 1);

    const AuthorityAdmissionToken::RevocationTicket ticket = token->beginRevoke();
    QVERIFY(!ticket.isDrained());
    QVERIFY(!ticket.waitAllowedOnCurrentThread());
    QVERIFY(!token->tryAcquireUse().has_value());
    QVERIFY(!guard->publishIfStillAdmitted([&publications] {
        ++publications;
        return true;
    }));
    QCOMPARE(publications, 1);
    auto repeatedRevoker = std::async(std::launch::async, [token] {
        const auto repeatedTicket = token->beginRevoke();
        return repeatedTicket.waitAllowedOnCurrentThread();
    });
    QVERIFY(!repeatedRevoker.get());

    auto timed = std::async(std::launch::async, [ticket] {
        return ticket.waitUntil(monotonicDeadlineAfter(
            std::chrono::milliseconds(20)));
    });
    QVERIFY(!timed.get());
    QVERIFY(!ticket.isDrained());
    bool publicationAfterDeadlineRan = false;
    QVERIFY(!guard->publishIfStillAdmitted([&publicationAfterDeadlineRan] {
        publicationAfterDeadlineRan = true;
        return true;
    }));
    QVERIFY(!publicationAfterDeadlineRan);
    auto drained = std::async(std::launch::async, [ticket] {
        ticket.waitUntilDrained();
        return ticket.isDrained();
    });
    QCOMPARE(drained.wait_for(std::chrono::milliseconds(20)),
             std::future_status::timeout);
    guard.reset();
    QCOMPARE(drained.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    QVERIFY(drained.get());
    QVERIFY(ticket.isDrained());
}

void HostCapabilityRuntimeTest::retiringRuntimeLeavesSiblingGestureStorageAndCompletionActive()
{
    QTemporaryDir firstStorage;
    QTemporaryDir siblingStorage;
    QVERIFY(firstStorage.isValid());
    QVERIFY(siblingStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority firstAuthority = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority siblingAuthority = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
    QString firstError;
    QString siblingError;
    auto first = HostCapabilityRuntime::create(
        firstAuthority, firstToken, router.get(),
        clipboardAndStoragePermissions(), origin, firstStorage.path(), 100,
        &firstError);
    QVERIFY2(first != nullptr, qPrintable(firstError));
    auto firstCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(first, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    auto sibling = HostCapabilityRuntime::create(
        siblingAuthority, siblingToken, router.get(),
        clipboardAndStoragePermissions(), origin, siblingStorage.path(), 100,
        &siblingError);
    QVERIFY2(sibling != nullptr, qPrintable(siblingError));
    auto siblingCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(sibling, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    QSignalSpy completed(sibling.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());
    QVERIFY(router->activateBinding(firstAuthority));
    router->setSystemEvidenceForTesting(validEvidence(firstAuthority, 800));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 700));

    HostCapabilityRuntime::retire(std::exchange(first, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(!firstToken->tryAcquireUse().has_value());
    QVERIFY(siblingToken->tryAcquireUse().has_value());
    QVERIFY(router->activateBinding(siblingAuthority));
    sibling->dispatch(
        siblingAuthority.sessionGeneration + 1,
        QStringLiteral("wrong-generation"), QStringLiteral("storage"),
        QStringLiteral("get"),
        {{QStringLiteral("key"), QStringLiteral("must-not-dispatch")}});
    QTest::qWait(50);
    QCOMPARE(completed.count(), 0);
    router->setSystemEvidenceForTesting(validEvidence(siblingAuthority, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    QGuiApplication::clipboard()->setText(QStringLiteral("sibling-value"));
    sibling->dispatch(siblingAuthority.sessionGeneration,
                      QStringLiteral("same-request"),
                      QStringLiteral("clipboard"), QStringLiteral("read"), {});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2'000);
    QCOMPARE(completed.at(0).at(0).value<TabCapabilityAuthority>(),
             siblingAuthority);
    QCOMPARE(completed.at(0).at(1).toULongLong(),
             siblingAuthority.sessionGeneration);
    QCOMPARE(completed.at(0).at(2).toString(), QStringLiteral("same-request"));
    const BrokerResult clipboardResult =
        completed.at(0).at(3).value<BrokerResult>();
    QVERIFY(clipboardResult.ok
            || clipboardResult.errorCode == QStringLiteral("clipboard.failed"));
    QVERIFY(clipboardResult.errorCode
            != QStringLiteral("clipboard.gesture_required"));

    sibling->dispatch(
        siblingAuthority.sessionGeneration, QStringLiteral("storage-request"),
        QStringLiteral("storage"), QStringLiteral("get"),
        {{QStringLiteral("key"), QStringLiteral("survives")}});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 2'000);
    QCOMPARE(completed.at(1).at(0).value<TabCapabilityAuthority>(),
             siblingAuthority);
    QCOMPARE(completed.at(1).at(2).toString(),
             QStringLiteral("storage-request"));
    QCOMPARE(completed.at(1).at(3).value<BrokerResult>().errorCode,
             QStringLiteral("storage.not_found"));
}

void HostCapabilityRuntimeTest::repeatedRetireClaimsWorkerExactlyOnce()
{
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"), {},
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), storage.path(),
        0, 0, 0, &error);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    auto alias = runtime;

    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    HostCapabilityRuntime::retire(std::exchange(alias, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QCoreApplication::processEvents();
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
}

void HostCapabilityRuntimeTest::slowRequestRetiresWithoutBlockingOrLateDelivery()
{
    QTcpServer silentServer;
    QVERIFY(silentServer.listen(QHostAddress::LocalHost));
    const QUrl origin(QStringLiteral("http://127.0.0.1:%1/")
                          .arg(silentServer.serverPort()));
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"),
        networkPermission(silentServer.serverPort()), origin, storage.path(),
        0, 0, 0, &error);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    int delivered = 0;
    connect(runtime.get(), &HostCapabilityRuntime::completed,
            this, [&delivered] { ++delivered; });
    runtime->dispatch(
        7, QStringLiteral("slow-request"), QStringLiteral("network"),
        QStringLiteral("request"),
        requestPayload(QUrl(origin.toString() + QStringLiteral("api/dashboard"))));

    QElapsedTimer destruction;
    destruction.start();
    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    QVERIFY2(destruction.elapsed() < 100,
             qPrintable(QStringLiteral("destruction blocked %1 ms")
                            .arg(destruction.elapsed())));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    QCoreApplication::processEvents();
    QCOMPARE(delivered, 0);
}

QTEST_MAIN(HostCapabilityRuntimeTest)

#include "tst_host_capability_runtime.moc"
