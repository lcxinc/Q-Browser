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

std::optional<std::vector<wchar_t>> makeEnvironmentBlock(
    const QString &tempDirectory)
{
    std::array<wchar_t, MAX_PATH + 1> windowsBuffer{};
    const UINT windowsLength = GetWindowsDirectoryW(
        windowsBuffer.data(), static_cast<UINT>(windowsBuffer.size()));
    if (windowsLength == 0 || windowsLength >= windowsBuffer.size()
        || tempDirectory.contains(u'\0')) {
        return std::nullopt;
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
            return std::nullopt;
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
    return block;
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

    bool initialize(const DWORD count)
    {
        SIZE_T bytes = 0;
        if (InitializeProcThreadAttributeList(nullptr, count, 0, &bytes) != FALSE
            || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return false;
        }
        list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
        if (list_ == nullptr
            || !InitializeProcThreadAttributeList(list_, count, 0, &bytes)) {
            if (list_ != nullptr) {
                HeapFree(GetProcessHeap(), 0, list_);
                list_ = nullptr;
            }
            return false;
        }
        return true;
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

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
};

SandboxLaunchResult failure(const QString &code,
                            const DWORD nativeError = ERROR_INVALID_DATA)
{
    return {std::nullopt, code, nativeError};
}

bool inheritablePipeHandle(const HANDLE handle)
{
    DWORD flags = 0;
    return validHandle(handle) && GetFileType(handle) == FILE_TYPE_PIPE
        && GetHandleInformation(handle, &flags) != FALSE
        && (flags & HANDLE_FLAG_INHERIT) != 0U;
}

} // namespace

SandboxProcess::~SandboxProcess()
{
    close();
}

SandboxProcess::SandboxProcess(SandboxProcess &&other) noexcept
    : process_(std::exchange(other.process_, nullptr)),
      processId_(std::exchange(other.processId_, 0)),
      job_(std::move(other.job_)),
      grants_(std::move(other.grants_)),
      appContainerSid_(std::move(other.appContainerSid_))
{
}

SandboxProcess &SandboxProcess::operator=(SandboxProcess &&other) noexcept
{
    if (this != &other) {
        close();
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
    return validHandle(process_) && job_.isValid() && processId_ != 0;
}

HANDLE SandboxProcess::nativeProcessHandle() const noexcept
{
    return process_;
}

DWORD SandboxProcess::processId() const noexcept
{
    return processId_;
}

const QString &SandboxProcess::appContainerSid() const noexcept
{
    return appContainerSid_;
}

bool SandboxProcess::waitForFinished(const int timeoutMs) const noexcept
{
    if (!validHandle(process_) || timeoutMs < 0) {
        return false;
    }
    return WaitForSingleObject(process_, static_cast<DWORD>(timeoutMs))
        == WAIT_OBJECT_0;
}

DWORD SandboxProcess::exitCode() const noexcept
{
    DWORD code = ERROR_PROCESS_ABORTED;
    return validHandle(process_) && GetExitCodeProcess(process_, &code)
        ? code
        : ERROR_PROCESS_ABORTED;
}

void SandboxProcess::terminate(const DWORD exitCode) noexcept
{
    if (validHandle(process_)
        && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process_, exitCode);
        (void)WaitForSingleObject(process_, 5000);
    }
}

void SandboxProcess::close() noexcept
{
    job_.reset();
    if (validHandle(process_)) {
        (void)WaitForSingleObject(process_, 5000);
        CloseHandle(process_);
    }
    process_ = nullptr;
    processId_ = 0;
    for (auto grant = grants_.rbegin(); grant != grants_.rend(); ++grant) {
        (void)grant->restore();
    }
    grants_.clear();
    appContainerSid_.clear();
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

SandboxLaunchResult SandboxLauncher::launch(
    const SandboxLaunchConfig &config,
    WorkerPipeEnds &&workerPipeEnds)
{
    static_assert(!std::is_copy_constructible_v<WorkerPipeEnds>);
    if (!workerPipeEnds.isValid()) {
        return failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                       ERROR_INVALID_HANDLE);
    }
    const HANDLE workerRead = workerPipeEnds.nativeReadHandle();
    const HANDLE workerWrite = workerPipeEnds.nativeWriteHandle();
    if (workerRead == workerWrite || !inheritablePipeHandle(workerRead)
        || !inheritablePipeHandle(workerWrite)) {
        return failure(QStringLiteral("sandbox.ipc.invalid_handles"),
                       ERROR_INVALID_HANDLE);
    }

    const QFileInfo executableInfo(config.executablePath);
    const QFileInfo packageInfo(config.packageDirectory);
    const QFileInfo tempInfo(config.tempDirectory);
    if (!executableInfo.isAbsolute() || !executableInfo.isFile()
        || executableInfo.isSymLink() || !packageInfo.isAbsolute()
        || !packageInfo.isDir() || packageInfo.isSymLink()
        || !tempInfo.isAbsolute() || !tempInfo.isDir() || tempInfo.isSymLink()) {
        return failure(QStringLiteral("sandbox.path.invalid"),
                       ERROR_PATH_NOT_FOUND);
    }
    const QString executable = absolutePath(config.executablePath);
    const QString packageDirectory = absolutePath(config.packageDirectory);
    const QString tempDirectory = absolutePath(config.tempDirectory);
    if (pathWithin(packageDirectory, tempDirectory)
        || pathWithin(tempDirectory, packageDirectory)) {
        return failure(QStringLiteral("sandbox.path.overlap"));
    }

    // Keep the validated image open without write/delete sharing through
    // CreateProcess so it cannot be replaced between validation and launch.
    UniqueHandle executableLock(CreateFileW(
        reinterpret_cast<LPCWSTR>(executable.utf16()),
        GENERIC_READ | FILE_EXECUTE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    FILE_ATTRIBUTE_TAG_INFO executableAttributes{};
    if (!validHandle(executableLock.get())
        || !GetFileInformationByHandleEx(executableLock.get(),
                                         FileAttributeTagInfo,
                                         &executableAttributes,
                                         sizeof(executableAttributes))
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            != 0U
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            != 0U) {
        return failure(QStringLiteral("sandbox.path.executable_unstable"),
                       GetLastError());
    }

    auto profile = AppContainerProfile::createOrOpen(config.appId);
    if (!profile.has_value()) {
        return failure(QStringLiteral("sandbox.profile.create_failed"),
                       GetLastError());
    }
    const QString sidText = profile->sidString();
    if (sidText.isEmpty()) {
        return failure(QStringLiteral("sandbox.profile.sid_failed"));
    }

    std::vector<AclGrant> grants;
    grants.reserve(3);
    auto packageGrant = AclGrant::apply(packageDirectory,
                                        profile->sid(),
                                        SandboxPathAccess::ReadOnly,
                                        true);
    auto tempGrant = AclGrant::apply(tempDirectory,
                                     profile->sid(),
                                     SandboxPathAccess::ReadWrite,
                                     true);
    auto executableGrant = AclGrant::apply(executable,
                                           profile->sid(),
                                           SandboxPathAccess::ReadOnly,
                                           false);
    if (!packageGrant.has_value() || !tempGrant.has_value()
        || !executableGrant.has_value()) {
        return failure(QStringLiteral("sandbox.acl.grant_failed"),
                       GetLastError());
    }
    if (pathWithin(packageGrant->finalPath(), tempGrant->finalPath())
        || pathWithin(tempGrant->finalPath(), packageGrant->finalPath())) {
        return failure(QStringLiteral("sandbox.path.overlap"));
    }
    grants.push_back(std::move(*packageGrant));
    grants.push_back(std::move(*tempGrant));
    grants.push_back(std::move(*executableGrant));

    auto job = JobLimits::create(config.resourceLimits);
    if (!job.has_value()) {
        return failure(QStringLiteral("sandbox.job.create_failed"),
                       GetLastError());
    }

    ProcThreadAttributeList attributes;
    if (!attributes.initialize(3)) {
        return failure(QStringLiteral("sandbox.launch.attribute_list_failed"),
                       GetLastError());
    }
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = profile->sid();
    capabilities.Capabilities = nullptr;
    capabilities.CapabilityCount = 0;
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
        return failure(QStringLiteral("sandbox.launch.attribute_failed"),
                       GetLastError());
    }

    std::vector<wchar_t> commandLine = makeCommandLine(
        executable, config.arguments, workerRead, workerWrite);
    auto environmentBlock = makeEnvironmentBlock(tempDirectory);
    if (!environmentBlock.has_value()) {
        return failure(QStringLiteral("sandbox.launch.environment_failed"),
                       ERROR_INVALID_ENVIRONMENT);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION process{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED
        | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(executable.utf16()),
                        commandLine.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        flags,
                        environmentBlock->data(),
                        reinterpret_cast<LPCWSTR>(tempDirectory.utf16()),
                        &startup.StartupInfo,
                        &process)) {
        return failure(QStringLiteral("sandbox.launch.create_failed"),
                       GetLastError());
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    workerPipeEnds.close();

    if (!job->assignProcess(processHandle.get())) {
        const DWORD error = GetLastError();
        TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(processHandle.get(), 5000);
        return failure(QStringLiteral("sandbox.job.assign_failed"), error);
    }
    if (ResumeThread(threadHandle.get()) == DWORD(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(processHandle.get(), 5000);
        return failure(QStringLiteral("sandbox.launch.resume_failed"), error);
    }
    return {SandboxProcess(processHandle.release(),
                           process.dwProcessId,
                           std::move(*job),
                           std::move(grants),
                           sidText),
            {},
            ERROR_SUCCESS};
}
