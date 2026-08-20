#include "AclGrant.h"
#include "AppContainerProfile.h"
#include "FrameCodec.h"
#include "JobLimits.h"
#include "SandboxError.h"
#include "SandboxLauncher.h"
#include "SandboxTrustBoundary.h"
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
#include <winioctl.h>

#include <optional>
#include <memory>
#include <cstring>
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

QString capabilitySidString(const wchar_t *name)
{
    PSID *groupSids = nullptr;
    DWORD groupCount = 0;
    PSID *capabilitySids = nullptr;
    DWORD capabilityCount = 0;
    if (!DeriveCapabilitySidsFromName(name,
                                      &groupSids,
                                      &groupCount,
                                      &capabilitySids,
                                      &capabilityCount)) {
        return {};
    }
    for (DWORD index = 0; index < groupCount; ++index) {
        LocalFree(groupSids[index]);
    }
    LocalFree(groupSids);
    QString result;
    if (capabilityCount == 1) {
        LPWSTR converted = nullptr;
        if (ConvertSidToStringSidW(capabilitySids[0], &converted)) {
            result = QString::fromWCharArray(converted);
            LocalFree(converted);
        }
    }
    for (DWORD index = 0; index < capabilityCount; ++index) {
        LocalFree(capabilitySids[index]);
    }
    LocalFree(capabilitySids);
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

bool protectHostSystemDirectory(const QString &path,
                                const QString &userSid)
{
    const QString sddl = QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(userSid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
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
              nullptr,
              nullptr,
              dacl,
              nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

bool protectHostSystemDirectoryWithWorldAccess(const QString &path,
                                               const QString &userSid,
                                               const QString &rights)
{
    const QString sddl = QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)(A;OICI;%2;;;WD)")
                             .arg(userSid, rights);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
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
              nullptr,
              nullptr,
              dacl,
              nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

QByteArray daclSnapshot(const QString &path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return {};
    }
    const DWORD bytes = GetSecurityDescriptorLength(descriptor);
    const QByteArray snapshot(static_cast<const char *>(descriptor), bytes);
    LocalFree(descriptor);
    return snapshot;
}

bool createDirectoryJunction(const QString &junctionPath,
                             const QString &targetPath)
{
    struct MountPointReparseData final
    {
        DWORD tag;
        USHORT dataLength;
        USHORT reserved;
        USHORT substituteNameOffset;
        USHORT substituteNameLength;
        USHORT printNameOffset;
        USHORT printNameLength;
        wchar_t pathBuffer[1];
    };
    constexpr DWORD reparseHeaderBytes = sizeof(DWORD) + sizeof(USHORT) * 2U;
    if (!CreateDirectoryW(
            reinterpret_cast<LPCWSTR>(junctionPath.utf16()), nullptr)) {
        return false;
    }
    UniqueHandle junction(CreateFileW(
        reinterpret_cast<LPCWSTR>(junctionPath.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (junction.get() == nullptr || junction.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    const QString printName = QDir::toNativeSeparators(
        QFileInfo(targetPath).absoluteFilePath());
    const QString substituteName = QStringLiteral("\\??\\") + printName;
    std::vector<unsigned char> storage(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    auto *reparse = reinterpret_cast<MountPointReparseData *>(storage.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->reserved = 0;
    reparse->substituteNameOffset = 0;
    reparse->substituteNameLength =
        static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
    reparse->printNameOffset = reparse->substituteNameLength
        + sizeof(wchar_t);
    reparse->printNameLength =
        static_cast<USHORT>(printName.size() * sizeof(wchar_t));
    wchar_t *pathBuffer = reparse->pathBuffer;
    std::memcpy(pathBuffer,
                substituteName.utf16(),
                reparse->substituteNameLength);
    pathBuffer[substituteName.size()] = L'\0';
    auto *printBuffer = reinterpret_cast<wchar_t *>(
        reinterpret_cast<unsigned char *>(pathBuffer)
        + reparse->printNameOffset);
    std::memcpy(printBuffer,
                printName.utf16(),
                reparse->printNameLength);
    printBuffer[printName.size()] = L'\0';
    reparse->dataLength = static_cast<USHORT>(
        sizeof(reparse->substituteNameOffset)
        + sizeof(reparse->substituteNameLength)
        + sizeof(reparse->printNameOffset)
        + sizeof(reparse->printNameLength)
        + reparse->printNameOffset
        + reparse->printNameLength
        + sizeof(wchar_t));
    DWORD returned = 0;
    return DeviceIoControl(
               junction.get(),
               FSCTL_SET_REPARSE_POINT,
               reparse,
               reparseHeaderBytes + reparse->dataLength,
               nullptr,
               0,
               &returned,
               nullptr) != FALSE;
}

struct SandboxProcessCloseFixture final
{
    std::unique_ptr<QTemporaryDir> root;
    QString profileName;
    QString grantPath;
    QString sid;
    SandboxProcess process;
};

std::optional<SandboxProcessCloseFixture> makeSandboxProcessCloseFixture()
{
    auto root = std::make_unique<QTemporaryDir>();
    if (!root->isValid()) return std::nullopt;
    const QString appId = uniqueAppId(QStringLiteral("close-transaction"));
    auto profile = AppContainerProfile::createOrOpen(appId);
    if (!profile.has_value()) return std::nullopt;
    const auto sid = profile->sidString();
    if (!sid.value.has_value()) return std::nullopt;

    const QString executable = QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH);
    const QString command = quoted(executable) + QStringLiteral(" --wait-forever");
    std::wstring mutableCommand = command.toStdWString();
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION launched{};
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(executable.utf16()),
                        mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &launched)) {
        return std::nullopt;
    }
    UniqueHandle process(launched.hProcess);
    UniqueHandle thread(launched.hThread);
    auto job = JobLimits::create(
        SandboxResourceLimits{1, 64ULL * 1024ULL * 1024ULL});
    if (!job.has_value()
        || !job->assignProcess(process.get()).value.has_value()) {
        return std::nullopt;
    }
    const QString grantPath = root->filePath(QStringLiteral("worker-temp"));
    if (!QDir().mkdir(grantPath)) return std::nullopt;
    auto grant = AclGrant::apply(grantPath, profile->sid(),
                                 SandboxPathAccess::ReadWrite, true);
    if (!grant.has_value()) return std::nullopt;
    if (ResumeThread(thread.get()) == DWORD(-1)) return std::nullopt;
    std::vector<AclGrant> grants;
    grants.push_back(std::move(*grant));
    return SandboxProcessCloseFixture{
        std::move(root), profile->name(), grantPath, *sid.value,
        SandboxProcess::adoptForTesting(process.release(), launched.dwProcessId,
                                        std::move(*job), std::move(grants),
                                        *sid.value)};
}

} // namespace

class SandboxLauncherTest final : public QObject
{
    Q_OBJECT

private slots:
    void deterministicProfileNamesAreBoundedAndRejectInvalidIds();
    void profileCreationReusesTheSameSid();
    void nativeFailuresAreExactAndTyped();
    void trustBoundaryRejectsUntrustedRootsWithoutAclMutation();
    void trustBoundaryOnlyBuildsStrictDescendantRequests();
    void trustBoundaryRejectsSecurityStateChangesBeforeAclMutation();
    void invalidJobLimitsDoNotMutateAnyAcl();
    void aclGrantsAreExplicitAndLeastPrivilege();
    void jobObjectHasKillProcessAndMemoryLimits();
    void closingJobKillsAssignedProcess();
    void launchProbeProvesPositiveAndNegativeBoundaries();
    void dynamicQtCoreHelperLoadsInsideLpacWithMinimalRuntimeClosure();
    void closeWaitTimeoutRetainsAllOwnershipForRetry();
    void closeAclRestoreFailureRetainsGrantForRetry();
};

void SandboxLauncherTest::closeWaitTimeoutRetainsAllOwnershipForRetry()
{
    auto fixture = makeSandboxProcessCloseFixture();
    QVERIFY(fixture.has_value());
    fixture->process.requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    QVERIFY(fixture->process.waitForFinished(5000));

    qbrowser_sandbox_testing::SandboxProcessTestHooks hooks;
    hooks.forceCloseWaitTimeout = true;
    qbrowser_sandbox_testing::setSandboxProcessTestHooks(std::move(hooks));
    const auto timedOut = fixture->process.close();
    qbrowser_sandbox_testing::resetSandboxProcessTestHooks();

    QVERIFY(!timedOut.value.has_value());
    QCOMPARE(timedOut.errorCode,
             QStringLiteral("sandbox.process.wait_timeout"));
    QVERIFY(fixture->process.isValid());
    QCOMPARE(fixture->process.pendingGrantCountForTesting(), qsizetype(1));
    const auto retried = fixture->process.close();
    QVERIFY2(retried.value.has_value(), qPrintable(retried.errorCode));
    QCOMPARE(fixture->process.pendingGrantCountForTesting(), qsizetype(0));
    deleteProfileIfPresent(fixture->profileName);
}

void SandboxLauncherTest::closeAclRestoreFailureRetainsGrantForRetry()
{
    auto fixture = makeSandboxProcessCloseFixture();
    QVERIFY(fixture.has_value());
    PSID sid = nullptr;
    QVERIFY(ConvertStringSidToSidW(
        reinterpret_cast<LPCWSTR>(fixture->sid.utf16()), &sid));
    QVERIFY(explicitAllowMask(fixture->grantPath, sid) != 0U);
    fixture->process.requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    QVERIFY(fixture->process.waitForFinished(5000));

    qbrowser_sandbox_testing::SandboxProcessTestHooks hooks;
    hooks.failAclRestore = [](const QString &) { return true; };
    qbrowser_sandbox_testing::setSandboxProcessTestHooks(std::move(hooks));
    const auto failed = fixture->process.close();
    qbrowser_sandbox_testing::resetSandboxProcessTestHooks();

    QVERIFY(!failed.value.has_value());
    QCOMPARE(failed.errorCode, QStringLiteral("sandbox.acl.restore_failed"));
    QCOMPARE(fixture->process.pendingGrantCountForTesting(), qsizetype(1));
    QVERIFY(explicitAllowMask(fixture->grantPath, sid) != 0U);
    const auto retried = fixture->process.close();
    QVERIFY2(retried.value.has_value(), qPrintable(retried.errorCode));
    QCOMPARE(fixture->process.pendingGrantCountForTesting(), qsizetype(0));
    QCOMPARE(explicitAllowMask(fixture->grantPath, sid), quint32(0));
    LocalFree(sid);
    deleteProfileIfPresent(fixture->profileName);
}

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
        const auto firstSidResult = first->sidString();
        QVERIFY(firstSidResult.value.has_value());
        firstSid = *firstSidResult.value;
        QVERIFY(firstSid.startsWith(QStringLiteral("S-1-15-2-")));

        auto reused = AppContainerProfile::createOrOpen(appId);
        QVERIFY(reused.has_value());
        QVERIFY(reused->isValid());
        QVERIFY(!reused->wasCreated());
        const auto reusedSid = reused->sidString();
        QVERIFY(reusedSid.value.has_value());
        QCOMPARE(*reusedSid.value, firstSid);
    }
    deleteProfileIfPresent(profileName);
}

void SandboxLauncherTest::nativeFailuresAreExactAndTyped()
{
    const auto invalidRestore = AclGrant{}.restore();
    QVERIFY(!invalidRestore.value.has_value());
    QCOMPARE(invalidRestore.errorCode,
             QStringLiteral("sandbox.acl.restore_failed"));
    QCOMPARE(invalidRestore.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(invalidRestore.nativeError.value,
             quint32(ERROR_INVALID_HANDLE));

    const auto invalidSidText = AppContainerProfile{}.sidString();
    QVERIFY(!invalidSidText.value.has_value());
    QCOMPARE(invalidSidText.errorCode,
             QStringLiteral("sandbox.profile.sid_failed"));
    QCOMPARE(invalidSidText.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(invalidSidText.nativeError.value,
             quint32(ERROR_INVALID_SID));

    const auto invalidProfile = AppContainerProfile::createOrOpen(
        QStringLiteral("../invalid"));
    QVERIFY(!invalidProfile.value.has_value());
    QCOMPARE(invalidProfile.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(invalidProfile.nativeError.value,
             quint32(ERROR_INVALID_NAME));

    const QString appId = uniqueAppId(QStringLiteral("errors"));
    const QString profileName = *AppContainerProfile::deterministicName(appId);
    deleteProfileIfPresent(profileName);
    auto profileResult = AppContainerProfile::createOrOpen(appId);
    QVERIFY(profileResult.value.has_value());

    SetLastError(ERROR_ACCESS_DENIED);
    const auto missingGrant = AclGrant::apply(
        QStringLiteral("L:\\qbrowser-definitely-missing\\target"),
        profileResult.value->sid(),
        SandboxPathAccess::ReadOnly,
        false);
    QVERIFY(!missingGrant.value.has_value());
    QCOMPARE(missingGrant.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(missingGrant.nativeError.value,
             quint32(ERROR_PATH_NOT_FOUND));

    const auto invalidSidGrant = AclGrant::apply(
        QStringLiteral("L:\\qbrowser-definitely-missing\\target"),
        nullptr,
        SandboxPathAccess::ReadOnly,
        false);
    QVERIFY(!invalidSidGrant.value.has_value());
    QCOMPARE(invalidSidGrant.nativeError.value,
             quint32(ERROR_INVALID_SID));

    const auto invalidJob = JobLimits::create(
        SandboxResourceLimits{2, 96ULL * 1024ULL * 1024ULL});
    QVERIFY(!invalidJob.value.has_value());
    QCOMPARE(invalidJob.nativeError.value,
             quint32(ERROR_INVALID_PARAMETER));

    SetLastError(ERROR_BAD_ENVIRONMENT);
    const SandboxLaunchResult invalidLaunch = SandboxLauncher::launch(
        SandboxLaunchConfig{}, WorkerPipeEnds{});
    QVERIFY(!invalidLaunch.process.has_value());
    QCOMPARE(invalidLaunch.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(invalidLaunch.nativeError.value,
             quint32(ERROR_INVALID_HANDLE));

    profileResult.value.reset();
    deleteProfileIfPresent(profileName);
}

void SandboxLauncherTest::trustBoundaryRejectsUntrustedRootsWithoutAclMutation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    QVERIFY(QDir().mkpath(packageRoot));
    QVERIFY(QDir().mkpath(tempRoot));
    QVERIFY(QDir().mkpath(runtimeRoot));
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));
    const QByteArray packageAcl = daclSnapshot(packageRoot);
    const QByteArray tempAcl = daclSnapshot(tempRoot);
    const QByteArray runtimeAcl = daclSnapshot(runtimeRoot);
    QVERIFY(!packageAcl.isEmpty());
    QVERIFY(!tempAcl.isEmpty());
    QVERIFY(!runtimeAcl.isEmpty());

    QVERIFY(protectHostSystemDirectoryWithWorldAccess(
        runtimeRoot, userSid, QStringLiteral("GR")));
    const QByteArray broadReadAcl = daclSnapshot(runtimeRoot);
    auto broadReadAccepted = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY2(broadReadAccepted.value.has_value(),
             qPrintable(broadReadAccepted.errorCode));
    broadReadAccepted.value.reset();
    QCOMPARE(daclSnapshot(runtimeRoot), broadReadAcl);

    QVERIFY(protectHostSystemDirectoryWithWorldAccess(
        runtimeRoot, userSid, QStringLiteral("GW")));
    const QByteArray genericWriteAcl = daclSnapshot(runtimeRoot);
    const auto genericWriteRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY(!genericWriteRejected.value.has_value());
    QCOMPARE(genericWriteRejected.errorCode,
             QStringLiteral("sandbox.trust.root_not_host_owned"));
    QCOMPARE(daclSnapshot(runtimeRoot), genericWriteAcl);

    QVERIFY(protectHostSystemDirectoryWithWorldAccess(
        runtimeRoot, userSid, QStringLiteral("GA")));
    const QByteArray genericAllAcl = daclSnapshot(runtimeRoot);
    const auto genericAllRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY(!genericAllRejected.value.has_value());
    QCOMPARE(genericAllRejected.errorCode,
             QStringLiteral("sandbox.trust.root_not_host_owned"));
    QCOMPARE(daclSnapshot(runtimeRoot), genericAllAcl);
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));

    const auto volumeRootRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{QDir(packageRoot).rootPath(),
                             tempRoot,
                             {runtimeRoot}});
    QVERIFY(!volumeRootRejected.value.has_value());
    QCOMPARE(volumeRootRejected.errorCode,
             QStringLiteral("sandbox.trust.root_too_broad"));

    const QString nestedTemp = QDir(packageRoot).filePath(
        QStringLiteral("nested-temp"));
    QVERIFY(QDir().mkpath(nestedTemp));
    const auto overlapRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, nestedTemp, {runtimeRoot}});
    QVERIFY(!overlapRejected.value.has_value());
    QCOMPARE(overlapRejected.errorCode,
             QStringLiteral("sandbox.trust.roots_overlap"));

    const auto broadRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{QDir::currentPath(), tempRoot, {runtimeRoot}});
    QVERIFY(!broadRejected.value.has_value());
    QCOMPARE(broadRejected.errorCode,
             QStringLiteral("sandbox.trust.root_too_broad"));

    const QString extendedCurrent = QStringLiteral("\\\\?\\")
        + QDir::toNativeSeparators(QDir::currentPath());
    const auto extendedBroadRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{extendedCurrent, tempRoot, {runtimeRoot}});
    QVERIFY(!extendedBroadRejected.value.has_value());
    QCOMPARE(extendedBroadRejected.errorCode,
             QStringLiteral("sandbox.trust.root_too_broad"));

    BYTE everyoneBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD everyoneBytes = sizeof(everyoneBuffer);
    QVERIFY(CreateWellKnownSid(WinWorldSid,
                               nullptr,
                               everyoneBuffer,
                               &everyoneBytes));
    auto dangerousGrant = AclGrant::apply(runtimeRoot,
                                          everyoneBuffer,
                                          SandboxPathAccess::ReadWrite,
                                          true);
    QVERIFY(dangerousGrant.value.has_value());
    const QByteArray dangerousAcl = daclSnapshot(runtimeRoot);
    const auto writableRootRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY(!writableRootRejected.value.has_value());
    QCOMPARE(writableRootRejected.errorCode,
             QStringLiteral("sandbox.trust.root_not_host_owned"));
    QCOMPARE(daclSnapshot(runtimeRoot), dangerousAcl);
    dangerousGrant.value.reset();

    const QString junctionTarget = QDir(root.path()).filePath(
        QStringLiteral("junction-target"));
    const QString targetPackageRoot = QDir(junctionTarget).filePath(
        QStringLiteral("package-store"));
    const QString junctionPath = QDir(root.path()).filePath(
        QStringLiteral("junction"));
    QVERIFY(QDir().mkpath(targetPackageRoot));
    QVERIFY(protectHostSystemDirectory(targetPackageRoot, userSid));
    QVERIFY(createDirectoryJunction(junctionPath, junctionTarget));
    const QString packageThroughJunction = QDir(junctionPath).filePath(
        QStringLiteral("package-store"));
    const QByteArray junctionTargetAcl = daclSnapshot(targetPackageRoot);
    const auto reparseRejected = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageThroughJunction,
                             tempRoot,
                             {runtimeRoot}});
    QVERIFY(!reparseRejected.value.has_value());
    QCOMPARE(reparseRejected.errorCode,
             QStringLiteral("sandbox.trust.reparse_ancestor"));
    QCOMPARE(daclSnapshot(targetPackageRoot), junctionTargetAcl);

    QCOMPARE(daclSnapshot(packageRoot), packageAcl);
    QCOMPARE(daclSnapshot(tempRoot), tempAcl);
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);
}

void SandboxLauncherTest::trustBoundaryOnlyBuildsStrictDescendantRequests()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    QVERIFY(QDir().mkpath(packageRoot));
    QVERIFY(QDir().mkpath(tempRoot));
    QVERIFY(QDir().mkpath(runtimeRoot));
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));

    const QString package = QDir(packageRoot).filePath(
        QStringLiteral("orders/versions/1.0.0"));
    const QString workerTemp = QDir(tempRoot).filePath(
        QStringLiteral("orders/session-1"));
    QVERIFY(QDir().mkpath(package));
    QVERIFY(QDir().mkpath(workerTemp));
    const QString stagedExecutable = QDir(runtimeRoot).filePath(
        QStringLiteral("sandbox-probe.exe"));
    QVERIFY(QFile::copy(QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH),
                        stagedExecutable));

    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot + u'\\',
                             tempRoot + u'\\',
                             {runtimeRoot + u'\\'}});
    QVERIFY2(boundary.value.has_value(),
             qPrintable(boundary.errorCode));

    const QString lateExecutable = QDir(runtimeRoot).filePath(
        QStringLiteral("late-sandbox-probe.exe"));
    QVERIFY(QFile::copy(QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH),
                        lateExecutable));
    const QByteArray packageAclBeforeLate = daclSnapshot(packageRoot);
    const QByteArray tempAclBeforeLate = daclSnapshot(tempRoot);
    const QByteArray runtimeAclBeforeLate = daclSnapshot(runtimeRoot);
    SandboxLaunchRequest lateRequest;
    lateRequest.appId = uniqueAppId(QStringLiteral("late-runtime"));
    lateRequest.executablePath = lateExecutable;
    lateRequest.packageDirectory = package;
    lateRequest.tempDirectory = workerTemp;
    lateRequest.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};
    const auto lateRejected = boundary.value->makeLaunchConfig(lateRequest);
    QVERIFY(!lateRejected.value.has_value());
    QCOMPARE(lateRejected.errorCode,
             QStringLiteral("sandbox.trust.executable_not_in_runtime_closure"));
    QCOMPARE(daclSnapshot(packageRoot), packageAclBeforeLate);
    QCOMPARE(daclSnapshot(tempRoot), tempAclBeforeLate);
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAclBeforeLate);

    QVERIFY(!QFile::remove(stagedExecutable));

    SandboxLaunchRequest request;
    request.appId = uniqueAppId(QStringLiteral("boundary"));
    request.executablePath = stagedExecutable;
    request.packageDirectory = package;
    request.tempDirectory = workerTemp;
    request.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};
    auto accepted = boundary.value->makeLaunchConfig(request);
    QVERIFY2(accepted.value.has_value(), qPrintable(accepted.errorCode));

    const QByteArray packageAcl = daclSnapshot(packageRoot);
    const QByteArray tempAcl = daclSnapshot(tempRoot);
    const QByteArray runtimeAcl = daclSnapshot(runtimeRoot);
    request.packageDirectory = packageRoot;
    const auto equalRootRejected = boundary.value->makeLaunchConfig(request);
    QVERIFY(!equalRootRejected.value.has_value());
    QCOMPARE(equalRootRejected.errorCode,
             QStringLiteral("sandbox.trust.package_outside_root"));

    request.packageDirectory = package;
    request.tempDirectory = QDir(root.path()).filePath(
        QStringLiteral("outside-temp"));
    QVERIFY(QDir().mkpath(request.tempDirectory));
    const auto outsideTempRejected = boundary.value->makeLaunchConfig(request);
    QVERIFY(!outsideTempRejected.value.has_value());

    request.tempDirectory = workerTemp;
    request.executablePath = QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH);
    const auto outsideRuntimeRejected = boundary.value->makeLaunchConfig(request);
    QVERIFY(!outsideRuntimeRejected.value.has_value());

    QCOMPARE(daclSnapshot(packageRoot), packageAcl);
    QCOMPARE(daclSnapshot(tempRoot), tempAcl);
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);
}

void SandboxLauncherTest::invalidJobLimitsDoNotMutateAnyAcl()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    const QString package = QDir(packageRoot).filePath(
        QStringLiteral("orders/versions/1.0.0"));
    const QString workerTemp = QDir(tempRoot).filePath(
        QStringLiteral("orders/session-invalid-limits"));
    QVERIFY(QDir().mkpath(package));
    QVERIFY(QDir().mkpath(workerTemp));
    QVERIFY(QDir().mkpath(runtimeRoot));
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));
    const QString executable = QDir(runtimeRoot).filePath(
        QStringLiteral("sandbox-probe.exe"));
    QVERIFY(QFile::copy(QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH),
                        executable));

    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY2(boundary.value.has_value(), qPrintable(boundary.errorCode));
    SandboxLaunchRequest request;
    request.appId = uniqueAppId(QStringLiteral("invalid-limits"));
    request.executablePath = executable;
    request.packageDirectory = package;
    request.tempDirectory = workerTemp;
    request.resourceLimits = {2, 128ULL * 1024ULL * 1024ULL};
    auto config = boundary.value->makeLaunchConfig(request);
    QVERIFY2(config.value.has_value(), qPrintable(config.errorCode));

    const QByteArray packageAcl = daclSnapshot(package);
    const QByteArray tempAcl = daclSnapshot(workerTemp);
    const QByteArray runtimeAcl = daclSnapshot(runtimeRoot);
    const QByteArray executableAcl = daclSnapshot(executable);
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    const auto launched = SandboxLauncher::launch(
        *config.value, pair.takeWorkerEnds());
    QVERIFY(!launched.process.has_value());
    QCOMPARE(launched.errorCode,
             QStringLiteral("sandbox.job.invalid_limits"));
    QCOMPARE(launched.nativeError.value,
             quint32(ERROR_INVALID_PARAMETER));
    QCOMPARE(daclSnapshot(package), packageAcl);
    QCOMPARE(daclSnapshot(workerTemp), tempAcl);
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);
    QCOMPARE(daclSnapshot(executable), executableAcl);
}

void SandboxLauncherTest::trustBoundaryRejectsSecurityStateChangesBeforeAclMutation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    const QString package = QDir(packageRoot).filePath(
        QStringLiteral("orders/versions/1.0.0"));
    const QString workerTemp = QDir(tempRoot).filePath(
        QStringLiteral("orders/session-security-change"));
    QVERIFY(QDir().mkpath(package));
    QVERIFY(QDir().mkpath(workerTemp));
    QVERIFY(QDir().mkpath(runtimeRoot));
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));
    QVERIFY(protectHostSystemDirectory(package, userSid));
    QVERIFY(protectHostSystemDirectory(workerTemp, userSid));
    const QString executable = QDir(runtimeRoot).filePath(
        QStringLiteral("sandbox-probe.exe"));
    QVERIFY(QFile::copy(QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH),
                        executable));

    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY2(boundary.value.has_value(), qPrintable(boundary.errorCode));
    SandboxLaunchRequest request;
    request.appId = uniqueAppId(QStringLiteral("security-change"));
    request.executablePath = executable;
    request.packageDirectory = package;
    request.tempDirectory = workerTemp;
    request.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};
    auto config = boundary.value->makeLaunchConfig(request);
    QVERIFY2(config.value.has_value(), qPrintable(config.errorCode));

    QVERIFY(protectHostSystemDirectoryWithWorldAccess(
        package, userSid, QStringLiteral("GR")));
    const QByteArray changedPackageAcl = daclSnapshot(package);
    const QByteArray tempAcl = daclSnapshot(workerTemp);
    const QByteArray runtimeAcl = daclSnapshot(runtimeRoot);
    const QByteArray executableAcl = daclSnapshot(executable);
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    const auto launched = SandboxLauncher::launch(
        *config.value, pair.takeWorkerEnds());
    QVERIFY(!launched.process.has_value());
    QCOMPARE(launched.errorCode,
             QStringLiteral("sandbox.trust.security_changed"));
    QCOMPARE(launched.nativeError.value,
             quint32(ERROR_ACCESS_DENIED));
    QCOMPARE(daclSnapshot(package), changedPackageAcl);
    QCOMPARE(daclSnapshot(workerTemp), tempAcl);
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);
    QCOMPARE(daclSnapshot(executable), executableAcl);
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
    const QString runtimePath = QDir(root.path()).filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkpath(packagePath));
    QVERIFY(QDir().mkpath(tempPath));
    QVERIFY(QDir().mkpath(runtimePath));

    {
        auto packageGrant = AclGrant::apply(
            packagePath, profile->sid(), SandboxPathAccess::ReadOnly, true);
        auto tempGrant = AclGrant::apply(
            tempPath, profile->sid(), SandboxPathAccess::ReadWrite, true);
        auto runtimeGrant = AclGrant::apply(
            runtimePath, profile->sid(), SandboxPathAccess::ReadExecute, false);
        QVERIFY(packageGrant.has_value());
        QVERIFY(tempGrant.has_value());
        QVERIFY(runtimeGrant.has_value());
        const quint32 packageMask = explicitAllowMask(packagePath, profile->sid());
        const quint32 tempMask = explicitAllowMask(tempPath, profile->sid());
        const quint32 runtimeMask = explicitAllowMask(runtimePath, profile->sid());
        QVERIFY(packageMask & FILE_LIST_DIRECTORY);
        QVERIFY(packageMask & FILE_READ_DATA);
        QVERIFY(!(packageMask & FILE_WRITE_DATA));
        QVERIFY(!(packageMask & FILE_DELETE_CHILD));
        QVERIFY(!(packageMask & FILE_EXECUTE));
        QVERIFY(tempMask & FILE_LIST_DIRECTORY);
        QVERIFY(tempMask & FILE_ADD_FILE);
        QVERIFY(tempMask & FILE_WRITE_DATA);
        QVERIFY(tempMask & FILE_DELETE_CHILD);
        QVERIFY(!(tempMask & FILE_EXECUTE));
        QVERIFY(runtimeMask & FILE_READ_DATA);
        QVERIFY(runtimeMask & FILE_EXECUTE);
        QVERIFY(!(runtimeMask & FILE_WRITE_DATA));

        const auto runtimeRestored = runtimeGrant->restore();
        const auto tempRestored = tempGrant->restore();
        const auto packageRestored = packageGrant->restore();
        QVERIFY(runtimeRestored.value.has_value());
        QVERIFY(tempRestored.value.has_value());
        QVERIFY(packageRestored.value.has_value());
        QVERIFY(runtimeGrant->close().value.has_value());
        QVERIFY(tempGrant->close().value.has_value());
        QVERIFY(packageGrant->close().value.has_value());
    }

    QCOMPARE(explicitAllowMask(packagePath, profile->sid()), quint32(0));
    QCOMPARE(explicitAllowMask(tempPath, profile->sid()), quint32(0));
    QCOMPARE(explicitAllowMask(runtimePath, profile->sid()), quint32(0));
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

    SetLastError(ERROR_ACCESS_DENIED);
    const auto invalidAssignment = job->assignProcess(INVALID_HANDLE_VALUE);
    QVERIFY(!invalidAssignment.value.has_value());
    QCOMPARE(invalidAssignment.errorCode,
             QStringLiteral("sandbox.job.assign_failed"));
    QCOMPARE(invalidAssignment.nativeError.kind,
             SandboxNativeErrorKind::Win32);
    QCOMPARE(invalidAssignment.nativeError.value,
             quint32(ERROR_INVALID_HANDLE));
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
        QVERIFY(job->assignProcess(processHandle.get()).value.has_value());
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
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    QVERIFY(QDir().mkpath(packageRoot));
    QVERIFY(QDir().mkpath(tempRoot));
    QVERIFY(QDir().mkpath(runtimeRoot));
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));
    const QString packagePath = QDir(packageRoot).filePath(
        QStringLiteral("apps/probe/versions/1"));
    const QString tempPath = QDir(tempRoot).filePath(
        QStringLiteral("apps/probe/session"));
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
    const QString executable = QDir(runtimeRoot).filePath(
        QStringLiteral("q_browser_sandbox_probe.exe"));
    QVERIFY(QFile::copy(QString::fromUtf8(Q_BROWSER_SANDBOX_PROBE_PATH),
                        executable));
    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY2(boundary.value.has_value(), qPrintable(boundary.errorCode));
    SandboxLaunchRequest request;
    request.appId = appId;
    request.executablePath = executable;
    request.packageDirectory = packagePath;
    request.tempDirectory = tempPath;
    request.arguments = {
        QStringLiteral("--package-file"), packageFile,
        QStringLiteral("--temp-file"), tempFile,
        QStringLiteral("--private-file"), privateFile,
        QStringLiteral("--loopback-port"), QString::number(listener.serverPort()),
        QStringLiteral("--self-path"), executable};
    request.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};
    auto config = boundary.value->makeLaunchConfig(request);
    QVERIFY2(config.value.has_value(), qPrintable(config.errorCode));

    SandboxLaunchResult launched = SandboxLauncher::launch(
        *config.value, pair.takeWorkerEnds());
    const QString launchDiagnostic = QStringLiteral("%1 native=%2")
                                         .arg(launched.errorCode)
                                         .arg(launched.nativeError.value);
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
    QVERIFY(!result->value(QStringLiteral("packageExecuteOpen")).toBool());
    QVERIFY(!result->value(QStringLiteral("tempExecuteOpen")).toBool());
    QVERIFY(result->value(QStringLiteral("runtimeExecuteOpen")).toBool());
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
    QVERIFY(result->value(QStringLiteral("tokenGroupsQueried")).toBool());
    QVERIFY(result->value(
        QStringLiteral("tokenRestrictedSidsQueried")).toBool());
    QVERIFY(result->value(
        QStringLiteral("allApplicationPackagesQueried")).toBool());
    QVERIFY(!result->value(
        QStringLiteral("allApplicationPackagesMember")).toBool());
    QVERIFY(result->contains(QStringLiteral("capabilitiesQueried")));
    QVERIFY(result->value(QStringLiteral("capabilitiesQueried")).toBool());
    QCOMPARE(result->value(QStringLiteral("capabilityCount")).toInt(), 1);
    QVERIFY(result->contains(QStringLiteral("environmentSecretPresent")));
    QVERIFY(!result->value(
        QStringLiteral("environmentSecretPresent")).toBool());
    QVERIFY(result->value(QStringLiteral("jobBound")).toBool());
    QCOMPARE(result->value(QStringLiteral("appContainerSid")).toString(),
             launched.process->appContainerSid());

    const auto closed = launched.process->close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
    launched.process.reset();
    deleteProfileIfPresent(profileName);
}

void SandboxLauncherTest::dynamicQtCoreHelperLoadsInsideLpacWithMinimalRuntimeClosure()
{
    const QString appId = uniqueAppId(QStringLiteral("qtcore"));
    const QString profileName = *AppContainerProfile::deterministicName(appId);
    deleteProfileIfPresent(profileName);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString packageRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-package-store"));
    const QString tempRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-sandbox-temp"));
    const QString runtimeRoot = QDir(root.path()).filePath(
        QStringLiteral("approved-runtime"));
    QVERIFY(QDir().mkpath(packageRoot));
    QVERIFY(QDir().mkpath(tempRoot));
    QVERIFY(QDir().mkpath(runtimeRoot));
    const QString userSid = currentUserSidString();
    QVERIFY(!userSid.isEmpty());
    QVERIFY(protectHostSystemDirectory(packageRoot, userSid));
    QVERIFY(protectHostSystemDirectory(tempRoot, userSid));
    QVERIFY(protectHostSystemDirectory(runtimeRoot, userSid));
    const QString package = QDir(packageRoot).filePath(
        QStringLiteral("apps/qtcore/version"));
    const QString workerTemp = QDir(tempRoot).filePath(
        QStringLiteral("apps/qtcore/session"));
    QVERIFY(QDir().mkpath(package));
    QVERIFY(QDir().mkpath(workerTemp));

    const QString sourceHelper = QString::fromUtf8(
        Q_BROWSER_QT_SANDBOX_PROBE_PATH);
    const QString sourceQtCore = QString::fromUtf8(
        Q_BROWSER_QT_CORE_DLL_PATH);
    const QString stagedHelper = QDir(runtimeRoot).filePath(
        QFileInfo(sourceHelper).fileName());
    const QString stagedQtCore = QDir(runtimeRoot).filePath(
        QFileInfo(sourceQtCore).fileName());
    QVERIFY(QFile::copy(sourceHelper, stagedHelper));
    QFile qtCoreSource(sourceQtCore);
    QVERIFY2(qtCoreSource.copy(stagedQtCore),
             qPrintable(QStringLiteral("%1 -> %2: %3")
                            .arg(sourceQtCore,
                                 stagedQtCore,
                                 qtCoreSource.errorString())));
    QCOMPARE(QDir(runtimeRoot).entryList(QDir::Files).size(), qsizetype(2));

    auto boundary = SandboxTrustBoundary::create(
        SandboxApprovedRoots{packageRoot, tempRoot, {runtimeRoot}});
    QVERIFY2(boundary.value.has_value(), qPrintable(boundary.errorCode));
    SandboxLaunchRequest request;
    request.appId = appId;
    request.executablePath = stagedHelper;
    request.packageDirectory = package;
    request.tempDirectory = workerTemp;
    request.resourceLimits = {1, 128ULL * 1024ULL * 1024ULL};
    const QByteArray runtimeAcl = daclSnapshot(runtimeRoot);
    const QString untrackedRuntimeFile = QDir(runtimeRoot).filePath(
        QStringLiteral("not-in-approved-closure.dll"));
    QFile untracked(untrackedRuntimeFile);
    QVERIFY(untracked.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(untracked.write("untrusted"), qint64(9));
    untracked.close();

    request.compatibilityCapabilityForTesting =
        SandboxCompatibilityCapabilityForTesting::None;
    auto zeroConfig = boundary.value->makeLaunchConfig(request);
    QVERIFY2(zeroConfig.value.has_value(), qPrintable(zeroConfig.errorCode));
    WinPipePair zeroPair = WinPipeTransport::createHostPair();
    QVERIFY(zeroPair.isValid());
    auto zeroLaunch = SandboxLauncher::launch(
        *zeroConfig.value, zeroPair.takeWorkerEnds());
    QVERIFY2(zeroLaunch.process.has_value(), qPrintable(zeroLaunch.errorCode));
    QVERIFY(zeroLaunch.process->waitForFinished(5000));
    QCOMPARE(zeroLaunch.process->exitCode(), DWORD(0xC0000022UL));
    const auto zeroClosed = zeroLaunch.process->close();
    QVERIFY2(zeroClosed.value.has_value(), qPrintable(zeroClosed.errorCode));
    zeroLaunch.process.reset();
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);

    request.compatibilityCapabilityForTesting =
        SandboxCompatibilityCapabilityForTesting::LpacCom;
    auto lpacComConfig = boundary.value->makeLaunchConfig(request);
    QVERIFY2(lpacComConfig.value.has_value(),
             qPrintable(lpacComConfig.errorCode));
    WinPipePair lpacComPair = WinPipeTransport::createHostPair();
    QVERIFY(lpacComPair.isValid());
    auto lpacComLaunch = SandboxLauncher::launch(
        *lpacComConfig.value, lpacComPair.takeWorkerEnds());
    QVERIFY2(lpacComLaunch.process.has_value(),
             qPrintable(lpacComLaunch.errorCode));
    QVERIFY(lpacComLaunch.process->waitForFinished(5000));
    QCOMPARE(lpacComLaunch.process->exitCode(), DWORD(0xC0000022UL));
    const auto lpacComClosed = lpacComLaunch.process->close();
    QVERIFY2(lpacComClosed.value.has_value(),
             qPrintable(lpacComClosed.errorCode));
    lpacComLaunch.process.reset();
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);

    request.compatibilityCapabilityForTesting =
        SandboxCompatibilityCapabilityForTesting::RegistryRead;
    auto config = boundary.value->makeLaunchConfig(request);
    QVERIFY2(config.value.has_value(), qPrintable(config.errorCode));

    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport host = pair.takeHost();
    SandboxLaunchResult launched = SandboxLauncher::launch(
        *config.value, pair.takeWorkerEnds());
    const QString diagnostic = QStringLiteral("%1 native=%2")
                                   .arg(launched.errorCode)
                                   .arg(launched.nativeError.value);
    QVERIFY2(launched.process.has_value(), qPrintable(diagnostic));
    PSID launchedSid = nullptr;
    QVERIFY(ConvertStringSidToSidW(
        reinterpret_cast<LPCWSTR>(launched.process->appContainerSid().utf16()),
        &launchedSid));
    QVERIFY(explicitAllowMask(runtimeRoot, launchedSid) & FILE_LIST_DIRECTORY);
    QVERIFY(explicitAllowMask(stagedHelper, launchedSid) & FILE_EXECUTE);
    QVERIFY(explicitAllowMask(stagedQtCore, launchedSid) & FILE_EXECUTE);
    QCOMPARE(explicitAllowMask(untrackedRuntimeFile, launchedSid), quint32(0));
    LocalFree(launchedSid);
    FrameCodec codec;
    QList<QJsonObject> frames;
    const auto result = receiveProbeFrame(host, 5000, codec, frames);
    const QString frameDiagnostic = QStringLiteral("exit=%1 pipe=%2")
                                        .arg(launched.process->exitCode())
                                        .arg(static_cast<int>(host.lastStatus()));
    QVERIFY2(result.has_value(), qPrintable(frameDiagnostic));
    QVERIFY(!result->value(QStringLiteral("qtVersion")).toString().isEmpty());
    QVERIFY(result->value(QStringLiteral("appContainer")).toBool());
    QVERIFY(result->value(QStringLiteral("tokenGroupsQueried")).toBool());
    QVERIFY(result->value(
        QStringLiteral("tokenRestrictedSidsQueried")).toBool());
    QVERIFY(result->value(
        QStringLiteral("allApplicationPackagesQueried")).toBool());
    QVERIFY(!result->value(
        QStringLiteral("allApplicationPackagesMember")).toBool());
    QVERIFY(result->value(QStringLiteral("capabilitiesQueried")).toBool());
    QCOMPARE(result->value(QStringLiteral("capabilityCount")).toInt(), 1);
    const QString registryReadSid = capabilitySidString(L"registryRead");
    const QString internetClientSid = capabilitySidString(L"internetClient");
    const QString internetClientServerSid = capabilitySidString(
        L"internetClientServer");
    const QString privateNetworkSid = capabilitySidString(
        L"privateNetworkClientServer");
    QVERIFY(!registryReadSid.isEmpty());
    QVERIFY(!internetClientSid.isEmpty());
    QVERIFY(!internetClientServerSid.isEmpty());
    QVERIFY(!privateNetworkSid.isEmpty());
    QCOMPARE(result->value(QStringLiteral("capabilitySid")).toString(),
             registryReadSid);
    QVERIFY(result->value(QStringLiteral("capabilitySid")).toString()
            != internetClientSid);
    QVERIFY(result->value(QStringLiteral("capabilitySid")).toString()
            != internetClientServerSid);
    QVERIFY(result->value(QStringLiteral("capabilitySid")).toString()
            != privateNetworkSid);
    QVERIFY(host.writeAll(QByteArrayView("r", 1), 1000));
    QVERIFY(launched.process->waitForFinished(5000));
    QCOMPARE(launched.process->exitCode(), DWORD(0));
    const auto closed = launched.process->close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
    launched.process.reset();
    QCOMPARE(daclSnapshot(runtimeRoot), runtimeAcl);
    deleteProfileIfPresent(profileName);
}

QTEST_GUILESS_MAIN(SandboxLauncherTest)

#include "tst_sandbox_launcher.moc"
