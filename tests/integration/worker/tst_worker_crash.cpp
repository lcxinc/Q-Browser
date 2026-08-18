#include "WorkerSupervisor.h"

#include <QTest>

class WorkerCrashTest final : public QObject
{
    Q_OBJECT
private slots:
    void restartsOnceThenRollsBackInsideHealthWindow();
    void staleActivationCannotRestartOrRollBack();
    void heartbeatDeadlineIsSupervised();
};

void WorkerCrashTest::restartsOnceThenRollsBackInsideHealthWindow()
{
    int restarts = 0;
    int rollbacks = 0;
    WorkerSupervisor supervisor({5000, 1000}, [&] { ++restarts; }, [&] { ++rollbacks; });
    const quint64 generation = supervisor.beginActivation(100);
    QCOMPARE(supervisor.workerExited(generation, WorkerExitReason::Crashed, 200),
             WorkerSupervisionAction::Restart);
    QCOMPARE(restarts, 1);
    QCOMPARE(rollbacks, 0);
    QCOMPARE(supervisor.workerExited(generation, WorkerExitReason::Crashed, 300),
             WorkerSupervisionAction::CrashLoopRollback);
    QCOMPARE(restarts, 1);
    QCOMPARE(rollbacks, 1);
    QVERIFY(supervisor.isCrashLoop());
}

void WorkerCrashTest::staleActivationCannotRestartOrRollBack()
{
    int restarts = 0;
    int rollbacks = 0;
    WorkerSupervisor supervisor({5000, 1000}, [&] { ++restarts; }, [&] { ++rollbacks; });
    const quint64 stale = supervisor.beginActivation(100);
    const quint64 current = supervisor.beginActivation(200);
    QVERIFY(current != stale);
    QCOMPARE(supervisor.workerExited(stale, WorkerExitReason::Crashed, 300),
             WorkerSupervisionAction::IgnoredStaleGeneration);
    QCOMPARE(restarts, 0);
    QCOMPARE(rollbacks, 0);
}

void WorkerCrashTest::heartbeatDeadlineIsSupervised()
{
    int restarts = 0;
    WorkerSupervisor supervisor({5000, 1000}, [&] { ++restarts; }, [] {});
    const quint64 generation = supervisor.beginActivation(0);
    supervisor.heartbeat(generation, 500);
    QCOMPARE(supervisor.checkHealth(generation, 1499), WorkerSupervisionAction::None);
    QCOMPARE(supervisor.checkHealth(generation, 1501), WorkerSupervisionAction::Restart);
    QCOMPARE(restarts, 1);
}

QTEST_GUILESS_MAIN(WorkerCrashTest)
#include "tst_worker_crash.moc"
