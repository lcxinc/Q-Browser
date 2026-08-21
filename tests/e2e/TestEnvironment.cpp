#include "TestEnvironment.h"

#include "Archive.h"
#include "AppContainerProfile.h"
#include "ContentDigest.h"
#include "HostRuntimeConfig.h"
#include "HostWorkerSessionController.h"
#include "MainWindow.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"
#include "WorkerRetirementManager.h"
#include "WebSurface.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QWebEnginePage>

#include <Aclapi.h>
#include <qt_windows.h>
#include <userenv.h>

namespace {
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
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemBytes)) {
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

bool waitUntil(const std::function<bool()> &predicate, const int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return predicate();
}

bool processHasExited(const quint32 processId)
{
    if (processId == 0) return true;
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, processId);
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}
}

TestEnvironment::TestEnvironment()
{
    if (!workerEnvironment_.isValid()) {
        error_ = workerEnvironment_.error();
        return;
    }
    appId_ = workerEnvironment_.appId();
    if (!packages_.isValid() || !trust_.isValid() || !telemetry_.isValid()) {
        error_ = QStringLiteral("e2e temporary roots are unavailable");
        return;
    }
    if (!prepareTrustKey() || !startMockApi()) return;
}

TestEnvironment::~TestEnvironment()
{
    (void)shutdown();
}

bool TestEnvironment::shutdown()
{
    if (shutdown_) return cleanupError_.isEmpty();
    shutdown_ = true;
    const quint32 workerProcessId = currentWorkerProcessId_;
    if (host_ != nullptr && host_->mainWindow() != nullptr) {
        WebSurface *webSurface = host_->mainWindow()->webSurface();
        QWebEnginePage *page = webSurface != nullptr ? webSurface->page() : nullptr;
        if (page != nullptr) {
            page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
            page->triggerAction(QWebEnginePage::Stop);
            page->setUrl(WebSurface::trustedErrorUrl());
            (void)waitUntil([&] { return !page->isLoading(); }, 5'000);
        }
    }
    host_.reset();
    QElapsedTimer webEngineDrain;
    webEngineDrain.start();
    while (webEngineDrain.elapsed() < 750) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    if (!WorkerRetirementManager::instance().shutdownChecked(10'000)) {
        cleanupError_ = QStringLiteral("worker retirement cleanup failed");
    }
    if (!waitUntil([&] { return processHasExited(workerProcessId); }, 5'000)) {
        cleanupError_ = QStringLiteral("worker process remained alive after retirement");
    }
    if (mockApi_.state() != QProcess::NotRunning) {
        mockApi_.terminate();
        if (!mockApi_.waitForFinished(5'000)) {
            mockApi_.kill();
            if (!mockApi_.waitForFinished(5'000)) {
                cleanupError_ = QStringLiteral("mock-api process cleanup failed");
            }
        }
    }
    mockApi_.close();
    if (!packages_.remove()) {
        cleanupError_ = QStringLiteral("package fixture cleanup failed");
    } else {
        packages_.setAutoRemove(false);
    }
    if (!trust_.remove()) {
        cleanupError_ = QStringLiteral("trust fixture cleanup failed");
    } else {
        trust_.setAutoRemove(false);
    }
    if (!telemetry_.remove()) {
        cleanupError_ = QStringLiteral("telemetry fixture cleanup failed");
    } else {
        telemetry_.setAutoRemove(false);
    }
    if (!workerEnvironment_.cleanup()) cleanupError_ = workerEnvironment_.cleanupError();
    if (!cleanupError_.isEmpty()) error_ = cleanupError_;
    return cleanupError_.isEmpty();
}

bool TestEnvironment::isValid() const noexcept { return error_.isEmpty(); }
QString TestEnvironment::error() const { return error_; }
HostApplication *TestEnvironment::host() const noexcept { return host_.get(); }
QString TestEnvironment::mockOrigin() const { return mockOrigin_; }
int TestEnvironment::failureCount() const noexcept { return failureCount_; }
QString TestEnvironment::lastFailure() const { return lastFailure_; }
int TestEnvironment::tamperCanaryCount() const noexcept { return tamperCanaryCount_; }
quint32 TestEnvironment::currentWorkerProcessId() const noexcept
{
    return currentWorkerProcessId_;
}
QStringList TestEnvironment::readyVersions() const { return readyVersions_; }

QString TestEnvironment::currentVersionDirectory() const
{
    const ActivationStateResult state =
        PackageStore(workerEnvironment_.packageRoot()).activationState(appId_);
    return state.hasValue() ? state.state.current : QString{};
}

QString TestEnvironment::lastKnownGoodVersionDirectory() const
{
    const ActivationStateResult state =
        PackageStore(workerEnvironment_.packageRoot()).activationState(appId_);
    return state.hasValue() ? state.state.lastKnownGood : QString{};
}

bool TestEnvironment::prepareTrustKey()
{
    const auto keys = SignatureVerifier::generateKeyPair();
    if (!keys.hasValue()) {
        error_ = QStringLiteral("development keys could not be generated");
        return false;
    }
    privateKey_ = keys.value().privateKeyPem;
    publicKey_ = keys.value().publicKeyPem;
    publicKeyPath_ = trust_.filePath(QStringLiteral("trusted.pem"));
    if (!writeNewFile(publicKeyPath_, publicKey_) || !protectTrustKey(publicKeyPath_)) {
        error_ = QStringLiteral("development trust key could not be protected");
        return false;
    }
    return true;
}

bool TestEnvironment::startMockApi()
{
    mockApi_.setProgram(QString::fromUtf8(Q_BROWSER_NODE_PATH));
    mockApi_.setArguments({QStringLiteral("src/server.ts")});
    mockApi_.setWorkingDirectory(
        QDir(QString::fromUtf8(Q_BROWSER_SOURCE_DIR)).filePath(QStringLiteral("tools/mock-api")));
    mockApi_.setProcessChannelMode(QProcess::SeparateChannels);
    mockApi_.start();
    if (!mockApi_.waitForStarted(5'000)
        || !mockApi_.waitForReadyRead(10'000)) {
        error_ = QStringLiteral("mock-api did not start: ")
            + QString::fromUtf8(mockApi_.readAllStandardError());
        return false;
    }
    const QJsonDocument line = QJsonDocument::fromJson(mockApi_.readLine());
    mockOrigin_ = line.object().value(QStringLiteral("origin")).toString();
    if (!mockOrigin_.startsWith(QStringLiteral("http://127.0.0.1:"))) {
        error_ = QStringLiteral("mock-api returned an invalid origin");
        return false;
    }
    return true;
}

QString TestEnvironment::createPackage(const QString &version,
                                       const QByteArray &mainQml)
{
    const QString source = QDir(QString::fromUtf8(Q_BROWSER_SOURCE_DIR))
                               .filePath(QStringLiteral("packages/pilot"));
    QVector<ArchiveFile> files;
    QDirIterator iterator(source, QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QByteArray contents = file.readAll();
        const QByteArray relative = QDir(source).relativeFilePath(path).toUtf8();
        if (relative == QByteArrayLiteral("manifest.json")) {
            QJsonDocument document = QJsonDocument::fromJson(contents);
            QJsonObject object = document.object();
            object.insert(QStringLiteral("appId"), appId_);
            object.insert(QStringLiteral("version"), version);
            contents = QJsonDocument(object).toJson(QJsonDocument::Compact);
        } else if (relative == QByteArrayLiteral("qml/Main.qml") && !mainQml.isEmpty()) {
            contents = mainQml;
        }
        files.push_back({relative, contents});
    }
    const auto payload = ContentDigest::payload(files);
    if (!payload.hasValue()) return {};
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), payload.hex()});
    const auto digest = ContentDigest::signedPackage(files);
    if (!digest.hasValue()) return {};
    const auto signature = SignatureVerifier::signPem(digest.bytes(), privateKey_);
    if (!signature.hasValue()) return {};
    files.push_back({QByteArrayLiteral("metadata/signature.ed25519"), signature.value()});
    const QString output = packages_.filePath(version + QStringLiteral(".qapkg"));
    return Archive::createFromFiles(files, output).hasValue() ? output : QString{};
}

QString TestEnvironment::createTamperedPackage(const QString &version)
{
    const QString valid = createPackage(version);
    const auto snapshot = Archive::snapshot(valid);
    if (!snapshot.hasValue()) return {};
    QVector<ArchiveFile> files = snapshot.files();
    for (ArchiveFile &file : files) {
        if (file.path == QByteArrayLiteral("qml/Main.qml")) {
            file.contents = QByteArrayLiteral(
                "import QtQuick\nItem { Component.onCompleted: "
                "Runtime.invoke(\"storage\", \"get\", "
                "{ key: \"tamper-canary\" }) }\n");
        }
    }
    const QString tampered = packages_.filePath(version + QStringLiteral("-tampered.qapkg"));
    return Archive::createFromFiles(files, tampered).hasValue() ? tampered : QString{};
}

bool TestEnvironment::start(const QString &version)
{
    if (!isValid() || host_) return false;
    const QString package = createPackage(version);
    if (package.isEmpty()) return false;
    const QStringList arguments{
        QStringLiteral("--package-mode"),
        QStringLiteral("--mock-origin=") + mockOrigin_,
        QStringLiteral("--app-id=") + appId_,
        QStringLiteral("--trusted-public-key=") + publicKeyPath_,
        QStringLiteral("--package-store=") + workerEnvironment_.packageRoot(),
        QStringLiteral("--sandbox-temp=") + workerEnvironment_.sandboxTempRoot(),
        QStringLiteral("--runtime-root=") + workerEnvironment_.runtimeRoot(),
        QStringLiteral("--worker-executable=") + workerEnvironment_.workerExecutable(),
        QStringLiteral("--telemetry-directory=") + telemetry_.path(),
        QStringLiteral("--install-package=") + package,
        QStringLiteral("--health-window-ms=2000"),
        QStringLiteral("--heartbeat-timeout-ms=10000")};
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    if (!parsed.value.has_value()) {
        error_ = parsed.stableError;
        return false;
    }
    host_ = std::make_unique<HostApplication>(std::move(*parsed.value));
    QObject::connect(host_.get(), &HostApplication::packageWorkerReady,
                     host_.get(), [this](const QString &, const QString &readyVersion,
                                         const QString &, quint64, quint64, quint32 pid) {
        readyVersions_.push_back(readyVersion);
        currentWorkerProcessId_ = pid;
    });
    QObject::connect(host_.get(), &HostApplication::updateLifecycleFailed,
                     host_.get(), [this](const QString &stableError) {
        ++failureCount_;
        lastFailure_ = stableError;
        error_ = stableError;
    });
    QObject::connect(host_.get(), &HostApplication::workerCapabilityRequestObserved,
                     host_.get(), [this](const QString &, const QString &,
                                         const QVariantMap &payload) {
        if (payload.value(QStringLiteral("key")).toString()
            == QStringLiteral("tamper-canary")) {
            ++tamperCanaryCount_;
        }
    });
    if (!host_->start()) {
        error_ = QStringLiteral("production Host did not start");
        return false;
    }
    HostWorkerSessionController *controller = host_->workerSessionController();
    if (controller == nullptr) {
        error_ = QStringLiteral("production Host session controller is unavailable");
        return false;
    }
    QObject::connect(controller, &HostWorkerSessionController::heartbeatObserved,
                     host_.get(), [this] { ++heartbeatCount_; });
    if (!waitForReady(version)) {
        if (error_.isEmpty()) error_ = QStringLiteral("production Worker did not become ready");
        return false;
    }
    if (!waitUntil([&] { return heartbeatCount_ > 0; }, 10'000)) {
        error_ = QStringLiteral("production Worker did not become responsive");
        return false;
    }
    PackageStore store(workerEnvironment_.packageRoot());
    if (!waitUntil([&] {
            return !store.activationState(appId_)
                        .state.lastKnownGood.isEmpty();
        }, 10'000)) {
        error_ = QStringLiteral("initial package was not marked last-known-good");
        return false;
    }
    error_.clear();
    return true;
}

bool TestEnvironment::install(const QString &packagePath)
{
    return host_ && host_->requestPackageInstall(packagePath);
}

bool TestEnvironment::waitForReady(const QString &version, const int timeoutMs)
{
    return waitUntil([&] { return readyVersions_.contains(version); }, timeoutMs);
}

bool TestEnvironment::waitForFailure(const int previousCount, const int timeoutMs)
{
    return waitUntil([&] { return failureCount_ > previousCount; }, timeoutMs);
}
