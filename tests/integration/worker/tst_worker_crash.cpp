#include "WorkerSupervisor.h"
#include "WorkerTestEnvironment.h"

#include <QCoreApplication>
#include <QTest>

namespace {

bool completeHandshake(WorkerTestEnvironment::Launch &launch)
{
    const auto handshake = launch.hostSession.receive(15000);
    const auto surface = launch.hostSession.receive(15000);
    const auto ready = launch.hostSession.receive(15000);
    return handshake.status == SessionStatus::MessageReady
        && handshake.message->type() == ProtocolType::Handshake
        && surface.status == SessionStatus::MessageReady
        && surface.message->type() == ProtocolType::SurfaceReady
        && ready.status == SessionStatus::MessageReady
        && ready.message->type() == ProtocolType::Ready;
}

void terminateAndClose(WorkerTestEnvironment::Launch &launch)
{
    launch.hostSession.close();
    launch.process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(launch.process.waitForFinished(5000));
    const auto closed = launch.process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

} // namespace

class WorkerCrashTest final : public QObject
{
    Q_OBJECT
private slots:
    void restartBudgetBelongsToActivationNotAttempt();
    void timeoutAndExitOfSameAttemptCountOnce();
    void staleAttemptSignalsAndCleanShutdownAreIgnored();
    void realLpacWorkerRestartsThenRollsBack();
};

void WorkerCrashTest::restartBudgetBelongsToActivationNotAttempt()
{
    int restarts = 0;
    int rollbacks = 0;
    WorkerSupervisor supervisor({5000, 1000},
                                [&](WorkerActivationId) { ++restarts; },
                                [&](WorkerActivationId) { ++rollbacks; });
    const WorkerActivationId activation = supervisor.beginActivation(100);
    const WorkerAttemptId first = *supervisor.beginAttempt(activation, 100);
    QCOMPARE(supervisor.workerExited({activation, first}, WorkerExitReason::Crashed, 200),
             WorkerSupervisionAction::Restart);
    const WorkerAttemptId second = *supervisor.beginAttempt(activation, 210);
    QVERIFY(second != first);
    QCOMPARE(supervisor.workerExited({activation, second}, WorkerExitReason::Crashed, 300),
             WorkerSupervisionAction::CrashLoopRollback);
    QCOMPARE(restarts, 1);
    QCOMPARE(rollbacks, 1);
    QCOMPARE(supervisor.state(), WorkerSupervisorState::Retired);
    QCOMPARE(supervisor.workerExited({activation, second}, WorkerExitReason::Crashed, 301),
             WorkerSupervisionAction::IgnoredDuplicateFailure);
    QCOMPARE(rollbacks, 1);
}

void WorkerCrashTest::timeoutAndExitOfSameAttemptCountOnce()
{
    int restarts = 0;
    WorkerSupervisor supervisor({5000, 1000},
                                [&](WorkerActivationId) { ++restarts; },
                                [](WorkerActivationId) {});
    const WorkerActivationId activation = supervisor.beginActivation(0);
    const WorkerAttemptId attempt = *supervisor.beginAttempt(activation, 0);
    supervisor.heartbeat({activation, attempt}, 500);
    QCOMPARE(supervisor.checkHealth({activation, attempt}, 1501),
             WorkerSupervisionAction::Restart);
    QCOMPARE(supervisor.workerExited({activation, attempt}, WorkerExitReason::Crashed, 1502),
             WorkerSupervisionAction::IgnoredDuplicateFailure);
    QCOMPARE(restarts, 1);
}

void WorkerCrashTest::staleAttemptSignalsAndCleanShutdownAreIgnored()
{
    int restarts = 0;
    WorkerSupervisor supervisor({5000, 1000},
                                [&](WorkerActivationId) { ++restarts; },
                                [](WorkerActivationId) {});
    const WorkerActivationId oldActivation = supervisor.beginActivation(0);
    const WorkerAttemptId oldAttempt = *supervisor.beginAttempt(oldActivation, 0);
    const WorkerActivationId current = supervisor.beginActivation(100);
    const WorkerAttemptId currentAttempt = *supervisor.beginAttempt(current, 100);
    supervisor.heartbeat({oldActivation, oldAttempt}, 1000);
    QCOMPARE(supervisor.checkHealth({current, currentAttempt}, 1101),
             WorkerSupervisionAction::Restart);
    QCOMPARE(supervisor.workerExited({oldActivation, oldAttempt}, WorkerExitReason::Crashed, 200),
             WorkerSupervisionAction::IgnoredStaleAttempt);
    QCOMPARE(restarts, 1);

    WorkerSupervisor cleanSupervisor({5000, 1000},
                                     [&](WorkerActivationId) { ++restarts; },
                                     [](WorkerActivationId) {});
    const WorkerActivationId cleanActivation = cleanSupervisor.beginActivation(2000);
    const WorkerAttemptId cleanAttempt = *cleanSupervisor.beginAttempt(cleanActivation, 2000);
    QCOMPARE(cleanSupervisor.workerExited({cleanActivation, cleanAttempt},
                                          WorkerExitReason::Clean, 2100),
             WorkerSupervisionAction::None);
    QCOMPARE(cleanSupervisor.state(), WorkerSupervisorState::Retired);
    QCOMPARE(cleanSupervisor.checkHealth({cleanActivation, cleanAttempt}, 5000),
             WorkerSupervisionAction::None);
    QCOMPARE(restarts, 1);
}

void WorkerCrashTest::realLpacWorkerRestartsThenRollsBack()
{
    WorkerTestEnvironment environment;
    WorkerTestEnvironment previousEnvironment(QByteArrayLiteral(
        "import QtQuick\nRectangle { width: 100; height: 100; "
        "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", { kind: \"previous\" }) }\n"));
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(previousEnvironment.isValid(), qPrintable(previousEnvironment.error()));
    std::optional<WorkerTestEnvironment::Launch> live = environment.launch(
        QStringLiteral("crash-one"), QStringLiteral("crash-one"), 1000);
    QVERIFY2(live.has_value(), qPrintable(environment.error()));
    QVERIFY(completeHandshake(*live));
    const DWORD firstPid = live->process.processId();

    std::optional<WorkerAttemptId> restartedAttempt;
    std::optional<WorkerTestEnvironment::Launch> recovered;
    int rollbackCount = 0;
    WorkerSupervisor *supervisorPointer = nullptr;
    WorkerSupervisor supervisor(
        {5000, 2000},
        [&](const WorkerActivationId activation) {
            live = environment.launch(QStringLiteral("crash-two"),
                                      QStringLiteral("crash-two"), 1000);
            QVERIFY2(live.has_value(), qPrintable(environment.error()));
            QVERIFY(completeHandshake(*live));
            restartedAttempt = supervisorPointer->beginAttempt(activation, 210);
            QVERIFY(restartedAttempt.has_value());
        },
        [&](WorkerActivationId) {
            ++rollbackCount;
            recovered = previousEnvironment.launch(QStringLiteral("rollback-worker"),
                                                   QStringLiteral("rollback-worker"), 1000);
            QVERIFY2(recovered.has_value(), qPrintable(previousEnvironment.error()));
            QVERIFY(completeHandshake(*recovered));
            const auto previousRequest = recovered->hostSession.receive(5000);
            QCOMPARE(previousRequest.status, SessionStatus::MessageReady);
            QCOMPARE(previousRequest.message->type(), ProtocolType::Request);
            QCOMPARE(previousRequest.message->payload()
                         .value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("kind")).toString(),
                     QStringLiteral("previous"));
            const auto response = ProtocolMessage::successResponse(
                previousRequest.message->requestId(), {});
            QVERIFY(response.has_value());
            QVERIFY(recovered->hostSession.send(*response, 5000));
        });
    supervisorPointer = &supervisor;
    const WorkerActivationId activation = supervisor.beginActivation(100);
    const WorkerAttemptId firstAttempt = *supervisor.beginAttempt(activation, 100);

    terminateAndClose(*live);
    live.reset();
    QCOMPARE(supervisor.workerExited({activation, firstAttempt}, WorkerExitReason::Crashed, 200),
             WorkerSupervisionAction::Restart);
    QVERIFY(live.has_value());
    QVERIFY(restartedAttempt.has_value());
    QVERIFY(live->process.processId() != firstPid);

    terminateAndClose(*live);
    live.reset();
    QCOMPARE(supervisor.workerExited({activation, *restartedAttempt},
                                     WorkerExitReason::Crashed, 300),
             WorkerSupervisionAction::CrashLoopRollback);
    QCOMPARE(rollbackCount, 1);
    QVERIFY(recovered.has_value());
    QVERIFY(recovered->process.processId() != firstPid);

    QVERIFY(recovered->hostSession.send(
        *ProtocolMessage::shutdown(QStringLiteral("test.done")), 5000));
    QCOMPARE(recovered->hostSession.receive(5000).message->type(), ProtocolType::Shutdown);
    QVERIFY(recovered->process.waitForFinished(5000));
    const auto closed = recovered->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    WorkerCrashTest test;
    if (argc == 1) {
        char outputOption[] = "-o";
        char outputTarget[] = "-,txt";
        char *arguments[] = {argv[0], outputOption, outputTarget};
        return QTest::qExec(&test, 3, arguments);
    }
    return QTest::qExec(&test, argc, argv);
}

#include "tst_worker_crash.moc"
