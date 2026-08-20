#include "HostApplication.h"
#include "HostRuntimeConfig.h"
#include "InstalledPackageWorkerLauncherTestHooks.h"
#include "MainWindow.h"
#include "PackageInstaller.h"
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
#include <QSemaphore>
#include <QTest>
#include <QTemporaryDir>

#include <qt_windows.h>
#include <Aclapi.h>

#include <mutex>

namespace
{
bool writeNewFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}

bool protectTrustKey(const QString &path)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(
            const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
            SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS
        || descriptor == nullptr || owner == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemBytes = sizeof(systemBuffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer,
                            &systemBytes)) {
        LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESSW entries[2]{};
    for (EXPLICIT_ACCESSW &entry : entries) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(owner);
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemBuffer);
    PACL dacl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(2, entries, nullptr, &dacl);
    const DWORD applied = aclResult == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : aclResult;
    if (dacl != nullptr) LocalFree(dacl);
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
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

bool waitForProcessExit(const quint32 processId, const int timeoutMs)
{
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, static_cast<DWORD>(timeoutMs))
        == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}
}

class ProductionUpdateRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void signedInstalledPackagesDriveAutomaticRealWorkerRollback();
    void activationChangedBeforeProcessLaunchNeverAdmitsStaleWorker();
    void activationChangedBeforeHandshakeNeverAdmitsStaleWorker();
    void destroyedLauncherRetiresHandshakeCompletedInflightLaunch();
    void tempCleanupFailureStopsAdmissionAndCanBeRetried();
    void processWaitCleanupFailureIsFatalAndRetryable();
    void aclRestoreCleanupFailureIsFatalAndRetryable();
};

namespace
{
void runActivationAdmissionRace(const bool activateAfterProcessStart)
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QTemporaryDir trustRoot;
    QTemporaryDir telemetryRoot;
    QVERIFY(trustRoot.isValid());
    QVERIFY(telemetryRoot.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = trustRoot.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = telemetryRoot.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QByteArray qml = QByteArrayLiteral(
        "import QtQuick\nItem { width: 320; height: 200 }");
    const QString packageA = updateSignedPackage(
        temporary, QStringLiteral("binding-a"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem, false, qml, environment.appId());
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("binding-b"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem, false, qml, environment.appId());
    QVERIFY(!packageA.isEmpty());
    QVERIFY(!packageB.isEmpty());

    PackageStore competingStore(environment.packageRoot());
    InstallPolicy competingPolicy;
    competingPolicy.expectedAppId = environment.appId();
    competingPolicy.runtimeVersion = QStringLiteral("1.2.0");
    competingPolicy.allowedImports = {QStringLiteral("QtQuick"),
                                      QStringLiteral("Company.Design")};
    competingPolicy.preflight = [](const Manifest &, const QString &) {
        return true;
    };
    PackageInstaller competingInstaller(
        competingStore, keys.value().publicKeyPem, std::move(competingPolicy));
    std::atomic_bool activatedB = false;
    std::atomic_bool activationFinished = false;
    std::atomic_bool activationSucceeded = false;
    std::mutex activationResultMutex;
    QString activationStableError;
    const auto activateB = [&](const UpdateLaunchRequest &request) {
        if (request.packageVersion != QStringLiteral("1.0.0")
            || activatedB.exchange(true)) return;
        const InstallResult installed = competingInstaller.install(packageB);
        {
            std::lock_guard lock(activationResultMutex);
            activationStableError = installed.stableError;
        }
        activationSucceeded.store(installed.succeeded());
        activationFinished.store(true);
    };
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    if (activateAfterProcessStart) {
        hooks.afterProcessStartBeforeHandshake = activateB;
    } else {
        hooks.afterBindingValidationBeforeProcessLaunch = activateB;
    }
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));

    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + environment.packageRoot(),
        QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
        QStringLiteral("--worker-executable=") + environment.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--install-package=") + packageA,
        QStringLiteral("--health-window-ms=2000"),
        QStringLiteral("--heartbeat-timeout-ms=30000"),
    };
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(activatedB.load(), 30'000);
    QTRY_VERIFY_WITH_TIMEOUT(activationFinished.load(), 30'000);
    QString activationError;
    {
        std::lock_guard lock(activationResultMutex);
        activationError = activationStableError;
    }
    QVERIFY2(activationSucceeded.load(), qPrintable(activationError));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 30'000);

    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QCOMPARE(ready.count(), 0);
    const ActivationState stateAfterRejection = competingStore.activationState(
        environment.appId()).state;
    QVERIFY(stateAfterRejection.current.startsWith(QStringLiteral("1.1.0-")));
    QTest::qWait(500);
    QCOMPARE(competingStore.activationState(environment.appId()).state,
             stateAfterRejection);
    host.reset();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void runSandboxCloseFailure(const bool forceWaitTimeout)
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QTemporaryDir trustRoot;
    QTemporaryDir telemetryRoot;
    QVERIFY(temporary.isValid());
    QVERIFY(trustRoot.isValid());
    QVERIFY(telemetryRoot.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = trustRoot.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = telemetryRoot.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("close-failure"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem, false,
        QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
        environment.appId());
    QVERIFY(!package.isEmpty());
    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + environment.packageRoot(),
        QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
        QStringLiteral("--worker-executable=") + environment.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--install-package=") + package,
        QStringLiteral("--heartbeat-timeout-ms=30000"),
    };
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    QVERIFY(parsed.value.has_value());
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy exited(host.get(), &HostApplication::packageWorkerExited);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30'000);
    qbrowser_sandbox_testing::SandboxProcessTestHooks hooks;
    hooks.forceCloseWaitTimeout = forceWaitTimeout;
    if (!forceWaitTimeout) {
        hooks.failAclRestore = [](const QString &) { return true; };
    }
    qbrowser_sandbox_testing::setSandboxProcessTestHooks(std::move(hooks));
    QVERIFY(terminateProcessId(ready.first().at(5).toUInt()));
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 10'000);
    QCOMPARE(exited.count(), 0);
    const QString cleanupError = failed.last().at(0).toString();
    QCOMPARE(cleanupError,
             forceWaitTimeout
                 ? QStringLiteral("sandbox.process.wait_timeout")
                 : QStringLiteral("sandbox.acl.restore_failed"));

    qbrowser_sandbox_testing::resetSandboxProcessTestHooks();
    QVERIFY(host->retryWorkerCleanupForTesting());
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
    QCOMPARE(exited.count(), 0);
    host.reset();
}
}

void ProductionUpdateRuntimeTest::activationChangedBeforeProcessLaunchNeverAdmitsStaleWorker()
{
    runActivationAdmissionRace(false);
}

void ProductionUpdateRuntimeTest::activationChangedBeforeHandshakeNeverAdmitsStaleWorker()
{
    runActivationAdmissionRace(true);
}

void ProductionUpdateRuntimeTest::processWaitCleanupFailureIsFatalAndRetryable()
{
    runSandboxCloseFailure(true);
}

void ProductionUpdateRuntimeTest::aclRestoreCleanupFailureIsFatalAndRetryable()
{
    runSandboxCloseFailure(false);
}

void ProductionUpdateRuntimeTest::destroyedLauncherRetiresHandshakeCompletedInflightLaunch()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QTemporaryDir trustRoot;
    QTemporaryDir telemetryRoot;
    QVERIFY(temporary.isValid());
    QVERIFY(trustRoot.isValid());
    QVERIFY(telemetryRoot.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = trustRoot.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = telemetryRoot.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("inflight-destroy"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem, false,
        QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
        environment.appId());
    QVERIFY(!package.isEmpty());
    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + environment.packageRoot(),
        QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
        QStringLiteral("--worker-executable=") + environment.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--install-package=") + package,
    };
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    QVERIFY(parsed.value.has_value());
    QSemaphore releaseCompletion;
    std::atomic_bool handshakeCompleted = false;
    std::atomic<quint32> processId = 0;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32 pid) {
        processId.store(pid);
        handshakeCompleted.store(true);
        releaseCompletion.acquire();
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(handshakeCompleted.load(), 30'000);
    QVERIFY(processId.load() != 0);
    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    QVERIFY(destruction.elapsed() < 100);
    releaseCompletion.release();
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QVERIFY(waitForProcessExit(processId.load(), 10'000));
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void ProductionUpdateRuntimeTest::tempCleanupFailureStopsAdmissionAndCanBeRetried()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QTemporaryDir trustRoot;
    QTemporaryDir telemetryRoot;
    QVERIFY(temporary.isValid());
    QVERIFY(trustRoot.isValid());
    QVERIFY(telemetryRoot.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = trustRoot.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = telemetryRoot.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QByteArray qml = QByteArrayLiteral(
        "import QtQuick\nItem { width: 320; height: 200 }");
    const QString packageA = updateSignedPackage(
        temporary, QStringLiteral("cleanup-a"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem, false, qml, environment.appId());
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("cleanup-b"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem, false, qml, environment.appId());
    QVERIFY(!packageA.isEmpty());
    QVERIFY(!packageB.isEmpty());
    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--app-id=") + environment.appId(),
        QStringLiteral("--trusted-public-key=") + publicKey,
        QStringLiteral("--package-store=") + environment.packageRoot(),
        QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
        QStringLiteral("--worker-executable=") + environment.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry,
        QStringLiteral("--install-package=") + packageA,
        QStringLiteral("--heartbeat-timeout-ms=30000"),
    };
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    QVERIFY(parsed.value.has_value());
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30'000);
    const quint32 pid = ready.first().at(5).toUInt();
    const QString workers = QDir(environment.sandboxTempRoot()).filePath(
        QStringLiteral("workers"));
    const QString competitor = QDir(workers).filePath(
        QStringLiteral("competitor"));
    const QString marker = QDir(competitor).filePath(QStringLiteral("marker"));
    QVERIFY(QDir().mkdir(competitor));
    QVERIFY(writeNewFile(marker, QByteArrayLiteral("preserve")));
    std::atomic_bool failCleanup = true;
    std::atomic_int cleanupAttempts = 0;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.failWorkerTempCleanup = [&](const QString &path) {
        ++cleanupAttempts;
        return failCleanup.load()
            && QFileInfo(path).fileName().startsWith(QStringLiteral("launch-"));
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    QVERIFY(terminateProcessId(pid));
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 10'000);
    QVERIFY(QFileInfo::exists(marker));
    PackageStore observedStore(environment.packageRoot());
    const ActivationState stateBeforeRejectedInstall =
        observedStore.activationState(environment.appId()).state;
    QVERIFY(host->requestPackageInstall(packageB));
    QTest::qWait(2'000);
    QCOMPARE(ready.count(), 1);
    QCOMPARE(observedStore.activationState(environment.appId()).state,
             stateBeforeRejectedInstall);

    failCleanup.store(false);
    QVERIFY(host->retryWorkerCleanupForTesting());
    QTRY_VERIFY_WITH_TIMEOUT(cleanupAttempts.load() >= 2, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        QDir(workers).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
        QStringList{QStringLiteral("competitor")}, 10'000);
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QVERIFY(QFileInfo::exists(marker));
    host.reset();
    QVERIFY(QFile::remove(marker));
    QVERIFY(QDir().rmdir(competitor));
}

void ProductionUpdateRuntimeTest::signedInstalledPackagesDriveAutomaticRealWorkerRollback()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QTemporaryDir trustRoot;
    QTemporaryDir telemetryRoot;
    QVERIFY(trustRoot.isValid());
    QVERIFY(telemetryRoot.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString publicKey = trustRoot.filePath(QStringLiteral("trusted.pem"));
    const QString telemetry = telemetryRoot.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
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
        keys.value().privateKeyPem, false, recoveredQml, environment.appId(),
        QStringLiteral("qml/Start.qml"));
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
    QVERIFY(QFileInfo::exists(readyPath(1) + QStringLiteral("/qml/Start.qml")));
    QVERIFY(!QFileInfo::exists(readyPath(1) + QStringLiteral("/qml/Main.qml")));
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
    const quint32 liveWorker = readyPid(4);
    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    QVERIFY2(destruction.elapsed() < 100,
             "Host destruction blocked on the lifecycle thread");
    QVERIFY(waitForProcessExit(liveWorker, 10'000));
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
    QTest::qWait(500);
    QCOMPARE(ready.count(), readyBeforeShutdownRace);

    const auto startedEventCount = [](const QString &directory) {
        QFile events(QDir(directory).filePath(QStringLiteral("events.jsonl")));
        if (!events.open(QIODevice::ReadOnly)) return qsizetype{0};
        return events.readAll().count(QByteArrayLiteral("\"code\":\"started\""));
    };
    const QString cliTelemetry = telemetryRoot.filePath(
        QStringLiteral("cli-telemetry"));
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
