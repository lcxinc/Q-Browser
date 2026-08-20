#include "HostApplication.h"
#include "HostRuntimeConfig.h"
#include "MainWindow.h"
#include "PackageStore.h"
#include "UpdateTestSupport.h"
#include "WorkerTestEnvironment.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QProcess>
#include <QTest>

#include <qt_windows.h>

namespace
{
bool writeNewFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}

bool terminateProcessId(const quint32 processId)
{
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                       FALSE, processId);
    if (process == nullptr) return false;
    const bool terminated = TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE;
    const bool finished = terminated && WaitForSingleObject(process, 10'000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return finished;
}
}

class ProductionUpdateRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void signedInstalledPackagesDriveAutomaticRealWorkerRollback();
};

void ProductionUpdateRuntimeTest::signedInstalledPackagesDriveAutomaticRealWorkerRollback()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = temporary.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = temporary.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    const QByteArray recoveredQml = QByteArrayLiteral(
        "import QtQuick\nItem { width: 320; height: 200; "
        "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
        "{ kind: \"lkgRecovered\" }) }");
    const QByteArray simpleQml = QByteArrayLiteral(
        "import QtQuick\nItem { width: 320; height: 200 }");
    const QString one = updateSignedPackage(
        temporary, QStringLiteral("production-one"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem, false, simpleQml, environment.appId());
    const QString two = updateSignedPackage(
        temporary, QStringLiteral("production-two"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem, false, recoveredQml, environment.appId());
    const QString crashing = updateSignedPackage(
        temporary, QStringLiteral("production-crashing"), QStringLiteral("1.2.0"),
        keys.value().privateKeyPem, false, simpleQml, environment.appId());
    const QString cli = updateSignedPackage(
        temporary, QStringLiteral("production-cli"), QStringLiteral("1.3.0"),
        keys.value().privateKeyPem, false, simpleQml, environment.appId());
    QVERIFY(!one.isEmpty());
    QVERIFY(!two.isEmpty());
    QVERIFY(!crashing.isEmpty());
    QVERIFY(!cli.isEmpty());

    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + environment.packageRoot(),
        QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
        QStringLiteral("--worker-executable=") + environment.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--install-package=") + one,
        QStringLiteral("--health-window-ms=2000"),
        QStringLiteral("--heartbeat-timeout-ms=30000"),
    };
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy exited(host.get(), &HostApplication::packageWorkerExited);
    QSignalSpy capability(host.get(), &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy failure(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || !failure.isEmpty(), 30'000);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty() ? "" : qPrintable(failure.first().at(0).toString()));
    QCOMPARE(ready.count(), 1);

    PackageStore observedStore(environment.packageRoot());
    const auto readyVersion = [&ready](const int index) {
        return ready.at(index).at(1).toString();
    };
    const auto readyPath = [&ready](const int index) {
        return ready.at(index).at(2).toString();
    };
    const auto readyPid = [&ready](const int index) {
        return ready.at(index).at(5).toUInt();
    };
    QCOMPARE(readyVersion(0), QStringLiteral("1.0.0"));
    QCOMPARE(QFileInfo(readyPath(0)).canonicalFilePath(),
             QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
                 .canonicalFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(
        observedStore.activationState(environment.appId()).state.lastKnownGood
            == QFileInfo(readyPath(0)).fileName(),
        10'000);

    QVERIFY(host->requestPackageInstall(two));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 2 || !failure.isEmpty(), 30'000);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty() ? "" : qPrintable(failure.last().at(0).toString()));
    QCOMPARE(ready.count(), 2);
    QCOMPARE(readyVersion(1), QStringLiteral("1.1.0"));
    QCOMPARE(QFileInfo(readyPath(1)).canonicalFilePath(),
             QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
                 .canonicalFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(
        observedStore.activationState(environment.appId()).state.lastKnownGood
            == QFileInfo(readyPath(1)).fileName(),
        10'000);

    QVERIFY(host->requestPackageInstall(crashing));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 3 || !failure.isEmpty(), 30'000);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty() ? "" : qPrintable(failure.last().at(0).toString()));
    QCOMPARE(ready.count(), 3);
    QCOMPARE(readyVersion(2), QStringLiteral("1.2.0"));
    QVERIFY(terminateProcessId(readyPid(2)));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 4, 30'000);
    QCOMPARE(readyVersion(3), QStringLiteral("1.2.0"));
    QVERIFY(terminateProcessId(readyPid(3)));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 2, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 5, 30'000);
    QCOMPARE(readyVersion(4), QStringLiteral("1.1.0"));
    QCOMPARE(QFileInfo(readyPath(4)).canonicalFilePath(),
             QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
                 .canonicalFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(capability.count() >= 2, 10'000);
    QCOMPARE(capability.last().at(0).toString(), QStringLiteral("storage"));
    QCOMPARE(capability.last().at(1).toString(), QStringLiteral("get"));
    QCOMPARE(capability.last().at(2).toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("lkgRecovered"));
    QCOMPARE(failure.count(), 0);
    QVERIFY(QCoreApplication::instance() != nullptr);
    const int readyBeforeShutdownRace = ready.count();
    QVERIFY(terminateProcessId(readyPid(4)));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 3, 10'000);
    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    QVERIFY2(destruction.elapsed() < 250,
             "Host destruction blocked on the lifecycle thread");
    QTest::qWait(500);
    QCOMPARE(ready.count(), readyBeforeShutdownRace);

    const auto startedEventCount = [](const QString &directory) {
        QFile events(QDir(directory).filePath(QStringLiteral("events.jsonl")));
        if (!events.open(QIODevice::ReadOnly)) return qsizetype{0};
        return events.readAll().count(QByteArrayLiteral("\"code\":\"started\""));
    };
    const QString cliTelemetry = temporary.filePath(QStringLiteral("cli-telemetry"));
    QVERIFY(QDir().mkpath(cliTelemetry));
    QStringList installArguments = arguments;
    for (QString &argument : installArguments) {
        if (argument.startsWith(QStringLiteral("--install-package="))) {
            argument = QStringLiteral("--install-package=") + cli;
        } else if (argument.startsWith(
                       QStringLiteral("--telemetry-directory="))) {
            argument = QStringLiteral("--telemetry-directory=") + cliTelemetry;
        }
    }
    QProcess productionInstallHost;
    productionInstallHost.setProgram(QString::fromUtf8(Q_BROWSER_HOST_PATH));
    productionInstallHost.setArguments(installArguments);
    productionInstallHost.start();
    QVERIFY2(productionInstallHost.waitForStarted(10'000),
             qPrintable(productionInstallHost.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
            .fileName().startsWith(QStringLiteral("1.3.0-")),
        30'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        observedStore.activationState(environment.appId()).state.lastKnownGood
            .startsWith(QStringLiteral("1.3.0-")),
        30'000);
    QTRY_VERIFY_WITH_TIMEOUT(startedEventCount(cliTelemetry) > 0, 30'000);
    productionInstallHost.terminate();
    if (!productionInstallHost.waitForFinished(10'000)) {
        productionInstallHost.kill();
        QVERIFY(productionInstallHost.waitForFinished(10'000));
    }
    QCOMPARE(productionInstallHost.state(), QProcess::NotRunning);

    const qsizetype startsBeforeOfflineCli = startedEventCount(cliTelemetry);
    const qint64 generationBeforeOffline = observedStore.activationState(
        environment.appId()).state.generation;
    QStringList offlineArguments = installArguments;
    offlineArguments.removeIf([](const QString &argument) {
        return argument.startsWith(QStringLiteral("--install-package="));
    });
    QProcess productionHost;
    productionHost.setProgram(QString::fromUtf8(Q_BROWSER_HOST_PATH));
    productionHost.setArguments(offlineArguments);
    productionHost.start();
    QVERIFY2(productionHost.waitForStarted(10'000),
             qPrintable(productionHost.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(
        observedStore.activationState(environment.appId()).state.generation
            > generationBeforeOffline,
        30'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        startedEventCount(cliTelemetry) > startsBeforeOfflineCli, 30'000);
    productionHost.terminate();
    if (!productionHost.waitForFinished(10'000)) {
        productionHost.kill();
        QVERIFY(productionHost.waitForFinished(10'000));
    }
    QCOMPARE(productionHost.state(), QProcess::NotRunning);
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    ProductionUpdateRuntimeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_production_update_runtime.moc"
