#include "MainWindow.h"
#include "HostWorkerSessionController.h"
#include "TestEnvironment.h"

#include <QSignalSpy>
#include <QTest>

class HostRoutesE2eTest final : public QObject
{
    Q_OBJECT
private slots:
    void productionHostNavigatesTenPilotRoutes();
};

void HostRoutesE2eTest::productionHostNavigatesTenPilotRoutes()
{
    TestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(environment.start(), qPrintable(environment.error()));
    MainWindow *window = environment.host()->mainWindow();
    QVERIFY(window != nullptr);
    QVERIFY2(environment.host()->hasWorkerContext(),
             qPrintable(window->trustedErrorText()));
    HostWorkerSessionController *controller =
        environment.host()->workerSessionController();
    QVERIFY(controller != nullptr);
    QSignalSpy acknowledged(controller,
                            &HostWorkerSessionController::routeLoadAcknowledged);
    const QStringList routes{
        QStringLiteral("/login"), QStringLiteral("/dashboard"),
        QStringLiteral("/orders"), QStringLiteral("/orders/ORD-1001"),
        QStringLiteral("/orders/ORD-1001/edit"), QStringLiteral("/customers"),
        QStringLiteral("/customers/CUS-001"), QStringLiteral("/files"),
        QStringLiteral("/settings"), QStringLiteral("/web/help")};
    for (const QString &route : routes) {
        const QString appUrl = QStringLiteral("app://pilot") + route;
        const qsizetype acknowledgedBefore = acknowledged.size();
        QVERIFY2(window->navigate(appUrl),
                 qPrintable(route + QStringLiteral(": ") + window->trustedErrorText()
                            + QStringLiteral(" / ")
                            + environment.host()->workerSessionController()->lastErrorCode()));
        QCOMPARE(window->currentAppUrl(), appUrl);
        QCOMPARE(window->activeSurface(), route == QStringLiteral("/web/help")
                                          ? HostSurfaceKind::Web
                                          : HostSurfaceKind::Worker);
        if (route != QStringLiteral("/web/help")) {
            QTRY_COMPARE_WITH_TIMEOUT(acknowledged.size(),
                                      acknowledgedBefore + 1, 10'000);
            QCOMPARE(acknowledged.last().at(0).toString(), route);
            QCOMPARE(controller->pendingRouteLoadCount(), qsizetype(0));
            QCOMPARE(controller->state(), HostWorkerSessionState::Running);
        }
    }
    QCOMPARE(window->historyCount(), routes.size() + 1);
    QCOMPARE(window->historyIndex(), routes.size());
    // The final route is WebEngine-backed. The tab-keyed app runtime must be
    // retired instead of leaving a hidden package worker attached to the tab.
    QVERIFY(!environment.host()->hasWorkerContext());
    QVERIFY2(environment.shutdown(), qPrintable(environment.error()));
}

QTEST_MAIN(HostRoutesE2eTest)
#include "tst_host_routes.moc"
