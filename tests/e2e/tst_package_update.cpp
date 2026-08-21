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

    const int failures = environment.failureCount();
    const QString tampered = environment.createTamperedPackage(QStringLiteral("1.2.0"));
    QVERIFY(!tampered.isEmpty());
    QVERIFY(environment.install(tampered));
    QVERIFY(environment.waitForFailure(failures));
    QVERIFY(!environment.readyVersions().contains(QStringLiteral("1.2.0")));
    QVERIFY(environment.host()->hasWorkerContext());
}

QTEST_MAIN(PackageUpdateE2eTest)
#include "tst_package_update.moc"
