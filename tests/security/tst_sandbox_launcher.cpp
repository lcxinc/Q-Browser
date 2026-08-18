#include "AclGrant.h"
#include "AppContainerProfile.h"
#include "FrameCodec.h"
#include "JobLimits.h"
#include "SandboxLauncher.h"
#include "WinPipeTransport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class UniqueHandle final
{
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
};

class EnvironmentVariableGuard final
{
public:
    EnvironmentVariableGuard(const QByteArray &name, const QByteArray &value)
        : name_(name), previous_(qgetenv(name.constData())),
          wasSet_(qEnvironmentVariableIsSet(name.constData()))
    {
        qputenv(name_.constData(), value);
    }
    ~EnvironmentVariableGuard()
    {
        if (wasSet_) {
            qputenv(name_.constData(), previous_);
        } else {
            qunsetenv(name_.constData());
        }
    }
    EnvironmentVariableGuard(const EnvironmentVariableGuard &) = delete;
    EnvironmentVariableGuard &operator=(const EnvironmentVariableGuard &) = delete;

private:
    QByteArray name_;
    QByteArray previous_;
    bool wasSet_ = false;
};

QString uniqueAppId(const QString &suffix)
{
    return QStringLiteral("com.qbrowser.sandboxtest.%1.%2")
        .arg(suffix,
             QUuid::createUuid().toString(QUuid::Id128).toLower());
}

void deleteProfileIfPresent(const QString &profileName)
{
    const HRESULT result = DeleteAppContainerProfile(
        reinterpret_cast<PCWSTR>(profileName.utf16()));
    QVERIFY2(SUCCEEDED(result)
                 || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
             "test profile cleanup failed");
}

quint32 explicitAllowMask(const QString &path, PSID sid)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return 0;
    }
    quint32 mask = 0;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void *rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            continue;
        }
        const auto *header = static_cast<ACE_HEADER *>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
        PSID aceSid = const_cast<DWORD *>(&ace->SidStart);
        if (EqualSid(aceSid, sid)) {
            mask |= ace->Mask;
        }
    }
    LocalFree(descriptor);
    return mask;
}

std::optional<QJsonObject> receiveProbeFrame(WinPipeTransport &transport,
                                             const int timeoutMs,
                                             FrameCodec &codec,
                                             QList<QJsonObject> &buffered)
{
    if (!buffered.isEmpty()) {
        return buffered.takeFirst();
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        const PipeReadResult read = transport.readSome(64U * 1024U, 100);
        if (read.status == PipeIoStatus::TimedOut) {
            continue;
        }
        if (read.status != PipeIoStatus::Ok) {
            return std::nullopt;
        }
        const FrameFeedResult fed = codec.feed(read.bytes);
        if (fed.status == FrameStatus::Failed) {
            return std::nullopt;
        }
        if (!fed.frames.isEmpty()) {
            buffered.append(fed.frames);
            return buffered.takeFirst();
        }
    }
    return std::nullopt;
}

QString quoted(const QString &path)
{
    return QStringLiteral("\"") + path + QStringLiteral("\"");
}

QString currentUserSidString()
{
    HANDLE tokenRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw)) {
        return {};
    }
    UniqueHandle token(tokenRaw);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return {};
    }
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token.get(),
                             TokenUser,
                             buffer.data(),
                             bytes,
                             &bytes)) {
        return {};
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
    LPWSTR converted = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &converted)) {
        return {};
    }
    const QString result = QString::fromWCharArray(converted);
    LocalFree(converted);
    return result;
}

bool createProtectedPrivateFile(const QString &path,
                                const QString &userSid)
{
    const QString sddl = QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;%1)")
                             .arg(userSid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE fileRaw = CreateFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    LocalFree(descriptor);
    if (fileRaw == INVALID_HANDLE_VALUE) {
        return false;
    }
    UniqueHandle file(fileRaw);
    constexpr char contents[] = "host-private";
    DWORD written = 0;
    return WriteFile(file.get(),
                     contents,
                     static_cast<DWORD>(sizeof(contents) - 1U),
                     &written,
                     nullptr) != FALSE
        && written == sizeof(contents) - 1U;
}

bool hasProtectedHostSystemOnlyDacl(const QString &path,
                                   const QString &userSid)
{
    PSID user = nullptr;
    PSID system = nullptr;
    if (!ConvertStringSidToSidW(
            reinterpret_cast<LPCWSTR>(userSid.utf16()), &user)
        || !ConvertStringSidToSidW(L"S-1-5-18", &system)) {
        if (user != nullptr) {
            LocalFree(user);
        }
        if (system != nullptr) {
            LocalFree(system);
        }
        return false;
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    bool exact = result == ERROR_SUCCESS && descriptor != nullptr
        && dacl != nullptr && dacl->AceCount == 2;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    exact = exact
        && GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE
        && (control & SE_DACL_PROTECTED) != 0;
    bool sawUser = false;
    bool sawSystem = false;
    for (DWORD index = 0; exact && index < dacl->AceCount; ++index) {
        void *rawAce = nullptr;
        exact = GetAce(dacl, index, &rawAce) != FALSE;
        if (!exact) {
            break;
        }
        const auto *header = static_cast<ACE_HEADER *>(rawAce);
        exact = header->AceType == ACCESS_ALLOWED_ACE_TYPE;
        if (!exact) {
            break;
        }
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
        PSID aceSid = const_cast<DWORD *>(&ace->SidStart);
        if (EqualSid(aceSid, user)) {
            sawUser = true;
        } else if (EqualSid(aceSid, system)) {
            sawSystem = true;
        } else {
            exact = false;
        }
    }
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    LocalFree(user);
    LocalFree(system);
    return exact && sawUser && sawSystem;
}

} // namespace

class SandboxLauncherTest final : public QObject
{
    Q_OBJECT

private slots:
    void deterministicProfileNamesAreBoundedAndRejectInvalidIds();
    void profileCreationReusesTheSameSid();
    void aclGrantsAreExplicitAndLeastPrivilege();
    void jobObjectHasKillProcessAndMemoryLimits();
    void closingJobKillsAssignedProcess();
    void launchProbeProvesPositiveAndNegativeBoundaries();
};

void SandboxLauncherTest::deterministicProfileNamesAreBoundedAndRejectInvalidIds()
{
    const auto first = AppContainerProfile::deterministicName(
        QStringLiteral("com.qbrowser.orders"));
    const auto second = AppContainerProfile::deterministicName(
        QStringLiteral("com.qbrowser.orders"));
    const auto different = AppContainerProfile::deterministicName(
        QStringLiteral("com.qbrowser.customers"));
    QVERIFY(first.has_value());
    QCOMPARE(first, second);
    QVERIFY(first != different);
    QVERIFY(first->size() <= 64);
    QVERIFY(first->startsWith(QStringLiteral("QBrowser.Mvp.")));
    QVERIFY(!AppContainerProfile::deterministicName(QStringLiteral("../escape"))
                 .has_value());
    QVERIFY(!AppContainerProfile::deterministicName(QStringLiteral("UPPER.CASE"))
                 .has_value());
}

void SandboxLauncherTest::profileCreationReusesTheSameSid()
{
    const QString appId = uniqueAppId(QStringLiteral("profile"));
    const QString profileName = *AppContainerProfile::deterministicName(appId);
    deleteProfileIfPresent(profileName);

    QString firstSid;
    {
        auto first = AppContainerProfile::createOrOpen(appId);
        QVERIFY(first.has_value());
        QVERIFY(first->isValid());
        QVERIFY(first->wasCreated());
        firstSid = first->sidString();
        QVERIFY(firstSid.startsWith(QStringLiteral("S-1-15-2-")));

        auto reused = AppContainerProfile::createOrOpen(appId);
        QVERIFY(reused.has_value());
        QVERIFY(reused->isValid());
        QVERIFY(!reused->wasCreated());
        QCOMPARE(reused->sidString(), firstSid);
    }
    deleteProfileIfPresent(profileName);
}

void SandboxLauncherTest::aclGrantsAreExplicitAndLeastPrivilege()
{
    const QString appId = uniqueAppId(QStringLiteral("acl"));
    const QString profileName = *AppContainerProfile::deterministicName(appId);
    deleteProfileIfPresent(profileName);
    auto profile = AppContainerProfile::createOrOpen(appId);
    QVERIFY(profile.has_value());
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString packagePath = QDir(root.path()).filePath(QStringLiteral("package"));
    const QString tempPath = QDir(root.path()).filePath(QStringLiteral("temp"));
    QVERIFY(QDir().mkpath(packagePath));
    QVERIFY(QDir().mkpath(tempPath));

    {
        auto packageGrant = AclGrant::apply(
            packagePath, profile->sid(), SandboxPathAccess::ReadOnly, true);
        auto tempGrant = AclGrant::apply(
            tempPath, profile->sid(), SandboxPathAccess::ReadWrite, true);
        QVERIFY(packageGrant.has_value());
        QVERIFY(tempGrant.has_value());
        const quint32 packageMask = explicitAllowMask(packagePath, profile->sid());
        const quint32 tempMask = explicitAllowMask(tempPath, profile->sid());
        QVERIFY(packageMask & FILE_LIST_DIRECTORY);
        QVERIFY(packageMask & FILE_READ_DATA);
        QVERIFY(!(packageMask & FILE_WRITE_DATA));
        QVERIFY(!(packageMask & FILE_DELETE_CHILD));
        QVERIFY(tempMask & FILE_LIST_DIRECTORY);
        QVERIFY(tempMask & FILE_ADD_FILE);
        QVERIFY(tempMask & FILE_WRITE_DATA);
        QVERIFY(tempMask & FILE_DELETE_CHILD);
    }

    QCOMPARE(explicitAllowMask(packagePath, profile->sid()), quint32(0));
    QCOMPARE(explicitAllowMask(tempPath, profile->sid()), quint32(0));
    profile.reset();
    deleteProfileIfPresent(profileName);
}

void SandboxLauncherTest::jobObjectHasKillProcessAndMemoryLimits()
{
    constexpr quint64 memoryLimit = 96ULL * 1024ULL * 1024ULL;
    QVERIFY(!JobLimits::create(SandboxResourceLimits{2, memoryLimit})
                 .has_value());
    auto job = JobLimits::create(
        SandboxResourceLimits{1, memoryLimit});
    QVERIFY(job.has_value());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
    QVERIFY(QueryInformationJobObject(job->nativeHandle(),
                                      JobObjectExtendedLimitInformation,
                                      &information,
                                      sizeof(information),
                                      nullptr));
    const DWORD flags = information.BasicLimitInformation.LimitFlags;
    QVERIFY(flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
    QVERIFY(flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS);
    QVERIFY(flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY);
    QCOMPARE(information.BasicLimitInformation.ActiveProcessLimit, DWORD(1));
    QCOMPARE(static_cast<quint64>(information.ProcessMemoryLimit), memoryLimit);
}

void SandboxLauncherTest::closingJobKillsAssignedProcess()
{
    const QString executable = QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH);
    const QString command = quoted(executable) + QStringLiteral(" --wait-forever");
    std::wstring mutableCommand = command.toStdWString();
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    QVERIFY(CreateProcessW(reinterpret_cast<LPCWSTR>(executable.utf16()),
                           mutableCommand.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED,
                           nullptr,
                           nullptr,
                           &startup,
                           &process));
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    {
        auto job = JobLimits::create(
            SandboxResourceLimits{1, 64ULL * 1024ULL * 1024ULL});
        QVERIFY(job.has_value());
        QVERIFY(job->assignProcess(processHandle.get()));
        QVERIFY(ResumeThread(threadHandle.get()) != DWORD(-1));
        QCOMPARE(WaitForSingleObject(processHandle.get(), 50), DWORD(WAIT_TIMEOUT));
    }
    QCOMPARE(WaitForSingleObject(processHandle.get(), 5000), DWORD(WAIT_OBJECT_0));
}

void SandboxLauncherTest::launchProbeProvesPositiveAndNegativeBoundaries()
{
    const EnvironmentVariableGuard secret(
        QByteArrayLiteral("Q_BROWSER_SANDBOX_SENTINEL_SECRET"),
        QByteArrayLiteral("must-not-cross-the-sandbox-boundary"));
    const QString appId = uniqueAppId(QStringLiteral("launch"));
    const QString profileName = *AppContainerProfile::deterministicName(appId);
    deleteProfileIfPresent(profileName);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString packagePath = QDir(root.path()).filePath(QStringLiteral("package"));
    const QString tempPath = QDir(root.path()).filePath(QStringLiteral("temp"));
    QVERIFY(QDir().mkpath(packagePath));
    QVERIFY(QDir().mkpath(tempPath));
    const QString packageFile = QDir(packagePath).filePath(QStringLiteral("allowed.txt"));
    const QString tempFile = QDir(tempPath).filePath(QStringLiteral("probe-output.txt"));
    const QString privateFile = QDir(root.path()).filePath(
        QStringLiteral("host-private.txt"));
    QFile allowed(packageFile);
    QVERIFY(allowed.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(allowed.write("allowed"), qint64(7));
    allowed.close();
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    QVERIFY(createProtectedPrivateFile(privateFile, userSid));
    QVERIFY(hasProtectedHostSystemOnlyDacl(privateFile, userSid));
    QFile privateHostRead(privateFile);
    QVERIFY(privateHostRead.open(QIODevice::ReadOnly));
    QCOMPARE(privateHostRead.readAll(), QByteArray("host-private"));
    privateHostRead.close();

    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE sentinelReadRaw = nullptr;
    HANDLE sentinelWriteRaw = nullptr;
    QVERIFY(CreatePipe(&sentinelReadRaw, &sentinelWriteRaw, &attributes, 0));
    UniqueHandle sentinelRead(sentinelReadRaw);
    UniqueHandle sentinelWrite(sentinelWriteRaw);
    QVERIFY(SetHandleInformation(sentinelRead.get(), HANDLE_FLAG_INHERIT, 0));

    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport host = pair.takeHost();
    const QString executable = QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH);
    SandboxLaunchConfig config;
    config.appId = appId;
    config.executablePath = executable;
    config.packageDirectory = packagePath;
    config.tempDirectory = tempPath;
    config.arguments = {
        QStringLiteral("--package-file"), packageFile,
        QStringLiteral("--temp-file"), tempFile,
        QStringLiteral("--private-file"), privateFile,
        QStringLiteral("--loopback-port"), QString::number(listener.serverPort()),
        QStringLiteral("--self-path"), executable};
    config.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};

    SandboxLaunchResult launched = SandboxLauncher::launch(
        config, pair.takeWorkerEnds());
    const QString launchDiagnostic = QStringLiteral("%1 native=%2")
                                         .arg(launched.errorCode)
                                         .arg(launched.nativeError);
    QVERIFY2(launched.process.has_value(),
             qPrintable(launchDiagnostic));
    QVERIFY(CloseHandle(sentinelWrite.release()));
    std::optional<QJsonObject> result;
    FrameCodec probeCodec;
    QList<QJsonObject> bufferedFrames;
    for (int attempt = 0; attempt < 5 && !result.has_value(); ++attempt) {
        const auto frame = receiveProbeFrame(
            host, 1000, probeCodec, bufferedFrames);
        if (!frame.has_value()) {
            continue;
        }
        result = frame;
    }
    const QString frameDiagnostic = QStringLiteral("frame=%1 exit=%2 pipeStatus=%3")
        .arg(result.has_value())
        .arg(launched.process->exitCode())
        .arg(static_cast<int>(host.lastStatus()));
    QVERIFY2(result.has_value(), qPrintable(frameDiagnostic));
    QVERIFY(!launched.process->waitForFinished(0));
    DWORD available = 0;
    const bool sentinelClosed = !PeekNamedPipe(sentinelRead.get(),
                                               nullptr,
                                               0,
                                               nullptr,
                                               &available,
                                               nullptr)
        && GetLastError() == ERROR_BROKEN_PIPE;
    QVERIFY(sentinelClosed);
    QVERIFY(host.writeAll(QByteArrayView("r", 1), 1000));
    const bool finished = launched.process->waitForFinished(5000);
    const QString finishDiagnostic = QStringLiteral("finished=%1 exit=%2 pipeStatus=%3")
                                         .arg(finished)
                                         .arg(launched.process->exitCode())
                                         .arg(static_cast<int>(host.lastStatus()));
    QVERIFY2(finished, qPrintable(finishDiagnostic));
    QCOMPARE(launched.process->exitCode(), DWORD(0));
    QVERIFY(result->value(QStringLiteral("packageRead")).toBool());
    QVERIFY(result->value(QStringLiteral("tempWrite")).toBool());
    const QByteArray probeJson = QJsonDocument(*result).toJson(QJsonDocument::Compact);
    QVERIFY2(!result->value(QStringLiteral("privateRead")).toBool(),
             probeJson.constData());
    QVERIFY(result->value(QStringLiteral("networkAttempted")).toBool());
    QVERIFY(!result->value(QStringLiteral("loopbackConnect")).toBool());
    QVERIFY2(result->value(QStringLiteral("networkError")).toInt() != 0,
             probeJson.constData());
    QVERIFY(!result->value(QStringLiteral("cmdCreate")).toBool());
    QVERIFY(!result->value(QStringLiteral("selfCreate")).toBool());
    QVERIFY(result->value(QStringLiteral("appContainer")).toBool());
    QVERIFY(result->value(QStringLiteral("lessPrivileged")).toBool());
    QVERIFY(!result->value(
        QStringLiteral("allApplicationPackagesMember")).toBool());
    QVERIFY(result->contains(QStringLiteral("capabilitiesQueried")));
    QVERIFY(result->value(QStringLiteral("capabilitiesQueried")).toBool());
    QCOMPARE(result->value(QStringLiteral("capabilityCount")).toInt(), 0);
    QVERIFY(result->contains(QStringLiteral("environmentSecretPresent")));
    QVERIFY(!result->value(
        QStringLiteral("environmentSecretPresent")).toBool());
    QVERIFY(result->value(QStringLiteral("jobBound")).toBool());
    QCOMPARE(result->value(QStringLiteral("appContainerSid")).toString(),
             launched.process->appContainerSid());

    launched.process.reset();
    deleteProfileIfPresent(profileName);
}

QTEST_GUILESS_MAIN(SandboxLauncherTest)

#include "tst_sandbox_launcher.moc"
