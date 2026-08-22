#include "TestEnvironment.h"

#include <QTest>

class PackageUpdateE2eTest final : public QObject
{
    Q_OBJECT
private slots:
    void signedUpdateActivatesAndTamperNeverExecutes();
};

void PackageUpdateE2eTest::signedUpdateActivatesAndTamperNeverExecutes()
{
    TestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(environment.start(), qPrintable(environment.error()));
    const QString update = environment.createPackage(QStringLiteral("1.1.0"));
    QVERIFY(!update.isEmpty());
    QVERIFY(environment.install(update));
    QVERIFY(environment.waitForReady(QStringLiteral("1.1.0")));
    QVERIFY(environment.waitForHealthyVersion(QStringLiteral("1.1.0")));
    const QString expectedCurrent = environment.currentVersionDirectory();
    const QString expectedLastKnownGood = environment.lastKnownGoodVersionDirectory();
    QVERIFY(expectedCurrent.startsWith(QStringLiteral("1.1.0-")));
    QVERIFY(expectedLastKnownGood.startsWith(QStringLiteral("1.1.0-")));

    const int failures = environment.failureCount();
    const QString tampered = environment.createTamperedPackage(QStringLiteral("1.2.0"));
    QVERIFY(!tampered.isEmpty());
    QVERIFY(environment.install(tampered));
    QVERIFY(environment.waitForFailure(failures));
    QCOMPARE(environment.lastFailure(), QStringLiteral("signature_invalid"));
    QVERIFY(!environment.readyVersions().contains(QStringLiteral("1.2.0")));
    QCOMPARE(environment.tamperCanaryCount(), 0);
    QCOMPARE(environment.currentVersionDirectory(), expectedCurrent);
    QCOMPARE(environment.lastKnownGoodVersionDirectory(), expectedLastKnownGood);
    QVERIFY(environment.host()->hasWorkerContext());
    QVERIFY2(environment.shutdown(), qPrintable(environment.error()));
}

QTEST_MAIN(PackageUpdateE2eTest)
#include "tst_package_update.moc"
