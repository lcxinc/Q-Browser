#include "TestEnvironment.h"

#include "Archive.h"
#include "AppContainerProfile.h"
#include "ContentDigest.h"
#include "HostRuntimeConfig.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"
#include "WorkerRetirementManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

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
}

TestEnvironment::TestEnvironment()
{
    if (!workerEnvironment_.isValid()) {
        error_ = workerEnvironment_.error();
        return;
    }
    if (!packages_.isValid() || !trust_.isValid() || !telemetry_.isValid()) {
        error_ = QStringLiteral("e2e temporary roots are unavailable");
        return;
    }
    if (!prepareTrustKey() || !startMockApi()) return;
}

TestEnvironment::~TestEnvironment()
{
    host_.reset();
    (void)WorkerRetirementManager::instance().shutdownChecked(10'000);
    if (mockApi_.state() != QProcess::NotRunning) {
        mockApi_.terminate();
        if (!mockApi_.waitForFinished(5'000)) {
            mockApi_.kill();
            (void)mockApi_.waitForFinished(5'000);
        }
    }
    const auto profile = AppContainerProfile::deterministicName(
        QStringLiteral("com.qbrowser.pilot"));
    if (profile.has_value()) {
        (void)DeleteAppContainerProfile(
            reinterpret_cast<PCWSTR>(profile->utf16()));
    }
}

bool TestEnvironment::isValid() const noexcept { return error_.isEmpty(); }
QString TestEnvironment::error() const { return error_; }
HostApplication *TestEnvironment::host() const noexcept { return host_.get(); }
QString TestEnvironment::mockOrigin() const { return mockOrigin_; }
int TestEnvironment::failureCount() const noexcept { return failureCount_; }
quint32 TestEnvironment::currentWorkerProcessId() const noexcept
{
    return currentWorkerProcessId_;
}
QStringList TestEnvironment::readyVersions() const { return readyVersions_; }

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
            object.insert(QStringLiteral("appId"), QStringLiteral("com.qbrowser.pilot"));
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
            file.contents.append("\n// authenticated payload changed");
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
        QStringLiteral("--app-id=com.qbrowser.pilot"),
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
        error_ = stableError;
    });
    if (!host_->start()) {
        error_ = QStringLiteral("production Host did not start");
        return false;
    }
    if (!waitForReady(version)) {
        if (error_.isEmpty()) error_ = QStringLiteral("production Worker did not become ready");
        return false;
    }
    PackageStore store(workerEnvironment_.packageRoot());
    if (!waitUntil([&] {
            return !store.activationState(QStringLiteral("com.qbrowser.pilot"))
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
