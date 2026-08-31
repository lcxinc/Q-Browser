#include "SandboxLauncher.h"

#include "AppContainerProfile.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

QString absolutePath(const QString &path)
{
    return QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

bool pathWithin(const QString &root, const QString &candidate)
{
    const QString foldedRoot = QDir::toNativeSeparators(root).toCaseFolded();
    const QString foldedCandidate = QDir::toNativeSeparators(candidate).toCaseFolded();
    return foldedCandidate == foldedRoot
        || foldedCandidate.startsWith(foldedRoot + u'\\');
}

QString quoteArgument(const QString &argument)
{
    if (!argument.isEmpty()
        && !argument.contains(u' ')
        && !argument.contains(u'\t')
        && !argument.contains(u'\n')
        && !argument.contains(u'\v')
        && !argument.contains(u'"')) {
        return argument;
    }
    QString quoted = QStringLiteral("\"");
    qsizetype backslashes = 0;
    for (const QChar character : argument) {
        if (character == u'\\') {
            ++backslashes;
            continue;
        }
        if (character == u'"') {
            quoted += QString(backslashes * 2 + 1, u'\\');
            quoted += u'"';
            backslashes = 0;
            continue;
        }
        quoted += QString(backslashes, u'\\');
        backslashes = 0;
        quoted += character;
    }
    quoted += QString(backslashes * 2, u'\\');
    quoted += u'"';
    return quoted;
}

std::vector<wchar_t> makeCommandLine(const QString &executable,
                                     const QStringList &arguments,
                                     const HANDLE readHandle,
                                     const HANDLE writeHandle)
{
    QStringList complete;
    complete.reserve(arguments.size() + 5);
    complete.append(executable);
    complete.append(arguments);
    complete.append(QStringLiteral("--qbrowser-ipc-read-handle"));
    complete.append(QString::number(reinterpret_cast<quintptr>(readHandle)));
    complete.append(QStringLiteral("--qbrowser-ipc-write-handle"));
    complete.append(QString::number(reinterpret_cast<quintptr>(writeHandle)));
    QString command;
    for (const QString &argument : complete) {
        if (!command.isEmpty()) {
            command += u' ';
        }
        command += quoteArgument(argument);
    }
    const std::wstring wideCommand = command.toStdWString();
    std::vector<wchar_t> mutableCommand(wideCommand.begin(), wideCommand.end());
    mutableCommand.push_back(L'\0');
    return mutableCommand;
}

SandboxValueResult<std::vector<wchar_t>> makeEnvironmentBlock(
    const QString &tempDirectory)
{
    std::array<wchar_t, MAX_PATH + 1> windowsBuffer{};
    const UINT windowsLength = GetWindowsDirectoryW(
        windowsBuffer.data(), static_cast<UINT>(windowsBuffer.size()));
    if (windowsLength == 0) {
        const DWORD error = GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.launch.environment_failed"),
                SandboxNativeError::win32(error != ERROR_SUCCESS
                                              ? error
                                              : ERROR_INVALID_ENVIRONMENT)};
    }
    if (windowsLength >= windowsBuffer.size()) {
        return {std::nullopt,
                QStringLiteral("sandbox.launch.environment_failed"),
                SandboxNativeError::win32(ERROR_INSUFFICIENT_BUFFER)};
    }
    if (tempDirectory.contains(u'\0')) {
        return {std::nullopt,
                QStringLiteral("sandbox.launch.environment_failed"),
                SandboxNativeError::win32(ERROR_INVALID_NAME)};
    }
    const QString windowsDirectory = QString::fromWCharArray(
        windowsBuffer.data(), static_cast<qsizetype>(windowsLength));
    static constexpr std::array<const char *, 16> safeHostVariables{
        "ALLUSERSPROFILE",
        "APPDATA",
        "CommonProgramFiles",
        "CommonProgramFiles(x86)",
        "CommonProgramW6432",
        "HOMEDRIVE",
        "HOMEPATH",
        "LOCALAPPDATA",
        "OS",
        "ProgramData",
        "ProgramFiles",
        "ProgramFiles(x86)",
        "ProgramW6432",
        "PUBLIC",
        "SystemDrive",
        "USERPROFILE"};
    std::vector<QString> variables;
    variables.reserve(safeHostVariables.size() + 4U);
    for (const char *name : safeHostVariables) {
        const QString value = qEnvironmentVariable(name);
        if (value.contains(u'\0')) {
            return {std::nullopt,
                    QStringLiteral("sandbox.launch.environment_failed"),
                    SandboxNativeError::win32(ERROR_INVALID_ENVIRONMENT)};
        }
        if (!value.isEmpty()) {
            variables.push_back(QString::fromLatin1(name) + u'=' + value);
        }
    }
    variables.push_back(QStringLiteral("SystemRoot=") + windowsDirectory);
    variables.push_back(QStringLiteral("TEMP=") + tempDirectory);
    variables.push_back(QStringLiteral("TMP=") + tempDirectory);
    variables.push_back(QStringLiteral("WINDIR=") + windowsDirectory);
    std::sort(variables.begin(), variables.end(), [](const QString &left,
                                                     const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    std::vector<wchar_t> block;
    for (const QString &variable : variables) {
        const std::wstring wide = variable.toStdWString();
        block.insert(block.end(), wide.begin(), wide.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return {std::move(block), {}, {}};
}

class ProcThreadAttributeList final
{
public:
    ~ProcThreadAttributeList()
    {
        if (list_ != nullptr) {
            DeleteProcThreadAttributeList(list_);
            HeapFree(GetProcessHeap(), 0, list_);
        }
    }

    [[nodiscard]] DWORD initialize(const DWORD count)
    {
        SIZE_T bytes = 0;
        if (InitializeProcThreadAttributeList(nullptr, count, 0, &bytes)
            != FALSE) {
            return ERROR_INVALID_DATA;
        }
        const DWORD sizingError = GetLastError();
        if (sizingError != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return sizingError != ERROR_SUCCESS ? sizingError
                                                : ERROR_INVALID_DATA;
        }
        list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
        if (list_ == nullptr) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        if (!InitializeProcThreadAttributeList(list_, count, 0, &bytes)) {
            const DWORD error = GetLastError();
            HeapFree(GetProcessHeap(), 0, list_);
            list_ = nullptr;
            return error;
        }
        return ERROR_SUCCESS;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

class UniqueHandle final
{
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle()
    {
        if (validHandle(handle_)) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept
    {
        if (this != &other) {
            if (validHandle(handle_)) CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool isValid() const noexcept { return validHandle(handle_); }
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
};

class CapabilitySet final
{
public:
    CapabilitySet() = default;
    ~CapabilitySet()
    {
        for (PSID sid : sids_) {
            LocalFree(sid);
        }
    }

    CapabilitySet(const CapabilitySet &) = delete;
    CapabilitySet &operator=(const CapabilitySet &) = delete;

    bool add(const wchar_t *name, DWORD &error)
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
            error = GetLastError();
            return false;
        }
        for (DWORD index = 0; index < groupCount; ++index) {
            LocalFree(groupSids[index]);
        }
        LocalFree(groupSids);
        if (capabilityCount != 1 || capabilitySids == nullptr) {
            for (DWORD index = 0; index < capabilityCount; ++index) {
                LocalFree(capabilitySids[index]);
            }
            LocalFree(capabilitySids);
            error = ERROR_INVALID_SID;
            return false;
        }
        sids_.push_back(capabilitySids[0]);
        LocalFree(capabilitySids);
        attributes_.push_back({sids_.back(), SE_GROUP_ENABLED});
        error = ERROR_SUCCESS;
        return true;
    }

    SID_AND_ATTRIBUTES *data() noexcept { return attributes_.data(); }
    [[nodiscard]] DWORD size() const noexcept
    {
        return static_cast<DWORD>(attributes_.size());
    }

private:
    std::vector<PSID> sids_;
    std::vector<SID_AND_ATTRIBUTES> attributes_;
};

SandboxLaunchResult failure(const QString &code,
                            const SandboxNativeError nativeError)
{
    return {std::nullopt, code, nativeError};
}

SandboxLaunchResult win32Failure(
    const QString &code,
    const DWORD nativeError = ERROR_INVALID_DATA)
{
    return failure(code, SandboxNativeError::win32(nativeError));
}

SandboxValueResult<bool> closeAclGrant(AclGrant &grant) noexcept
{
#ifdef Q_BROWSER_SANDBOX_TESTING
    const auto &hooks = qbrowser_sandbox_testing::sandboxProcessTestHooks();
    if (hooks.failAclRestore && hooks.failAclRestore(grant.finalPath())) {
        return {std::nullopt,
                QStringLiteral("sandbox.acl.restore_failed"),
                SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
    }
#endif
    return grant.close();
}

bool inheritablePipeHandle(const HANDLE handle)
{
    DWORD flags = 0;
    return validHandle(handle) && GetFileType(handle) == FILE_TYPE_PIPE
        && GetHandleInformation(handle, &flags) != FALSE
        && (flags & HANDLE_FLAG_INHERIT) != 0U;
}

} // namespace

SandboxLaunchResult SandboxLauncher::failureAfterCheckedRollback(
    std::vector<AclGrant> &grants,
    const QString &originalCode,
    const SandboxNativeError originalError)
{
    QString rollbackCode;
    SandboxNativeError rollbackError;
    for (auto grant = grants.rbegin(); grant != grants.rend(); ++grant) {
        if (!grant->isValid()) continue;
        const auto restored = closeAclGrant(*grant);
        if (!restored.value.has_value() && rollbackCode.isEmpty()) {
            rollbackCode = restored.errorCode;
            rollbackError = restored.nativeError;
        }
    }
    if (rollbackCode.isEmpty()) {
        grants.clear();
        return failure(originalCode, originalError);
    }
    SandboxLaunchResult failed = failure(rollbackCode, rollbackError);
    failed.failedLaunchCleanup = SandboxProcess(
        nullptr, 0, JobLimits{}, std::move(grants), {});
    return failed;
}

#ifdef Q_BROWSER_SANDBOX_TESTING
namespace qbrowser_sandbox_testing
{
namespace
{
SandboxProcessTestHooks currentHooks;
}

void setSandboxProcessTestHooks(SandboxProcessTestHooks hooks)
{
    currentHooks = std::move(hooks);
}

void resetSandboxProcessTestHooks()
{
    currentHooks = {};
}

const SandboxProcessTestHooks &sandboxProcessTestHooks()
{
    return currentHooks;
}
}
#endif

struct SandboxPreparedLaunchState final
{
    SandboxLaunchConfig config;
    AppContainerProfile profile;
    QString appContainerSid;
    QByteArray appContainerSidBytes;
    UniqueHandle executableLock;
    JobLimits job;
    AclGrant packageGrant;
    bool requireMembershipSeal = false;
};

SandboxPreparedLaunch::SandboxPreparedLaunch(
    std::unique_ptr<SandboxPreparedLaunchState> state) noexcept
    : state_(std::move(state))
{
}

SandboxPreparedLaunch::~SandboxPreparedLaunch()
{
    closeBestEffort();
}

SandboxPreparedLaunch::SandboxPreparedLaunch(
    SandboxPreparedLaunch &&other) noexcept = default;

SandboxPreparedLaunch &SandboxPreparedLaunch::operator=(
    SandboxPreparedLaunch &&other) noexcept
{
    if (this != &other) {
        closeBestEffort();
        state_ = std::move(other.state_);
    }
    return *this;
}

bool SandboxPreparedLaunch::isValid() const noexcept
{
    return state_ != nullptr && state_->config.isValid()
        && state_->profile.isValid() && state_->executableLock.isValid()
        && state_->job.isValid() && state_->packageGrant.isValid()
        && !state_->appContainerSid.isEmpty()
        && !state_->appContainerSidBytes.isEmpty();
}

SandboxValueResult<bool> SandboxPreparedLaunch::close() noexcept
{
    if (state_ == nullptr) return {true, {}, {}};
    const auto closed = closeAclGrant(state_->packageGrant);
    if (!closed.value.has_value()) return closed;
    state_.reset();
    return {true, {}, {}};
}

void SandboxPreparedLaunch::closeBestEffort() noexcept
{
    (void)close();
}

SandboxProcessWaitHandle::SandboxProcessWaitHandle(HANDLE handle) noexcept
    : handle_(handle)
{
}

SandboxProcessWaitHandle::~SandboxProcessWaitHandle()
{
    if (validHandle(handle_)) (void)CloseHandle(handle_);
}

SandboxProcessWaitHandle::SandboxProcessWaitHandle(
    SandboxProcessWaitHandle &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
{
}

SandboxProcessWaitHandle &SandboxProcessWaitHandle::operator=(
    SandboxProcessWaitHandle &&other) noexcept
{
    if (this != &other) {
        if (validHandle(handle_)) (void)CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

SandboxProcessWaitResult SandboxProcessWaitHandle::wait(
    const int timeoutMs) const noexcept
{
    if (!validHandle(handle_) || timeoutMs < 0)
        return SandboxProcessWaitResult::Error;
    const DWORD waited = WaitForSingleObject(
        handle_, static_cast<DWORD>(timeoutMs));
    if (waited == WAIT_OBJECT_0) return SandboxProcessWaitResult::Finished;
    if (waited == WAIT_TIMEOUT) return SandboxProcessWaitResult::Timeout;
    return SandboxProcessWaitResult::Error;
}

SandboxProcess::~SandboxProcess()
{
    closeBestEffort();
}

#ifdef Q_BROWSER_SANDBOX_TESTING
SandboxProcess SandboxProcess::adoptForTesting(
    HANDLE process,
    const DWORD processId,
    JobLimits job,
    std::vector<AclGrant> grants,
    QString appContainerSid) noexcept
{
    return SandboxProcess(process, processId, std::move(job), std::move(grants),
                          std::move(appContainerSid));
}

qsizetype SandboxProcess::pendingGrantCountForTesting() const noexcept
{
    std::lock_guard lock(mutex_);
    return static_cast<qsizetype>(grants_.size());
}
#endif

SandboxProcess::SandboxProcess(SandboxProcess &&other) noexcept
{
    std::lock_guard lock(other.mutex_);
    process_ = std::exchange(other.process_, nullptr);
    processId_ = std::exchange(other.processId_, 0);
    job_ = std::move(other.job_);
    grants_ = std::move(other.grants_);
    appContainerSid_ = std::move(other.appContainerSid_);
}

SandboxProcess &SandboxProcess::operator=(SandboxProcess &&other) noexcept
{
    if (this != &other) {
        closeBestEffort();
        std::scoped_lock lock(mutex_, other.mutex_);
        process_ = std::exchange(other.process_, nullptr);
        processId_ = std::exchange(other.processId_, 0);
        job_ = std::move(other.job_);
        grants_ = std::move(other.grants_);
        appContainerSid_ = std::move(other.appContainerSid_);
    }
    return *this;
}

bool SandboxProcess::isValid() const noexcept
{
    std::lock_guard lock(mutex_);
    return validHandle(process_) && job_.isValid() && processId_ != 0;
}

bool SandboxProcess::isRunning() const noexcept
{
    std::lock_guard lock(mutex_);
    return validHandle(process_)
        && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

HANDLE SandboxProcess::nativeProcessHandle() const noexcept
{
    std::lock_guard lock(mutex_);
    return process_;
}

DWORD SandboxProcess::processId() const noexcept
{
    std::lock_guard lock(mutex_);
    return processId_;
}

QString SandboxProcess::appContainerSid() const
{
    std::lock_guard lock(mutex_);
    return appContainerSid_;
}

SandboxValueResult<SandboxProcessWaitHandle>
SandboxProcess::duplicateWaitHandle() const noexcept
{
    HANDLE duplicate = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!validHandle(process_)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.invalid"),
                    SandboxNativeError::win32(ERROR_INVALID_HANDLE)};
        }
        if (!DuplicateHandle(GetCurrentProcess(), process_,
                             GetCurrentProcess(), &duplicate,
                             SYNCHRONIZE, FALSE, 0)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.wait_duplicate_failed"),
                    SandboxNativeError::win32(GetLastError())};
        }
    }
    return {SandboxProcessWaitHandle(duplicate), {}, {}};
}

bool SandboxProcess::waitForFinished(const int timeoutMs) const noexcept
{
    if (timeoutMs < 0) return false;
    HANDLE duplicate = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!validHandle(process_)
            || !DuplicateHandle(GetCurrentProcess(), process_,
                                GetCurrentProcess(), &duplicate,
                                SYNCHRONIZE, FALSE, 0)) {
            return false;
        }
    }
    const bool finished = WaitForSingleObject(
        duplicate, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0;
    (void)CloseHandle(duplicate);
    return finished;
}

DWORD SandboxProcess::exitCode() const noexcept
{
    std::lock_guard lock(mutex_);
    DWORD code = ERROR_PROCESS_ABORTED;
    return validHandle(process_) && GetExitCodeProcess(process_, &code)
        ? code
        : ERROR_PROCESS_ABORTED;
}

void SandboxProcess::requestTerminateNoWait(const DWORD exitCode) noexcept
{
    std::lock_guard lock(mutex_);
    if (validHandle(process_)
        && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
#ifdef Q_BROWSER_SANDBOX_TESTING
        const auto &hooks = qbrowser_sandbox_testing::sandboxProcessTestHooks();
        if (hooks.beforeTerminate) hooks.beforeTerminate(processId_);
#endif
        (void)TerminateProcess(process_, exitCode);
    }
}

void SandboxProcess::terminate(const DWORD exitCode) noexcept
{
    requestTerminateNoWait(exitCode);
    (void)waitForFinished(5000);
}

SandboxValueResult<bool> SandboxProcess::closeExecutionLocked() noexcept
{
    HANDLE waitHandle = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (validHandle(process_)
            && !DuplicateHandle(GetCurrentProcess(), process_,
                                GetCurrentProcess(), &waitHandle,
                                SYNCHRONIZE, FALSE, 0)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.wait_failed"),
                    SandboxNativeError::win32(GetLastError())};
        }
    }
    if (validHandle(waitHandle)) {
        DWORD waited = WaitForSingleObject(waitHandle, 5000);
#ifdef Q_BROWSER_SANDBOX_TESTING
        if (qbrowser_sandbox_testing::sandboxProcessTestHooks()
                .forceCloseWaitTimeout) {
            waited = WAIT_TIMEOUT;
        }
#endif
        const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        (void)CloseHandle(waitHandle);
        if (waited == WAIT_FAILED) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.wait_failed"),
                    SandboxNativeError::win32(waitError)};
        }
        if (waited != WAIT_OBJECT_0) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.wait_timeout"),
                    SandboxNativeError::win32(ERROR_TIMEOUT)};
        }
    }

    std::lock_guard lock(mutex_);
    if (validHandle(process_)) {
        if (!CloseHandle(process_)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.process.close_failed"),
                    SandboxNativeError::win32(GetLastError())};
        }
        process_ = nullptr;
        processId_ = 0;
    }
    const auto jobClosed = job_.close();
    return jobClosed;
}

SandboxValueResult<bool> SandboxProcess::closeGrantsLocked() noexcept
{
    std::lock_guard lock(mutex_);

    QString firstErrorCode;
    SandboxNativeError firstNativeError;
    for (qsizetype index = static_cast<qsizetype>(grants_.size());
         index > 0; --index) {
        AclGrant &grant = grants_[static_cast<std::size_t>(index - 1)];
        const SandboxValueResult<bool> restored = closeAclGrant(grant);
        if (restored.value.has_value()) {
            grants_.erase(grants_.begin() + (index - 1));
        } else if (firstErrorCode.isEmpty()) {
            firstErrorCode = restored.errorCode;
            firstNativeError = restored.nativeError;
        }
    }
    if (!firstErrorCode.isEmpty()) {
        return {std::nullopt, firstErrorCode, firstNativeError};
    }
    appContainerSid_.clear();
    return {true, {}, {}};
}

SandboxValueResult<bool> SandboxProcess::closeExecution() noexcept
{
    std::lock_guard closeLock(closeMutex_);
    return closeExecutionLocked();
}

SandboxValueResult<bool> SandboxProcess::close() noexcept
{
    std::lock_guard closeLock(closeMutex_);
    const auto execution = closeExecutionLocked();
    if (!execution.value.has_value()) return execution;
    return closeGrantsLocked();
}

void SandboxProcess::closeBestEffort() noexcept
{
    requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    (void)close();
}

SandboxProcess::SandboxProcess(HANDLE process,
                               const DWORD processId,
                               JobLimits job,
                               std::vector<AclGrant> grants,
                               QString appContainerSid) noexcept
    : process_(process),
      processId_(processId),
      job_(std::move(job)),
      grants_(std::move(grants)),
      appContainerSid_(std::move(appContainerSid))
{
}

SandboxValueResult<SandboxPreparedLaunch> SandboxLauncher::prepare(
    const SandboxLaunchConfig &config)
{
    return prepare(config, true);
}

SandboxValueResult<SandboxPreparedLaunch> SandboxLauncher::prepare(
    const SandboxLaunchConfig &config,
    const bool requireMembershipSeal)
{
    auto trusted = config.revalidateTrust();
    if (!trusted.value.has_value()) {
        return {std::nullopt, trusted.errorCode, trusted.nativeError};
    }
    const QFileInfo executableInfo(config.executablePath());
    const QFileInfo packageInfo(config.packageDirectory());
    const QFileInfo tempInfo(config.tempDirectory());
    if (!executableInfo.isAbsolute() || !executableInfo.isFile()
        || executableInfo.isSymLink() || !packageInfo.isAbsolute()
        || !packageInfo.isDir() || packageInfo.isSymLink()
        || !tempInfo.isAbsolute() || !tempInfo.isDir()
        || tempInfo.isSymLink()) {
        return {std::nullopt,
                QStringLiteral("sandbox.path.invalid"),
                SandboxNativeError::win32(ERROR_PATH_NOT_FOUND)};
    }
    const QString executable = absolutePath(config.executablePath());
    const QString packageDirectory = absolutePath(config.packageDirectory());
    const QString tempDirectory = absolutePath(config.tempDirectory());
    if (pathWithin(packageDirectory, tempDirectory)
        || pathWithin(tempDirectory, packageDirectory)) {
        return {std::nullopt,
                QStringLiteral("sandbox.path.overlap"),
                SandboxNativeError::win32(ERROR_INVALID_DATA)};
    }

    // Keep the validated image open without write/delete sharing before any
    // ACL mutation so it cannot be replaced between validation and launch.
    UniqueHandle executableLock(CreateFileW(
        reinterpret_cast<LPCWSTR>(executable.utf16()),
        GENERIC_READ | FILE_EXECUTE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    FILE_ATTRIBUTE_TAG_INFO executableAttributes{};
    if (!executableLock.isValid()
        || !GetFileInformationByHandleEx(executableLock.get(),
                                         FileAttributeTagInfo,
                                         &executableAttributes,
                                         sizeof(executableAttributes))
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            != 0U
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            != 0U) {
        const DWORD error = !executableLock.isValid()
            ? GetLastError()
            : (executableAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                != 0U
            ? ERROR_REPARSE_TAG_INVALID
            : (executableAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                != 0U
            ? ERROR_DIRECTORY
            : GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.path.executable_unstable"),
                SandboxNativeError::win32(error)};
    }
    auto job = JobLimits::create(config.resourceLimits());
    if (!job.value.has_value()) {
        return {std::nullopt, job.errorCode, job.nativeError};
    }
    auto profile = AppContainerProfile::createOrOpen(config.appId());
    if (!profile.has_value()) {
        return {std::nullopt, profile.errorCode, profile.nativeError};
    }
    auto sidText = profile->sidString();
    if (!sidText.value.has_value()) {
        return {std::nullopt, sidText.errorCode, sidText.nativeError};
    }
    const DWORD sidBytes = GetLengthSid(profile->sid());
    if (sidBytes == 0) {
        return {std::nullopt,
                QStringLiteral("sandbox.profile.sid_failed"),
                SandboxNativeError::win32(ERROR_INVALID_SID)};
    }
#ifdef Q_BROWSER_SANDBOX_TESTING
    if (qbrowser_sandbox_testing::sandboxProcessTestHooks().beforeAclGrant) {
        qbrowser_sandbox_testing::sandboxProcessTestHooks().beforeAclGrant(
            packageDirectory);
    }
#endif
    auto packageGrant = AclGrant::apply(packageDirectory,
                                         profile->sid(),
                                         SandboxPathAccess::ReadOnly,
                                         true);
    if (!packageGrant.has_value()) {
        return {std::nullopt,
                packageGrant.errorCode,
                packageGrant.nativeError};
    }
    auto state = std::make_unique<SandboxPreparedLaunchState>();
    state->config = config;
    state->appContainerSidBytes = QByteArray(
        static_cast<const char *>(profile->sid()),
        static_cast<qsizetype>(sidBytes));
    state->appContainerSid = std::move(*sidText.value);
    state->executableLock = std::move(executableLock);
    state->job = std::move(*job.value);
    state->packageGrant = std::move(*packageGrant);
    state->profile = std::move(*profile.value);
    state->requireMembershipSeal = requireMembershipSeal;
    return {SandboxPreparedLaunch(std::move(state)), {}, {}};
}

SandboxLaunchResult SandboxLauncher::launch(
    SandboxPreparedLaunch &prepared,
    WorkerPipeEnds &&workerPipeEnds)
{
    static_assert(!std::is_copy_constructible_v<WorkerPipeEnds>);
    if (!prepared.isValid()) {
        return win32Failure(QStringLiteral("sandbox.launch.not_prepared"),
                            ERROR_INVALID_STATE);
    }
    if (!workerPipeEnds.isValid()) {
        return win32Failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                            ERROR_INVALID_HANDLE);
    }
    const HANDLE workerRead = workerPipeEnds.nativeReadHandle();
    const HANDLE workerWrite = workerPipeEnds.nativeWriteHandle();
    if (workerRead == workerWrite || !inheritablePipeHandle(workerRead)
        || !inheritablePipeHandle(workerWrite)) {
        return win32Failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                            ERROR_INVALID_HANDLE);
    }

    auto rebound = prepared.state_->config.rebindPreparedPackageSecurity(
        prepared.state_->appContainerSidBytes,
        prepared.state_->requireMembershipSeal);
    if (!rebound.value.has_value()) {
        return failure(rebound.errorCode, rebound.nativeError);
    }
    const SandboxLaunchConfig &config = *rebound.value;
    auto trusted = config.revalidateTrust();
    if (!trusted.value.has_value()) {
        return failure(trusted.errorCode, trusted.nativeError);
    }

    const QFileInfo executableInfo(config.executablePath());
    const QFileInfo packageInfo(config.packageDirectory());
    const QFileInfo tempInfo(config.tempDirectory());
    if (!executableInfo.isAbsolute() || !executableInfo.isFile()
        || executableInfo.isSymLink() || !packageInfo.isAbsolute()
        || !packageInfo.isDir() || packageInfo.isSymLink()
        || !tempInfo.isAbsolute() || !tempInfo.isDir() || tempInfo.isSymLink()) {
        return win32Failure(QStringLiteral("sandbox.path.invalid"),
                            ERROR_PATH_NOT_FOUND);
    }
    const QString executable = absolutePath(config.executablePath());
    const QString packageDirectory = absolutePath(config.packageDirectory());
    const QString tempDirectory = absolutePath(config.tempDirectory());
    if (pathWithin(packageDirectory, tempDirectory)
        || pathWithin(tempDirectory, packageDirectory)) {
        return win32Failure(QStringLiteral("sandbox.path.overlap"));
    }

    std::vector<AclGrant> grants;
    grants.reserve(static_cast<size_t>(config.runtimeResources().size())
                   + 2U);
    auto tempGrant = AclGrant::apply(tempDirectory,
                                     prepared.state_->profile.sid(),
                                     SandboxPathAccess::ReadWrite,
                                     true);
    if (!tempGrant.has_value()) {
        return failureAfterCheckedRollback(grants,
                                           tempGrant.errorCode,
                                           tempGrant.nativeError);
    }
    if (pathWithin(prepared.state_->packageGrant.finalPath(),
                   tempGrant->finalPath())
        || pathWithin(tempGrant->finalPath(),
                      prepared.state_->packageGrant.finalPath())) {
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.path.overlap"),
            SandboxNativeError::win32(ERROR_INVALID_DATA));
    }
    grants.push_back(std::move(*tempGrant));
    for (const QString &runtimeResource : config.runtimeResources()) {
        auto runtimeGrant = AclGrant::apply(runtimeResource,
                                            prepared.state_->profile.sid(),
                                            SandboxPathAccess::ReadExecute,
                                            false);
        if (!runtimeGrant.has_value()) {
            return failureAfterCheckedRollback(grants,
                                               runtimeGrant.errorCode,
                                               runtimeGrant.nativeError);
        }
        grants.push_back(std::move(*runtimeGrant));
    }

    ProcThreadAttributeList attributes;
    const DWORD attributeListError = attributes.initialize(3);
    if (attributeListError != ERROR_SUCCESS) {
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.launch.attribute_list_failed"),
            SandboxNativeError::win32(attributeListError));
    }
    SECURITY_CAPABILITIES capabilities{};
    CapabilitySet compatibilityCapabilities;
    DWORD capabilityError = ERROR_SUCCESS;
    // Desktop Qt6Core reads Windows configuration during image startup. The
    // official LPAC model requires registryRead for registry access; the
    // launch tests pin this as the sole capability and prove network denial.
    const wchar_t *compatibilityCapabilityName = L"registryRead";
#ifdef Q_BROWSER_SANDBOX_TESTING
    switch (config.compatibilityCapabilityForTesting()) {
    case SandboxCompatibilityCapabilityForTesting::RegistryRead:
        break;
    case SandboxCompatibilityCapabilityForTesting::None:
        compatibilityCapabilityName = nullptr;
        break;
    case SandboxCompatibilityCapabilityForTesting::LpacCom:
        compatibilityCapabilityName = L"lpacCom";
        break;
    }
#endif
    if (compatibilityCapabilityName != nullptr
        && !compatibilityCapabilities.add(compatibilityCapabilityName,
                                          capabilityError)) {
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.launch.capability_failed"),
            SandboxNativeError::win32(capabilityError));
    }
    capabilities.AppContainerSid = prepared.state_->profile.sid();
    capabilities.Capabilities = compatibilityCapabilities.data();
    capabilities.CapabilityCount = compatibilityCapabilities.size();
    capabilities.Reserved = 0;
    std::array<HANDLE, 2> inheritedHandles{workerRead, workerWrite};
    DWORD allApplicationPackagesPolicy =
        PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    if (!UpdateProcThreadAttribute(attributes.get(),
                                   0,
                                   PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &capabilities,
                                   sizeof(capabilities),
                                   nullptr,
                                   nullptr)
        || !UpdateProcThreadAttribute(attributes.get(),
                                      0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inheritedHandles.data(),
                                      sizeof(inheritedHandles),
                                      nullptr,
                                      nullptr)
        || !UpdateProcThreadAttribute(attributes.get(),
                                      0,
                                      PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                                      &allApplicationPackagesPolicy,
                                      sizeof(allApplicationPackagesPolicy),
                                      nullptr,
                                      nullptr)) {
        const DWORD error = GetLastError();
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.launch.attribute_failed"),
            SandboxNativeError::win32(error));
    }

    std::vector<wchar_t> commandLine = makeCommandLine(
        executable, config.arguments(), workerRead, workerWrite);
    auto environmentBlock = makeEnvironmentBlock(tempDirectory);
    if (!environmentBlock.value.has_value()) {
        return failureAfterCheckedRollback(grants,
                                           environmentBlock.errorCode,
                                           environmentBlock.nativeError);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION process{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED
        | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    bool created = false;
#ifdef Q_BROWSER_SANDBOX_TESTING
    if (!qbrowser_sandbox_testing::sandboxProcessTestHooks()
             .forceCreateProcessFailure) {
#endif
        created = CreateProcessW(
            reinterpret_cast<LPCWSTR>(executable.utf16()),
            commandLine.data(), nullptr, nullptr, TRUE, flags,
            environmentBlock.value->data(),
            reinterpret_cast<LPCWSTR>(tempDirectory.utf16()),
            &startup.StartupInfo, &process) != FALSE;
#ifdef Q_BROWSER_SANDBOX_TESTING
    } else {
        SetLastError(ERROR_ACCESS_DENIED);
    }
#endif
    if (!created) {
        const DWORD error = GetLastError();
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.launch.create_failed"),
            SandboxNativeError::win32(error));
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    workerPipeEnds.close();

    auto assignment = prepared.state_->job.assignProcess(processHandle.get());
    if (!assignment.value.has_value()) {
        TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(processHandle.get(), 5000);
        return failureAfterCheckedRollback(grants,
                                           assignment.errorCode,
                                           assignment.nativeError);
    }
    if (ResumeThread(threadHandle.get()) == DWORD(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(processHandle.get(), 5000);
        return failureAfterCheckedRollback(
            grants,
            QStringLiteral("sandbox.launch.resume_failed"),
            SandboxNativeError::win32(error));
    }
    grants.insert(grants.begin(),
                  std::move(prepared.state_->packageGrant));
    QString appContainerSid = prepared.state_->appContainerSid;
    SandboxProcess launched(processHandle.release(),
                            process.dwProcessId,
                             std::move(prepared.state_->job),
                            std::move(grants),
                            std::move(appContainerSid));
    prepared.state_.reset();
    return {std::move(launched), {}, {}};
}

SandboxLaunchResult SandboxLauncher::launch(
    const SandboxLaunchConfig &config,
    WorkerPipeEnds &&workerPipeEnds)
{
    if (!workerPipeEnds.isValid()) {
        return win32Failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                            ERROR_INVALID_HANDLE);
    }
    const HANDLE workerRead = workerPipeEnds.nativeReadHandle();
    const HANDLE workerWrite = workerPipeEnds.nativeWriteHandle();
    if (workerRead == workerWrite || !inheritablePipeHandle(workerRead)
        || !inheritablePipeHandle(workerWrite)) {
        return win32Failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                            ERROR_INVALID_HANDLE);
    }
    auto prepared = prepare(config, false);
    if (!prepared.value.has_value()) {
        return failure(prepared.errorCode, prepared.nativeError);
    }
    SandboxLaunchResult launched = launch(
        *prepared.value, std::move(workerPipeEnds));
    if (!launched.process.has_value()) {
        if (launched.failedLaunchCleanup.has_value()) {
            launched.preparedCleanup.emplace(
                std::move(*prepared.value));
            return launched;
        }
        const auto closed = prepared->close();
        if (!closed.value.has_value()) {
            launched.errorCode = closed.errorCode;
            launched.nativeError = closed.nativeError;
            launched.preparedCleanup.emplace(
                std::move(*prepared.value));
        }
    }
    return launched;
}
