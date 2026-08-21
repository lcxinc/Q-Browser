#include "MainWindow.h"
#include "TestEnvironment.h"

#include <QElapsedTimer>
#include <QTest>
#include <qt_windows.h>

namespace {
bool terminateWorker(const quint32 processId)
{
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                       FALSE, processId);
    if (process == nullptr) return false;
    const bool result = TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE
        && WaitForSingleObject(process, 10'000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return result;
}
}

class HostSurvivesWorkerCrashE2eTest final : public QObject
{
    Q_OBJECT
private slots:
    void crashingCandidateRollsBackWithoutTerminatingHost();
};

void HostSurvivesWorkerCrashE2eTest::crashingCandidateRollsBackWithoutTerminatingHost()
{
    TestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(environment.start(), qPrintable(environment.error()));
    QTest::qWait(500);
    const qsizetype readyBefore = environment.readyVersions().size();
    const QByteArray crashQml = QByteArrayLiteral(
        "import QtQuick\nItem { width: 320; height: 200 }");
    const QString crashing = environment.createPackage(QStringLiteral("1.1.0"), crashQml);
    QVERIFY(!crashing.isEmpty());
    QVERIFY(environment.install(crashing));

    QVERIFY(environment.waitForReady(QStringLiteral("1.1.0")));
    QVERIFY(terminateWorker(environment.currentWorkerProcessId()));
    QElapsedTimer elapsed;
    elapsed.start();
    while (environment.readyVersions().size() < readyBefore + 2
           && elapsed.elapsed() < 60'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }
    QVERIFY(environment.readyVersions().size() >= readyBefore + 2);
    QCOMPARE(environment.readyVersions().last(), QStringLiteral("1.1.0"));
    QVERIFY(terminateWorker(environment.currentWorkerProcessId()));
    elapsed.restart();
    while ((environment.readyVersions().size() < readyBefore + 3
            || environment.readyVersions().last() != QStringLiteral("1.0.0"))
           && elapsed.elapsed() < 60'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }
    QVERIFY(environment.readyVersions().size() >= readyBefore + 3);
    QCOMPARE(environment.readyVersions().last(), QStringLiteral("1.0.0"));
    QVERIFY(environment.host()->mainWindow() != nullptr);
    QVERIFY(environment.host()->mainWindow()->navigate(
        QStringLiteral("app://pilot/dashboard")));
    QCOMPARE(environment.host()->mainWindow()->activeSurface(), HostSurfaceKind::Worker);
}

QTEST_MAIN(HostSurvivesWorkerCrashE2eTest)
#include "tst_host_survives_worker_crash.moc"
