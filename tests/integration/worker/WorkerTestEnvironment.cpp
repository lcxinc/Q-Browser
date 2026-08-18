#include "WorkerTestEnvironment.h"

#include "AppContainerProfile.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

namespace {

class UniqueHandle final
{
public:
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { if (value_ != nullptr) CloseHandle(value_); }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

QString currentUserSidString()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return {};
    }
    UniqueHandle token(rawToken);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return {};
    }
    QByteArray storage(static_cast<qsizetype>(bytes), Qt::Uninitialized);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes)) {
        return {};
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.constData());
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &text)) {
        return {};
    }
    const QString result = QString::fromWCharArray(text);
    LocalFree(text);
    return result;
}

bool protectRoot(const QString &path, const QString &userSid)
{
    const QString sddl = QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(userSid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1,
            &descriptor, nullptr)) {
        return false;
    }
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    const bool extracted = GetSecurityDescriptorDacl(
        descriptor, &present, &dacl, &defaulted) != FALSE && present != FALSE;
    const DWORD result = extracted
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

bool writeNewFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(contents) == contents.size();
}

} // namespace

SessionReceiveResult receiveUntil(IpcSession &session,
                                  const ProtocolType expectedType,
                                  const int timeoutMs)
{
    constexpr qsizetype heartbeatBurstAllowance = 32;
    constexpr int minimumHeartbeatIntervalMs = 20;
    const auto timeout = [](const QString &errorCode) {
        return SessionReceiveResult{SessionStatus::TimedOut, std::nullopt, errorCode};
    };
    if (timeoutMs <= 0) {
        return timeout(QStringLiteral("worker.test.receive_timeout"));
    }
    const qsizetype maximumSkippedHeartbeats = heartbeatBurstAllowance
        + static_cast<qsizetype>(timeoutMs / minimumHeartbeatIntervalMs);
    qsizetype skippedHeartbeats = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        const qint64 elapsedMs = elapsed.elapsed();
        if (elapsedMs >= timeoutMs) {
            return timeout(QStringLiteral("worker.test.receive_timeout"));
        }
        const int remaining = timeoutMs - static_cast<int>(elapsedMs);
        SessionReceiveResult result = session.receive(remaining);
        if (result.status != SessionStatus::MessageReady || !result.message.has_value()
            || result.message->type() == expectedType
            || result.message->type() != ProtocolType::Heartbeat) {
            return result;
        }
        ++skippedHeartbeats;
        if (skippedHeartbeats >= maximumSkippedHeartbeats) {
            return timeout(QStringLiteral("worker.test.heartbeat_limit"));
        }
    }
}

WorkerTestEnvironment::Launch::Launch(IpcSession host, SandboxProcess child)
    : hostSession(std::move(host)), process(std::move(child))
{
}

WorkerTestEnvironment::WorkerTestEnvironment(QByteArray mainQml)
    : appId_(QStringLiteral("com.qbrowser.workertest.%1")
                 .arg(QUuid::createUuid().toString(QUuid::Id128).toLower())),
      mainQml_(std::move(mainQml))
{
    if (!prepare() && error_.isEmpty()) {
        error_ = QStringLiteral("worker test environment preparation failed");
    }
}

WorkerTestEnvironment::~WorkerTestEnvironment()
{
    const auto profileName = AppContainerProfile::deterministicName(appId_);
    if (profileName.has_value()) {
        (void)DeleteAppContainerProfile(
            reinterpret_cast<PCWSTR>(profileName->utf16()));
    }
}

bool WorkerTestEnvironment::isValid() const noexcept
{
    return boundary_.has_value();
}

QString WorkerTestEnvironment::error() const
{
    return error_;
}

QString WorkerTestEnvironment::appId() const
{
    return appId_;
}

bool WorkerTestEnvironment::prepare()
{
    if (!root_.isValid()) {
        error_ = QStringLiteral("temporary root is invalid");
        return false;
    }
    packageRoot_ = QDir(root_.path()).filePath(QStringLiteral("packages"));
    tempRoot_ = QDir(root_.path()).filePath(QStringLiteral("temp"));
    runtimeRoot_ = QDir(root_.path()).filePath(QStringLiteral("runtime"));
    if (!QDir().mkpath(packageRoot_) || !QDir().mkpath(tempRoot_)
        || !QDir().mkpath(runtimeRoot_)) {
        error_ = QStringLiteral("approved roots could not be created");
        return false;
    }
    const QString userSid = currentUserSidString();
    if (userSid.isEmpty() || !protectRoot(packageRoot_, userSid)
        || !protectRoot(tempRoot_, userSid) || !protectRoot(runtimeRoot_, userSid)) {
        error_ = QStringLiteral("approved roots could not be protected");
        return false;
    }

    packageDirectory_ = QDir(packageRoot_).filePath(QStringLiteral("apps/test/1.0.0"));
    workerTemp_ = QDir(tempRoot_).filePath(QStringLiteral("sessions/worker"));
    const QString qmlDirectory = QDir(packageDirectory_).filePath(QStringLiteral("qml"));
    if (!QDir().mkpath(qmlDirectory) || !QDir().mkpath(workerTemp_)) {
        error_ = QStringLiteral("worker descendants could not be created");
        return false;
    }
    if (mainQml_.isEmpty()) {
        mainQml_ = QByteArrayLiteral(
            "import QtQuick\nRectangle { width: 320; height: 200; color: \"#123456\" }\n");
    }
    if (!writeNewFile(QDir(qmlDirectory).filePath(QStringLiteral("Main.qml")), mainQml_)) {
        error_ = QStringLiteral("test QML could not be written");
        return false;
    }

    const QString sourceWorker = QString::fromUtf8(Q_BROWSER_WORKER_PATH);
    workerExecutable_ = QDir(runtimeRoot_).filePath(QFileInfo(sourceWorker).fileName());
    if (!QFile::copy(sourceWorker, workerExecutable_)) {
        error_ = QStringLiteral("worker executable could not be staged");
        return false;
    }
    QProcess deploy;
    deploy.start(QString::fromUtf8(Q_BROWSER_WINDEPLOYQT_PATH),
                 {QStringLiteral("--no-translations"),
                  QStringLiteral("--qmldir"), qmlDirectory,
                  QStringLiteral("--dir"), runtimeRoot_,
                  workerExecutable_});
    if (!deploy.waitForStarted(5000) || !deploy.waitForFinished(120000)
        || deploy.exitStatus() != QProcess::NormalExit || deploy.exitCode() != 0) {
        error_ = QStringLiteral("windeployqt failed: %1")
                     .arg(QString::fromLocal8Bit(deploy.readAllStandardError()));
        return false;
    }

    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot_, tempRoot_, {runtimeRoot_}});
    if (!boundary.value.has_value()) {
        error_ = QStringLiteral("trust boundary failed: %1 native=%2")
                     .arg(boundary.errorCode)
                     .arg(boundary.nativeError.value);
        return false;
    }
    boundary_.emplace(std::move(*boundary.value));
    return true;
}

std::optional<WorkerTestEnvironment::Launch> WorkerTestEnvironment::launch(
    const QString &workerNonce,
    const QString &hostNonce,
    const int heartbeatMs)
{
    if (!boundary_.has_value()) {
        return std::nullopt;
    }
    SandboxLaunchRequest request;
    request.appId = appId_;
    request.executablePath = workerExecutable_;
    request.packageDirectory = packageDirectory_;
    request.tempDirectory = workerTemp_;
    request.arguments = {QStringLiteral("--qbrowser-package"), packageDirectory_,
                         QStringLiteral("--qbrowser-entry"), QStringLiteral("qml/Main.qml"),
                         QStringLiteral("--qbrowser-nonce"), workerNonce,
                         QStringLiteral("--qbrowser-heartbeat-ms"), QString::number(heartbeatMs)};
    request.resourceLimits = {1, 512ULL * 1024ULL * 1024ULL};
    auto config = boundary_->makeLaunchConfig(request);
    if (!config.value.has_value()) {
        error_ = config.errorCode;
        return std::nullopt;
    }
    WinPipePair pair = WinPipeTransport::createHostPair();
    if (!pair.isValid()) {
        error_ = QStringLiteral("pipe creation failed");
        return std::nullopt;
    }
    WinPipeTransport host = pair.takeHost();
    auto launched = SandboxLauncher::launch(*config.value, pair.takeWorkerEnds());
    if (!launched.process.has_value()) {
        error_ = launched.errorCode;
        return std::nullopt;
    }
    return Launch(IpcSession(std::move(host), IpcRole::Host,
                             HostLaunchContext{hostNonce, appId_}),
                  std::move(*launched.process));
}
