#include "HostApplication.h"
#include "HostRuntimeConfig.h"
#include "InstalledPackageWorkerLauncherTestHooks.h"
#include "MainWindow.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "RuntimePackageAuthority.h"
#include "UpdateTestSupport.h"
#include "WorkerTestEnvironment.h"
#include "WorkerRetirementManager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTest>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <qt_windows.h>
#include <Aclapi.h>

#include <future>
#include <mutex>

namespace
{
bool writeNewFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}

HostRuntimeConfigResult parseHostArguments(const QStringList &arguments,
                                           const QString &storage)
{
    QStringList completeArguments = arguments;
    const QString storageArgument = QStringLiteral("--storage-directory=") + storage;
    if (!completeArguments.contains(storageArgument)) {
        completeArguments.push_back(storageArgument);
    }
    return HostRuntimeConfig::fromArguments(completeArguments);
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

struct CloseWindowRequest final
{
    DWORD processId = 0;
    bool posted = false;
};

BOOL CALLBACK closeOwnedTopLevelWindow(const HWND window, const LPARAM parameter)
{
    auto *const request = reinterpret_cast<CloseWindowRequest *>(parameter);
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(window, &processId);
    if (request != nullptr && processId == request->processId
        && GetWindow(window, GW_OWNER) == nullptr
        && PostMessageW(window, WM_CLOSE, 0, 0) != FALSE) {
        request->posted = true;
    }
    return TRUE;
}

bool stopOwnedProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning) return true;
    CloseWindowRequest close{static_cast<DWORD>(process.processId()), false};
    (void)EnumWindows(closeOwnedTopLevelWindow,
                      reinterpret_cast<LPARAM>(&close));
    if (close.posted && process.waitForFinished(5'000)) {
        process.close();
        return true;
    }
    process.terminate();
    if (!process.waitForFinished(5'000)) {
        process.kill();
        if (!process.waitForFinished(10'000)) return false;
    }
    process.close();
    return process.state() == QProcess::NotRunning;
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

void recordProductionPhase(const QByteArray &phase)
{
    QFile file(QDir::temp().filePath(
        QStringLiteral("qbrowser-production-update-stage.txt")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        (void)file.write(QByteArray::number(GetTickCount64()));
        (void)file.write(" ");
        (void)file.write(phase);
        (void)file.write("\n");
    }
}

QByteArray readDiagnosticFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    constexpr qsizetype maximumDiagnosticBytes = 4 * 1024;
    if (bytes.size() > maximumDiagnosticBytes)
        bytes = bytes.last(maximumDiagnosticBytes);
    return bytes;
}

qsizetype telemetryEventCount(const QString &directory, const QByteArray &code)
{
    QFile events(QDir(directory).filePath(QStringLiteral("events.jsonl")));
    if (!events.open(QIODevice::ReadOnly)) return 0;
    return events.readAll().count(QByteArrayLiteral("\"code\":\"")
                                  + code + QByteArrayLiteral("\""));
}

template<typename Complete, typename Terminal>
bool waitForExternalHost(QProcess &process,
                         Complete complete,
                         Terminal terminal,
                         const int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!complete() && !terminal()
           && process.state() != QProcess::NotRunning
           && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return complete();
}

QString externalHostDiagnostic(QProcess &process,
                               PackageStore &store,
                               const QString &appId,
                               const QString &telemetry,
                               const QString &standardOutput,
                               const QString &standardError)
{
    const ActivationStateResult activation = store.activationState(appId);
    const QString current = activation.hasValue()
        ? activation.state.current : QStringLiteral("<unavailable>");
    const QString lastKnownGood = activation.hasValue()
        ? activation.state.lastKnownGood : QStringLiteral("<unavailable>");
    return QStringLiteral(
        "state=%1 exitCode=%2 current=%3 lkg=%4 telemetry=%5 stdout=%6 stderr=%7")
        .arg(static_cast<int>(process.state()))
        .arg(process.state() == QProcess::NotRunning ? process.exitCode() : -1)
        .arg(current, lastKnownGood,
             QString::fromUtf8(readDiagnosticFile(
                 QDir(telemetry).filePath(QStringLiteral("events.jsonl")))),
             QString::fromUtf8(readDiagnosticFile(standardOutput)),
             QString::fromUtf8(readDiagnosticFile(standardError)));
}
}

class ProductionUpdateRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void signedInstalledPackagesDriveAutomaticRealWorkerRollback();
    void activationChangedBeforeProcessLaunchNeverAdmitsStaleWorker();
    void activationChangedBeforeHandshakeNeverAdmitsStaleWorker();
    void activationChangedAfterHandshakeBeforeAdmissionNeverAttaches();
    void staleAdmissionFailureLetsReplacementBecomeHealthy();
    void destroyedLauncherRetiresHandshakeCompletedInflightLaunch();
    void hostDestroyBeforeFirstValidationKeepsAuthorityAlive();
    void hostDestroyBetweenValidationsKeepsAuthorityAlive();
    void tempCleanupFailureStopsAdmissionAndCanBeRetried();
    void cleanupFailureOutlivesDestroyedHostAndAutoRecovers();
    void repeatedLiveCancellationLeavesNoObserversOrContexts();
    void admissionTimeoutRejectsLateAcceptedReplay();
    void lifecycleQueueFullBeforeAdmissionNeverAttaches();
    void processWaitCleanupFailureIsFatalAndRetryable();
    void aclRestoreCleanupFailureIsFatalAndRetryable();
    void readyHandlerCanDestroyHostWithoutUseAfterFree();
    void launchThreadStartFailureRetiresSynchronously();
    void observerThreadStartFailureRetiresSynchronously();
    void launchThreadStartFailureHandlerCanDestroyHost();
    void observerThreadStartFailureHandlerCanDestroyHost();
    void retirementThreadStartFailureIsFatalAndRetryable();
    void checkedShutdownWaitsForInflightLaunchRegistration();
};

namespace
{
enum class ActivationRacePoint
{
    BeforeProcessLaunch,
    BeforeHandshake,
    BeforeAdmission,
};

void runHostDestroyDuringValidation(const bool beforeFirstValidation)
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("authority-lifetime"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());

    QSemaphore entered;
    QSemaphore release;
    std::atomic_bool barrierEntered = false;
    const auto barrier = [&](const UpdateLaunchRequest &) {
        barrierEntered.store(true, std::memory_order_release);
        entered.release();
        release.acquire();
    };
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    if (beforeFirstValidation) {
        hooks.beforeBindingValidation = barrier;
    } else {
        hooks.afterBindingValidationBeforeProcessLaunch = barrier;
    }
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    const qsizetype liveBefore = RuntimePackageAuthority::liveCountForTesting();
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(
        barrierEntered.load(std::memory_order_acquire), 30'000);
    QVERIFY(entered.tryAcquire());
    QCOMPARE(RuntimePackageAuthority::liveCountForTesting(), liveBefore + 1);
    host.reset();
    QCOMPARE(RuntimePackageAuthority::liveCountForTesting(), liveBefore + 1);
    if (beforeFirstValidation) QTest::qWait(2'000);
    release.release();
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QTRY_COMPARE_WITH_TIMEOUT(
        RuntimePackageAuthority::liveCountForTesting(), liveBefore, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void runActivationAdmissionRace(const ActivationRacePoint racePoint)
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QByteArray qml = racePoint == ActivationRacePoint::BeforeAdmission
        ? QByteArrayLiteral(
              "import QtQuick\nItem { width: 320; height: 200; "
              "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
              "{ kind: \"staleAdmission\" }) }")
        : QByteArrayLiteral(
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
    const auto activateB = [&] {
        if (activatedB.exchange(true)) return;
        const InstallResult installed = competingInstaller.install(packageB);
        {
            std::lock_guard lock(activationResultMutex);
            activationStableError = installed.stableError;
        }
        activationSucceeded.store(installed.succeeded());
        activationFinished.store(true);
    };
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    const auto activateForRequest = [&](const UpdateLaunchRequest &request) {
        if (request.packageVersion == QStringLiteral("1.0.0")) activateB();
    };
    switch (racePoint) {
    case ActivationRacePoint::BeforeProcessLaunch:
        hooks.afterBindingValidationBeforeProcessLaunch = activateForRequest;
        break;
    case ActivationRacePoint::BeforeHandshake:
        hooks.afterProcessStartBeforeHandshake = activateForRequest;
        break;
    case ActivationRacePoint::BeforeAdmission:
        hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32) {
            activateB();
        };
        break;
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QSignalSpy capability(
        host.get(), &HostApplication::workerCapabilityRequestObserved);
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
    QCOMPARE(capability.count(), 0);
    QVERIFY(!host->hasWorkerContext());
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
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

void runLauncherThreadStartFailure(const bool observerFailure)
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary,
        observerFailure ? QStringLiteral("observer-thread-failure")
                        : QStringLiteral("launch-thread-failure"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));

    const qsizetype contextsBefore =
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts();
    const qsizetype launchesBefore =
        qbrowser_host_testing::installedPackageWorkerActiveLaunchThreads();
    const qsizetype observersBefore =
        qbrowser_host_testing::installedPackageWorkerActiveObservers();
    std::atomic<quint32> processId = 0;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.failLaunchThreadStart = !observerFailure;
    hooks.failObserverThreadStart = observerFailure;
    hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32 pid) {
        processId.store(pid, std::memory_order_release);
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty() || !ready.isEmpty(), 30'000);

    const QString stableError = failed.isEmpty()
        ? QString{} : failed.first().at(0).toString();
    const int readyCount = ready.count();
    if (processId.load(std::memory_order_acquire) == 0 && readyCount != 0) {
        processId.store(ready.first().at(5).toUInt(), std::memory_order_release);
    }
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    const qint64 destructionMs = destruction.elapsed();
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    const quint32 observedProcess = processId.load(std::memory_order_acquire);
    const bool processExited = observedProcess == 0
        || waitForProcessExit(observedProcess, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerActiveLaunchThreads(),
        launchesBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerActiveObservers(),
        observersBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts(),
        contextsBefore, 10'000);
    const QStringList workerEntries = QDir(
        QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
        .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    QCOMPARE(readyCount, 0);
    QCOMPARE(stableError,
             observerFailure
                 ? QStringLiteral("host.launch.observer_thread_unavailable")
                 : QStringLiteral("host.launch.launch_thread_unavailable"));
    QVERIFY(destructionMs < 100);
    QVERIFY(processExited);
    QVERIFY(workerEntries.isEmpty());
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
}

void runThreadStartFailureDestroyingHost(const bool observerFailure)
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary,
        observerFailure ? QStringLiteral("observer-thread-destroy-host")
                        : QStringLiteral("launch-thread-destroy-host"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));

    const qsizetype authoritiesBefore = RuntimePackageAuthority::liveCountForTesting();
    const qsizetype contextsBefore =
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts();
    const qsizetype launchesBefore =
        qbrowser_host_testing::installedPackageWorkerActiveLaunchThreads();
    const qsizetype observersBefore =
        qbrowser_host_testing::installedPackageWorkerActiveObservers();
    std::atomic<quint32> processId = 0;
    std::atomic_int continuedAfterFailureSignal = 0;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.failLaunchThreadStart = !observerFailure;
    hooks.failObserverThreadStart = observerFailure;
    hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32 pid) {
        processId.store(pid, std::memory_order_release);
    };
    hooks.afterFailureSignalBeforeLifecycleEnqueue = [&] {
        continuedAfterFailureSignal.fetch_add(1, std::memory_order_relaxed);
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));

    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    std::atomic_bool destroyedFromFailure = false;
    QString stableError;
    qint64 destructionMs = -1;
    QObject::connect(
        host.get(), &HostApplication::updateLifecycleFailed,
        QCoreApplication::instance(),
        [&](const QString &error) {
            stableError = error;
            QElapsedTimer destruction;
            destruction.start();
            host.reset();
            destructionMs = destruction.elapsed();
            destroyedFromFailure.store(true, std::memory_order_release);
        },
        Qt::DirectConnection);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(
        destroyedFromFailure.load(std::memory_order_acquire), 30'000);

    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    std::atomic_bool guiResponsive = false;
    QTimer::singleShot(0, QCoreApplication::instance(), [&] {
        guiResponsive.store(true, std::memory_order_release);
    });
    QTRY_VERIFY_WITH_TIMEOUT(guiResponsive.load(std::memory_order_acquire), 1'000);
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    const quint32 observedProcess = processId.load(std::memory_order_acquire);
    QVERIFY(observedProcess == 0 || waitForProcessExit(observedProcess, 10'000));
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerActiveLaunchThreads(),
        launchesBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerActiveObservers(),
        observersBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts(),
        contextsBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(RuntimePackageAuthority::liveCountForTesting(),
                              authoritiesBefore, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);

    QCOMPARE(ready.count(), 0);
    QCOMPARE(stableError,
             observerFailure
                 ? QStringLiteral("host.launch.observer_thread_unavailable")
                 : QStringLiteral("host.launch.launch_thread_unavailable"));
    QCOMPARE(continuedAfterFailureSignal.load(std::memory_order_relaxed), 0);
    QVERIFY(destructionMs >= 0);
    QVERIFY(destructionMs < 100);
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
}
}

void ProductionUpdateRuntimeTest::hostDestroyBeforeFirstValidationKeepsAuthorityAlive()
{
    runHostDestroyDuringValidation(true);
}

void ProductionUpdateRuntimeTest::hostDestroyBetweenValidationsKeepsAuthorityAlive()
{
    runHostDestroyDuringValidation(false);
}

void ProductionUpdateRuntimeTest::activationChangedBeforeProcessLaunchNeverAdmitsStaleWorker()
{
    runActivationAdmissionRace(ActivationRacePoint::BeforeProcessLaunch);
}

void ProductionUpdateRuntimeTest::activationChangedBeforeHandshakeNeverAdmitsStaleWorker()
{
    runActivationAdmissionRace(ActivationRacePoint::BeforeHandshake);
}

void ProductionUpdateRuntimeTest::activationChangedAfterHandshakeBeforeAdmissionNeverAttaches()
{
    runActivationAdmissionRace(ActivationRacePoint::BeforeAdmission);
}

void ProductionUpdateRuntimeTest::staleAdmissionFailureLetsReplacementBecomeHealthy()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString packageA = updateSignedPackage(
        temporary, QStringLiteral("stale-admission-a"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral(
            "import QtQuick\nItem { width: 320; height: 200; "
            "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
            "{ kind: \"staleA\" }) }"),
        environment.appId());
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("stale-admission-b"),
        QStringLiteral("1.1.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
        environment.appId());
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
        QStringLiteral("--health-window-ms=500"),
        QStringLiteral("--heartbeat-timeout-ms=30000"),
    };
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));

    std::atomic_bool beganB = false;
    std::atomic_bool beginBSucceeded = false;
    std::atomic<quint32> processA = 0;
    std::mutex beginMutex;
    QString beginError;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32 processId) {
        quint32 expected = 0;
        (void)processA.compare_exchange_strong(expected, processId);
    };
    hooks.beforeAdmissionDecision = [&](
        const UpdateLaunchRequest &request,
        UpdateLifecycleCoordinator &coordinator) {
        if (request.packageVersion != QStringLiteral("1.0.0")
            || beganB.exchange(true)) {
            return;
        }
        const UpdateLifecycleResult result = coordinator.installAndLaunch(packageB);
        beginBSucceeded.store(result.succeeded(), std::memory_order_release);
        std::lock_guard lock(beginMutex);
        beginError = result.stableError;
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy capability(
        host.get(), &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(beganB.load(std::memory_order_acquire), 30'000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30'000);
    PackageStore observedStore(environment.packageRoot());
    QTRY_VERIFY_WITH_TIMEOUT(
        telemetryEventCount(telemetry, QByteArrayLiteral("healthy")) >= 1,
        10'000);
    QVERIFY(observedStore.activationState(environment.appId())
                .state.lastKnownGood.startsWith(QStringLiteral("1.1.0-")));

    const bool replacementBegan = beginBSucceeded.load(std::memory_order_acquire);
    QString replacementError;
    {
        std::lock_guard lock(beginMutex);
        replacementError = beginError;
    }
    const int failedCount = failed.count();
    const int readyCount = ready.count();
    const QString readyVersion = readyCount == 1
        ? ready.first().at(1).toString() : QString{};
    const quint32 processB = readyCount == 1
        ? ready.first().at(5).toUInt() : 0;
    const int capabilityCount = capability.count();
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    const qint64 destructionMs = destruction.elapsed();
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    const bool aExited = processA.load(std::memory_order_acquire) != 0
        && waitForProcessExit(processA.load(std::memory_order_acquire), 10'000);
    const bool bExited = processB != 0 && waitForProcessExit(processB, 10'000);
    const QStringList workerEntries = QDir(
        QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
        .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    QVERIFY2(replacementBegan, qPrintable(replacementError));
    QCOMPARE(readyCount, 1);
    QCOMPARE(readyVersion, QStringLiteral("1.1.0"));
    QCOMPARE(failedCount, 0);
    QCOMPARE(capabilityCount, 0);
    QVERIFY(processA.load(std::memory_order_acquire) != processB);
    QVERIFY(aExited);
    QVERIFY(bExited);
    QVERIFY(destructionMs < 100);
    QVERIFY(workerEntries.isEmpty());
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
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

void ProductionUpdateRuntimeTest::cleanupFailureOutlivesDestroyedHostAndAutoRecovers()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("manager-outlives-host"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
        environment.appId());
    const QString persistentPackage = updateSignedPackage(
        temporary, QStringLiteral("manager-persistent"),
        QStringLiteral("1.1.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
        environment.appId());
    QVERIFY(!package.isEmpty());
    QVERIFY(!persistentPackage.isEmpty());
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QVERIFY(host->start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30'000);
    const quint32 pid = ready.first().at(5).toUInt();

    QSemaphore releaseFirstFailure;
    std::atomic_int cleanupAttempts = 0;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.failWorkerTempCleanup = [&](const QString &) {
        const int attempt = cleanupAttempts.fetch_add(1) + 1;
        if (attempt == 1) {
            releaseFirstFailure.acquire();
            return true;
        }
        return false;
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    QVERIFY(terminateProcessId(pid));
    QTRY_COMPARE_WITH_TIMEOUT(cleanupAttempts.load(), 1, 10'000);

    QElapsedTimer destruction;
    destruction.start();
    host.reset();
    QVERIFY(destruction.elapsed() < 100);
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    releaseFirstFailure.release();
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    const QString workers = QDir(environment.sandboxTempRoot()).filePath(
        QStringLiteral("workers"));
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(workers).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);

    QStringList persistentArguments = arguments;
    for (QString &argument : persistentArguments) {
        if (argument.startsWith(QStringLiteral("--install-package="))) {
            argument = QStringLiteral("--install-package=") + persistentPackage;
        }
    }
    HostRuntimeConfigResult persistentParsed =
        parseHostArguments(persistentArguments, storage);
    QVERIFY(persistentParsed.value.has_value());
    auto persistentHost = std::make_unique<HostApplication>(
        std::move(*persistentParsed.value));
    QSignalSpy persistentReady(
        persistentHost.get(), &HostApplication::packageWorkerReady);
    QVERIFY(persistentHost->start());
    QTRY_COMPARE_WITH_TIMEOUT(persistentReady.count(), 1, 30'000);
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks persistentHooks;
    persistentHooks.failWorkerTempCleanup = [](const QString &) { return true; };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(persistentHooks));
    QVERIFY(terminateProcessId(persistentReady.first().at(5).toUInt()));
    QTRY_COMPARE_WITH_TIMEOUT(
        WorkerRetirementManager::instance().status().fatal, qsizetype(1), 10'000);
    destruction.restart();
    persistentHost.reset();
    QVERIFY(destruction.elapsed() < 100);
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QVERIFY(WorkerRetirementManager::instance().retryFatal());
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(workers).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void ProductionUpdateRuntimeTest::repeatedLiveCancellationLeavesNoObserversOrContexts()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    QStringList packages;
    for (int iteration = 0; iteration < 3; ++iteration) {
        packages.push_back(updateSignedPackage(
            temporary,
            QStringLiteral("cancel-repeat-%1").arg(iteration),
            QStringLiteral("1.%1.0").arg(iteration),
            keys.value().privateKeyPem, false,
            QByteArrayLiteral("import QtQuick\nItem { width: 320; height: 200 }"),
            environment.appId()));
        QVERIFY(!packages.back().isEmpty());
    }
    const qsizetype contextsBefore =
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts();
    const qsizetype observersBefore =
        qbrowser_host_testing::installedPackageWorkerActiveObservers();
    for (int iteration = 0; iteration < 3; ++iteration) {
        const QStringList arguments{
            QStringLiteral("--package-mode"),
            QStringLiteral("--app-id=") + environment.appId(),
            QStringLiteral("--trusted-public-key=") + publicKey,
            QStringLiteral("--package-store=") + environment.packageRoot(),
            QStringLiteral("--sandbox-temp=") + environment.sandboxTempRoot(),
            QStringLiteral("--runtime-root=") + environment.runtimeRoot(),
            QStringLiteral("--worker-executable=") + environment.workerExecutable(),
            QStringLiteral("--telemetry-directory=") + telemetry,
            QStringLiteral("--install-package=") + packages.at(iteration),
            QStringLiteral("--heartbeat-timeout-ms=30000"),
        };
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
        QVERIFY(parsed.value.has_value());
        auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
        QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
        QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
        QVERIFY(host->start());
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || !failed.isEmpty(), 30'000);
        QVERIFY2(ready.count() == 1,
                 failed.isEmpty()
                     ? "worker did not become ready"
                     : qPrintable(failed.last().at(0).toString()));
        const quint32 pid = ready.first().at(5).toUInt();
        QElapsedTimer destruction;
        destruction.start();
        host.reset();
        QVERIFY(destruction.elapsed() < 100);
        QVERIFY(WorkerRetirementManager::instance().flush(10'000));
        QVERIFY(waitForProcessExit(pid, 10'000));
        QTRY_COMPARE_WITH_TIMEOUT(
            qbrowser_host_testing::installedPackageWorkerActiveObservers(),
            observersBefore, 10'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts(),
            contextsBefore, 10'000);
    }
}

void ProductionUpdateRuntimeTest::admissionTimeoutRejectsLateAcceptedReplay()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("admission-timeout"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral(
            "import QtQuick\nItem { width: 320; height: 200; "
            "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
            "{ kind: \"lateAdmission\" }) }"),
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());
    QSemaphore releaseAdmission;
    std::atomic_bool admissionBlocked = false;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.beforeAdmissionDecision = [&](const UpdateLaunchRequest &,
                                        UpdateLifecycleCoordinator &) {
        admissionBlocked.store(true, std::memory_order_release);
        releaseAdmission.acquire();
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    std::unique_ptr<HostApplication> host;
    bool admissionCleanupComplete = false;
    [[maybe_unused]] const auto admissionCleanup = qScopeGuard([&] {
        if (admissionCleanupComplete) return;
        qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
        releaseAdmission.release();
        host.reset();
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy capability(
        host.get(), &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(
        admissionBlocked.load(std::memory_order_acquire), 60'000);
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 10'000);
    QCOMPARE(failed.last().at(0).toString(),
             QStringLiteral("host.launch.admission_timeout"));
    QCOMPARE(ready.count(), 0);
    QCOMPARE(capability.count(), 0);
    QVERIFY(!host->hasWorkerContext());

    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    releaseAdmission.release();
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QTest::qWait(500);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(capability.count(), 0);
    QVERIFY(!host->hasWorkerContext());
    host.reset();
    admissionCleanupComplete = true;
}

void ProductionUpdateRuntimeTest::lifecycleQueueFullBeforeAdmissionNeverAttaches()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("admission-queue-full"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem, false,
        QByteArrayLiteral(
            "import QtQuick\nItem { width: 320; height: 200; "
            "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
            "{ kind: \"queueFullAdmission\" }) }"),
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());
    HostApplication *hostPointer = nullptr;
    std::atomic_bool queueHookInvoked = false;
    std::atomic_bool queueMutationSucceeded = false;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.afterHandshakeBeforeCompletionQueued = [&](const quint32) {
        queueHookInvoked.store(true, std::memory_order_release);
        if (hostPointer == nullptr) return;
        queueMutationSucceeded.store(QMetaObject::invokeMethod(
            hostPointer,
            [hostPointer] {
                hostPointer->forceLifecycleQueueFullForTesting(true);
            }, Qt::BlockingQueuedConnection), std::memory_order_release);
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    hostPointer = host.get();
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy capability(
        host.get(), &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 30'000);
    QVERIFY(queueHookInvoked.load(std::memory_order_acquire));
    QVERIFY(queueMutationSucceeded.load(std::memory_order_acquire));
    QCOMPARE(failed.last().at(0).toString(),
             QStringLiteral("host.launch.admission_unavailable"));
    QCOMPARE(ready.count(), 0);
    QCOMPARE(capability.count(), 0);
    QVERIFY(!host->hasWorkerContext());
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    host.reset();
}

void ProductionUpdateRuntimeTest::readyHandlerCanDestroyHostWithoutUseAfterFree()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("ready-destroy"), QStringLiteral("1.0.0"),
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());

    const qsizetype contextsBefore =
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts();
    const qsizetype observersBefore =
        qbrowser_host_testing::installedPackageWorkerActiveObservers();
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy failed(host.get(), &HostApplication::updateLifecycleFailed);
    std::atomic_bool destroyedFromReady = false;
    std::atomic<quint32> processId = 0;
    connect(host.get(), &HostApplication::packageWorkerReady, this,
            [&](const QString &, const QString &, const QString &,
                const quint64, const quint64, const quint32 pid) {
        processId.store(pid, std::memory_order_release);
        host.reset();
        destroyedFromReady.store(true, std::memory_order_release);
    });
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(
        destroyedFromReady.load(std::memory_order_acquire) || !failed.isEmpty(),
        30'000);
    QVERIFY2(destroyedFromReady.load(std::memory_order_acquire),
             failed.isEmpty()
                 ? "worker did not become ready"
                 : qPrintable(failed.last().at(0).toString()));
    QVERIFY(processId.load(std::memory_order_acquire) != 0);
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(waitForProcessExit(processId.load(std::memory_order_acquire), 10'000));
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerActiveObservers(),
        observersBefore, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qbrowser_host_testing::installedPackageWorkerLiveRetirementContexts(),
        contextsBefore, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void ProductionUpdateRuntimeTest::launchThreadStartFailureRetiresSynchronously()
{
    runLauncherThreadStartFailure(false);
}

void ProductionUpdateRuntimeTest::observerThreadStartFailureRetiresSynchronously()
{
    runLauncherThreadStartFailure(true);
}

void ProductionUpdateRuntimeTest::launchThreadStartFailureHandlerCanDestroyHost()
{
    runThreadStartFailureDestroyingHost(false);
}

void ProductionUpdateRuntimeTest::observerThreadStartFailureHandlerCanDestroyHost()
{
    runThreadStartFailureDestroyingHost(true);
}

void ProductionUpdateRuntimeTest::retirementThreadStartFailureIsFatalAndRetryable()
{
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    std::atomic_int completions = 0;
    std::atomic_bool firstCompletionSucceeded = true;
    std::mutex errorMutex;
    QString firstError;
    qbrowser_host_testing::failNextWorkerRetirementThreadStartForTesting();
    const WorkerRetirementManager::Ticket ticket =
        WorkerRetirementManager::instance().retire(
            [] { return WorkerRetirementAttemptResult{true, {}}; },
            [&](const bool succeeded, const QString &stableError) {
                const int completion = completions.fetch_add(1) + 1;
                if (completion == 1) {
                    firstCompletionSucceeded.store(
                        succeeded, std::memory_order_release);
                    std::lock_guard lock(errorMutex);
                    firstError = stableError;
                }
            });
    QVERIFY(ticket != 0);
    QTRY_COMPARE_WITH_TIMEOUT(completions.load(), 1, 5'000);
    QVERIFY(!firstCompletionSucceeded.load(std::memory_order_acquire));
    const WorkerRetirementStatus failed =
        WorkerRetirementManager::instance().status();
    QCOMPARE(failed.fatal, 1);
    QCOMPARE(failed.activeThreads, 0);
    {
        std::lock_guard lock(errorMutex);
        QCOMPARE(firstError,
                 QStringLiteral("host.launch.retirement_thread_unavailable"));
    }
    QVERIFY(WorkerRetirementManager::instance().retryFatal());
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QCOMPARE(completions.load(), 2);
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
}

void ProductionUpdateRuntimeTest::checkedShutdownWaitsForInflightLaunchRegistration()
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(writeNewFile(publicKey, keys.value().publicKeyPem));
    QVERIFY(protectTrustKey(publicKey));
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("checked-shutdown"), QStringLiteral("1.0.0"),
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY(parsed.value.has_value());

    QSemaphore releaseValidation;
    std::atomic_bool validationBlocked = false;
    qbrowser_host_testing::InstalledPackageWorkerLauncherTestHooks hooks;
    hooks.beforeBindingValidation = [&](const UpdateLaunchRequest &) {
        validationBlocked.store(true, std::memory_order_release);
        releaseValidation.acquire();
    };
    qbrowser_host_testing::setInstalledPackageWorkerLauncherTestHooks(
        std::move(hooks));
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(
        validationBlocked.load(std::memory_order_acquire), 30'000);
    host.reset();

    auto shutdown = std::async(std::launch::async, [] {
        return WorkerRetirementManager::instance().shutdownChecked(10'000);
    });
    const bool returnedBeforeLaunchFinished =
        shutdown.wait_for(std::chrono::milliseconds(250))
        == std::future_status::ready;
    qbrowser_host_testing::resetInstalledPackageWorkerLauncherTestHooks();
    releaseValidation.release();
    const bool shutdownSucceeded = shutdown.get();

    QVERIFY(!returnedBeforeLaunchFinished);
    QVERIFY(shutdownSucceeded);
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
}

void ProductionUpdateRuntimeTest::signedInstalledPackagesDriveAutomaticRealWorkerRollback()
{
    constexpr int launchObservationTimeoutMs = 120'000;
    constexpr int externalHostObservationTimeoutMs = 30'000;
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("qbrowser-production-update-stage.txt")));
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("production-install-host.stdout")));
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("production-install-host.stderr")));
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("production-offline-host.stdout")));
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("production-offline-host.stderr")));
    recordProductionPhase("signed-runtime-start");
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
    const QString storage = telemetryRoot.filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkpath(telemetry));
    QVERIFY(QDir().mkpath(storage));
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
    HostRuntimeConfigResult parsed = parseHostArguments(arguments, storage);
    QVERIFY2(parsed.value.has_value(), qPrintable(parsed.stableError));
    const qsizetype authoritiesBefore =
        RuntimePackageAuthority::liveCountForTesting();
    auto host = std::make_unique<HostApplication>(std::move(*parsed.value));
    QSignalSpy ready(host.get(), &HostApplication::packageWorkerReady);
    QSignalSpy exited(host.get(), &HostApplication::packageWorkerExited);
    QSignalSpy capability(host.get(), &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy failure(host.get(), &HostApplication::updateLifecycleFailed);
    QVERIFY(host->start());
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || !failure.isEmpty(),
                             launchObservationTimeoutMs);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty()
                 ? ""
                 : qPrintable(QStringLiteral("initial 1.0.0 launch: ")
                              + failure.first().at(0).toString()));
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
        telemetryEventCount(telemetry, QByteArrayLiteral("healthy")) >= 1,
        10'000);
    QCOMPARE(observedStore.activationState(environment.appId()).state.lastKnownGood,
             QFileInfo(readyPath(0)).fileName());
    recordProductionPhase("initial-package-ready");

    QVERIFY(host->requestPackageInstall(two));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 2 || !failure.isEmpty(),
                             launchObservationTimeoutMs);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty()
                 ? ""
                 : qPrintable(QStringLiteral("1.1.0 update launch: ")
                              + failure.last().at(0).toString()));
    QCOMPARE(ready.count(), 2);
    QCOMPARE(readyVersion(1), QStringLiteral("1.1.0"));
    QCOMPARE(QFileInfo(readyPath(1)).canonicalFilePath(),
             QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
                 .canonicalFilePath());
    QVERIFY(QFileInfo::exists(readyPath(1) + QStringLiteral("/qml/Start.qml")));
    QVERIFY(!QFileInfo::exists(readyPath(1) + QStringLiteral("/qml/Main.qml")));
    QTRY_VERIFY_WITH_TIMEOUT(
        telemetryEventCount(telemetry, QByteArrayLiteral("healthy")) >= 2,
        10'000);
    QCOMPARE(observedStore.activationState(environment.appId()).state.lastKnownGood,
             QFileInfo(readyPath(1)).fileName());
    recordProductionPhase("healthy-update-ready");

    QVERIFY(host->requestPackageInstall(crashing));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 3 || !failure.isEmpty(),
                             launchObservationTimeoutMs);
    QVERIFY2(failure.isEmpty(),
             failure.isEmpty()
                 ? ""
                 : qPrintable(QStringLiteral("1.2.0 crash candidate launch: ")
                              + failure.last().at(0).toString()));
    QCOMPARE(ready.count(), 3);
    QCOMPARE(readyVersion(2), QStringLiteral("1.2.0"));
    recordProductionPhase("crash-candidate-ready");
    QVERIFY(terminateProcessId(readyPid(2)));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 4, launchObservationTimeoutMs);
    QCOMPARE(readyVersion(3), QStringLiteral("1.2.0"));
    recordProductionPhase("crash-candidate-restarted");
    QVERIFY(terminateProcessId(readyPid(3)));
    QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 2, 10'000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 5, launchObservationTimeoutMs);
    QCOMPARE(readyVersion(4), QStringLiteral("1.1.0"));
    recordProductionPhase("rollback-lkg-ready");
    QCOMPARE(QFileInfo(readyPath(4)).canonicalFilePath(),
             QFileInfo(observedStore.resolveCurrent(environment.appId()).path)
                 .canonicalFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(capability.count() >= 2, 10'000);
    QCOMPARE(capability.last().at(0).toString(), QStringLiteral("storage"));
    QCOMPARE(capability.last().at(1).toString(), QStringLiteral("get"));
    QCOMPARE(capability.last().at(2).toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("lkgRecovered"));
    recordProductionPhase("rollback-capability-observed");
    QCOMPARE(failure.count(), 0);
    QVERIFY(QCoreApplication::instance() != nullptr);
    const int readyBeforeShutdownRace = ready.count();
    const quint32 liveWorker = readyPid(4);
    QElapsedTimer destruction;
    destruction.start();
    recordProductionPhase("in-process-host-retire-start");
    host.reset();
    QVERIFY2(destruction.elapsed() < 100,
             "Host destruction blocked on the lifecycle thread");
    QVERIFY(waitForProcessExit(liveWorker, 10'000));
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(environment.sandboxTempRoot()).filePath(QStringLiteral("workers")))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        10'000);
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    QTRY_COMPARE_WITH_TIMEOUT(RuntimePackageAuthority::liveCountForTesting(),
                              authoritiesBefore, 10'000);
    recordProductionPhase("in-process-host-retire-complete");
    QTest::qWait(500);
    QCOMPARE(ready.count(), readyBeforeShutdownRace);

    const QString cliTelemetry = telemetryRoot.filePath(
        QStringLiteral("cli-telemetry"));
    QVERIFY(QDir().mkpath(cliTelemetry));
    QStringList installArguments = arguments;
    installArguments.push_back(QStringLiteral("--storage-directory=") + storage);
    for (QString &argument : installArguments) {
        if (argument.startsWith(QStringLiteral("--install-package="))) {
            argument = QStringLiteral("--install-package=") + cli;
        } else if (argument.startsWith(
                       QStringLiteral("--telemetry-directory="))) {
            argument = QStringLiteral("--telemetry-directory=") + cliTelemetry;
        }
    }
    QProcess productionInstallHost;
    QProcessEnvironment diagnosticEnvironment =
        QProcessEnvironment::systemEnvironment();
    diagnosticEnvironment.insert(
        QStringLiteral("Q_BROWSER_HOST_DIAGNOSTIC_PHASES"),
        QStringLiteral("1"));
    productionInstallHost.setProcessEnvironment(diagnosticEnvironment);
    const QString installStdout = QDir::temp().filePath(
        QStringLiteral("production-install-host.stdout"));
    const QString installStderr = QDir::temp().filePath(
        QStringLiteral("production-install-host.stderr"));
    productionInstallHost.setStandardOutputFile(installStdout);
    productionInstallHost.setStandardErrorFile(installStderr);
    [[maybe_unused]] const auto cleanupInstallHost = qScopeGuard([&] {
        (void)stopOwnedProcess(productionInstallHost);
    });
    productionInstallHost.setProgram(QString::fromUtf8(Q_BROWSER_HOST_PATH));
    productionInstallHost.setArguments(installArguments);
    recordProductionPhase("cli-install-start");
    productionInstallHost.start();
    QVERIFY2(productionInstallHost.waitForStarted(10'000),
             qPrintable(productionInstallHost.errorString()));
    const auto cliHealthy = [&] {
        return telemetryEventCount(cliTelemetry, QByteArrayLiteral("healthy")) > 0;
    };
    const auto cliTerminal = [&] {
        return readDiagnosticFile(QDir(cliTelemetry).filePath(
                   QStringLiteral("events.jsonl")))
                   .contains(QByteArrayLiteral("\"code\":\"recovered\""))
            || readDiagnosticFile(installStderr).contains(
                QByteArrayLiteral("qbrowser-host lifecycle failure:"));
    };
    QVERIFY2(waitForExternalHost(productionInstallHost, cliHealthy, cliTerminal,
                                 externalHostObservationTimeoutMs),
             qPrintable(externalHostDiagnostic(
                 productionInstallHost, observedStore, environment.appId(),
                 cliTelemetry, installStdout, installStderr)));
    const ActivationStateResult cliHealthyState = observedStore.activationState(
        environment.appId());
    QVERIFY(cliHealthyState.hasValue());
    QVERIFY(cliHealthyState.state.current.startsWith(QStringLiteral("1.3.0-")));
    QVERIFY(cliHealthyState.state.lastKnownGood.startsWith(
        QStringLiteral("1.3.0-")));
    recordProductionPhase("cli-install-healthy");
    QVERIFY(stopOwnedProcess(productionInstallHost));
    QCOMPARE(productionInstallHost.state(), QProcess::NotRunning);
    recordProductionPhase("cli-install-stopped");

    const qsizetype healthyBeforeOfflineCli = telemetryEventCount(
        cliTelemetry, QByteArrayLiteral("healthy"));
    const ActivationStateResult beforeOffline = observedStore.activationState(
        environment.appId());
    QVERIFY(beforeOffline.hasValue());
    const qint64 generationBeforeOffline = beforeOffline.state.generation;
    QStringList offlineArguments = installArguments;
    offlineArguments.removeIf([](const QString &argument) {
        return argument.startsWith(QStringLiteral("--install-package="));
    });
    QProcess productionHost;
    productionHost.setProcessEnvironment(diagnosticEnvironment);
    const QString offlineStdout = QDir::temp().filePath(
        QStringLiteral("production-offline-host.stdout"));
    const QString offlineStderr = QDir::temp().filePath(
        QStringLiteral("production-offline-host.stderr"));
    productionHost.setStandardOutputFile(offlineStdout);
    productionHost.setStandardErrorFile(offlineStderr);
    [[maybe_unused]] const auto cleanupOfflineHost = qScopeGuard([&] {
        (void)stopOwnedProcess(productionHost);
    });
    productionHost.setProgram(QString::fromUtf8(Q_BROWSER_HOST_PATH));
    productionHost.setArguments(offlineArguments);
    recordProductionPhase("cli-offline-start");
    productionHost.start();
    QVERIFY2(productionHost.waitForStarted(10'000),
             qPrintable(productionHost.errorString()));
    const auto offlineReady = [&] {
        return telemetryEventCount(cliTelemetry, QByteArrayLiteral("healthy"))
            > healthyBeforeOfflineCli;
    };
    const auto offlineTerminal = [&] {
        return readDiagnosticFile(offlineStderr).contains(
            QByteArrayLiteral("qbrowser-host lifecycle failure:"));
    };
    QVERIFY2(waitForExternalHost(productionHost, offlineReady, offlineTerminal,
                                 externalHostObservationTimeoutMs),
             qPrintable(externalHostDiagnostic(
                 productionHost, observedStore, environment.appId(), cliTelemetry,
                 offlineStdout, offlineStderr)));
    const ActivationStateResult offlineState = observedStore.activationState(
        environment.appId());
    QVERIFY(offlineState.hasValue());
    QVERIFY(offlineState.state.generation > generationBeforeOffline);
    QVERIFY(offlineState.state.current.startsWith(QStringLiteral("1.3.0-")));
    QVERIFY(offlineState.state.lastKnownGood.startsWith(
        QStringLiteral("1.3.0-")));
    recordProductionPhase("cli-offline-ready");
    QVERIFY(stopOwnedProcess(productionHost));
    QCOMPARE(productionHost.state(), QProcess::NotRunning);
    recordProductionPhase("cli-offline-stopped");
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    ProductionUpdateRuntimeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_production_update_runtime.moc"
