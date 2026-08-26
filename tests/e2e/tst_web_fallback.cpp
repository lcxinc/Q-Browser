#include "MainWindow.h"
#include "TestEnvironment.h"
#include "WebSurface.h"

#include <QSignalSpy>
#include <QTest>

class WebFallbackE2eTest final : public QObject
{
    Q_OBJECT
private slots:
    void productionHostSwitchesQmlWebQml();
};

void WebFallbackE2eTest::productionHostSwitchesQmlWebQml()
{
    TestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(environment.start(), qPrintable(environment.error()));
    MainWindow *window = environment.host()->mainWindow();
    QVERIFY(window != nullptr);
    QVERIFY(window->navigate(QStringLiteral("app://pilot/dashboard")));
    QCOMPARE(window->activeSurface(), HostSurfaceKind::Worker);

    QVERIFY(window->navigate(QStringLiteral("app://pilot/web/help")));
    QCOMPARE(window->activeSurface(), HostSurfaceKind::Web);
    WebSurface *webSurface = window->webSurface();
    QVERIFY(webSurface != nullptr);
    QSignalSpy loaded(webSurface, &WebSurface::navigationFinished);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15'000);
    const QList<QVariant> completion = loaded.takeLast();
    QCOMPARE(completion.at(0).toUrl(), QUrl(environment.mockOrigin() + QStringLiteral("/help")));
    QVERIFY(completion.at(1).toBool());

    QVERIFY(window->navigate(QStringLiteral("app://pilot/settings")));
    QCOMPARE(window->activeSurface(), HostSurfaceKind::Worker);
    QVERIFY(environment.host()->hasWorkerContext());
    QVERIFY2(environment.shutdown(), qPrintable(environment.error()));
}

QTEST_MAIN(WebFallbackE2eTest)
#include "tst_web_fallback.moc"
