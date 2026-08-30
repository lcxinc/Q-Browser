#include "BrowserWindowGeometry.h"

#include <QList>
#include <QRect>
#include <QTest>

class BrowserWindowGeometryTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesPersistedGeometryVisibleOnCurrentScreen();
    void centersDefaultWhenPersistedGeometryIsCompletelyOffscreen();
    void centersDefaultWhenPersistedMonitorWasRemoved();
    void requiresSixtyFourBySixtyFourOnOneScreen_data();
    void requiresSixtyFourBySixtyFourOnOneScreen();
    void clampsDefaultToPrimaryAvailableGeometry_data();
    void clampsDefaultToPrimaryAvailableGeometry();
    void handlesInvalidOrEmptyScreenInputSafely_data();
    void handlesInvalidOrEmptyScreenInputSafely();
};

void BrowserWindowGeometryTest::preservesPersistedGeometryVisibleOnCurrentScreen()
{
    const QRect primaryAvailable(0, 0, 1920, 1040);
    const QRect persisted(200, 120, 1000, 700);

    QCOMPARE(restoreBrowserWindowGeometry(
                 persisted, primaryAvailable, {primaryAvailable}),
             persisted);
}

void BrowserWindowGeometryTest::centersDefaultWhenPersistedGeometryIsCompletelyOffscreen()
{
    const QRect primaryAvailable(100, 50, 1600, 900);
    const QRect persisted(-3000, -2000, 1000, 700);

    QCOMPARE(restoreBrowserWindowGeometry(
                 persisted, primaryAvailable, {primaryAvailable}),
             QRect(260, 100, 1280, 800));
}

void BrowserWindowGeometryTest::centersDefaultWhenPersistedMonitorWasRemoved()
{
    const QRect primaryAvailable(0, 0, 1920, 1040);
    const QRect removedMonitorGeometry(2240, 180, 1200, 760);

    QCOMPARE(restoreBrowserWindowGeometry(
                 removedMonitorGeometry,
                 primaryAvailable,
                 {primaryAvailable}),
             QRect(320, 120, 1280, 800));
}

void BrowserWindowGeometryTest::requiresSixtyFourBySixtyFourOnOneScreen_data()
{
    QTest::addColumn<QRect>("persisted");
    QTest::addColumn<QList<QRect>>("availableScreens");
    QTest::addColumn<QRect>("expected");

    const QRect primaryAvailable(0, 0, 1920, 1040);
    const QRect fallback(320, 120, 1280, 800);

    const QRect exactEdge(1856, 976, 200, 200);
    QTest::newRow("exact-64-by-64-edge")
        << exactEdge << QList<QRect>{primaryAvailable} << exactEdge;
    QTest::newRow("63-pixels-wide")
        << QRect(1857, 976, 200, 200)
        << QList<QRect>{primaryAvailable} << fallback;
    QTest::newRow("63-pixels-high")
        << QRect(1856, 977, 200, 200)
        << QList<QRect>{primaryAvailable} << fallback;
    QTest::newRow("area-4096-but-only-32-wide")
        << QRect(1888, 912, 200, 128)
        << QList<QRect>{primaryAvailable} << fallback;
    QTest::newRow("64-pixels-split-between-two-screens")
        << QRect(1888, 200, 64, 300)
        << QList<QRect>{primaryAvailable, QRect(1920, 0, 1920, 1040)}
        << fallback;

    const QRect negativeCoordinateScreen(-1600, 0, 1600, 900);
    const QRect exactNegativeEdge(-1660, 100, 124, 600);
    QTest::newRow("exact-negative-coordinate-edge")
        << exactNegativeEdge
        << QList<QRect>{primaryAvailable, negativeCoordinateScreen}
        << exactNegativeEdge;
}

void BrowserWindowGeometryTest::requiresSixtyFourBySixtyFourOnOneScreen()
{
    QFETCH(QRect, persisted);
    QFETCH(QList<QRect>, availableScreens);
    QFETCH(QRect, expected);

    const QRect primaryAvailable(0, 0, 1920, 1040);
    QCOMPARE(restoreBrowserWindowGeometry(
                 persisted, primaryAvailable, availableScreens),
             expected);
}

void BrowserWindowGeometryTest::clampsDefaultToPrimaryAvailableGeometry_data()
{
    QTest::addColumn<QRect>("primaryAvailable");
    QTest::addColumn<QRect>("expected");

    QTest::newRow("both-dimensions-smaller")
        << QRect(-1200, 30, 1024, 700)
        << QRect(-1200, 30, 1024, 700);
    QTest::newRow("height-only-smaller")
        << QRect(40, -200, 1600, 600)
        << QRect(200, -200, 1280, 600);
    QTest::newRow("width-only-smaller")
        << QRect(-500, 80, 900, 1000)
        << QRect(-500, 180, 900, 800);
}

void BrowserWindowGeometryTest::clampsDefaultToPrimaryAvailableGeometry()
{
    QFETCH(QRect, primaryAvailable);
    QFETCH(QRect, expected);

    QCOMPARE(restoreBrowserWindowGeometry(
                 QRect(5000, 5000, 900, 700),
                 primaryAvailable,
                 {primaryAvailable}),
             expected);
}

void BrowserWindowGeometryTest::handlesInvalidOrEmptyScreenInputSafely_data()
{
    QTest::addColumn<QRect>("persisted");
    QTest::addColumn<QRect>("primaryAvailable");
    QTest::addColumn<QList<QRect>>("availableScreens");
    QTest::addColumn<QRect>("expected");

    const QRect deterministicDefault(0, 0, 1280, 800);
    QTest::newRow("invalid-primary-empty-list")
        << QRect(100, 100, 900, 700) << QRect() << QList<QRect>{}
        << deterministicDefault;
    QTest::newRow("all-screen-geometries-invalid")
        << QRect(100, 100, 900, 700) << QRect()
        << QList<QRect>{QRect(), QRect(20, 30, 0, 600)}
        << deterministicDefault;

    const QRect firstValidScreen(-1000, 50, 800, 600);
    QTest::newRow("invalid-primary-uses-first-valid-current-screen")
        << QRect(5000, 5000, 900, 700) << QRect()
        << QList<QRect>{QRect(), firstValidScreen} << firstValidScreen;
    QTest::newRow("persisted-coordinates-do-not-select-fallback-screen")
        << QRect(1537, 100, 200, 700) << QRect()
        << QList<QRect>{firstValidScreen, QRect(0, 50, 1600, 900)}
        << firstValidScreen;

    const QRect primaryAvailable(50, 100, 1600, 900);
    QTest::newRow("empty-list-does-not-trust-persisted-coordinates")
        << QRect(200, 200, 900, 700) << primaryAvailable << QList<QRect>{}
        << QRect(210, 150, 1280, 800);
    QTest::newRow("invalid-persisted-geometry")
        << QRect(200, 200, 0, 700) << primaryAvailable
        << QList<QRect>{primaryAvailable} << QRect(210, 150, 1280, 800);

    const QRect validSecondary(-1400, -100, 1200, 900);
    const QRect visibleOnSecondary(-1300, 20, 900, 700);
    QTest::newRow("invalid-primary-still-allows-current-screen-validation")
        << visibleOnSecondary << QRect()
        << QList<QRect>{QRect(), validSecondary} << visibleOnSecondary;
}

void BrowserWindowGeometryTest::handlesInvalidOrEmptyScreenInputSafely()
{
    QFETCH(QRect, persisted);
    QFETCH(QRect, primaryAvailable);
    QFETCH(QList<QRect>, availableScreens);
    QFETCH(QRect, expected);

    QCOMPARE(restoreBrowserWindowGeometry(
                 persisted, primaryAvailable, availableScreens),
             expected);
}

QTEST_APPLESS_MAIN(BrowserWindowGeometryTest)

#include "tst_browser_window_geometry.moc"
