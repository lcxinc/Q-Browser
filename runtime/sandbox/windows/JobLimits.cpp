#include "JobLimits.h"

#include <limits>
#include <utility>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

} // namespace

JobLimits::~JobLimits()
{
    closeBestEffort();
}

JobLimits::JobLimits(JobLimits &&other) noexcept
    : job_(std::exchange(other.job_, nullptr))
{
}

JobLimits &JobLimits::operator=(JobLimits &&other) noexcept
{
    if (this != &other) {
        closeBestEffort();
        job_ = std::exchange(other.job_, nullptr);
    }
    return *this;
}

SandboxValueResult<JobLimits> JobLimits::create(
    const SandboxResourceLimits &limits)
{
    if (limits.activeProcessLimit != 1
        || limits.processMemoryBytes < 16ULL * 1024ULL * 1024ULL
        || limits.processMemoryBytes
            > static_cast<quint64>(std::numeric_limits<SIZE_T>::max())) {
        return {std::nullopt,
                QStringLiteral("sandbox.job.invalid_limits"),
                SandboxNativeError::win32(ERROR_INVALID_PARAMETER)};
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!validHandle(job)) {
        return {std::nullopt,
                QStringLiteral("sandbox.job.create_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
    information.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS
        | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    information.BasicLimitInformation.ActiveProcessLimit =
        limits.activeProcessLimit;
    information.ProcessMemoryLimit =
        static_cast<SIZE_T>(limits.processMemoryBytes);
    if (!SetInformationJobObject(job,
                                 JobObjectExtendedLimitInformation,
                                 &information,
                                 sizeof(information))) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        return {std::nullopt,
                QStringLiteral("sandbox.job.set_limits_failed"),
                SandboxNativeError::win32(error)};
    }
    return {JobLimits(job), {}, {}};
}

bool JobLimits::isValid() const noexcept
{
    return validHandle(job_);
}

HANDLE JobLimits::nativeHandle() const noexcept
{
    return job_;
}

SandboxValueResult<bool> JobLimits::assignProcess(
    const HANDLE process) const noexcept
{
    if (!isValid() || !validHandle(process)) {
        return {std::nullopt,
                QStringLiteral("sandbox.job.assign_failed"),
                SandboxNativeError::win32(ERROR_INVALID_HANDLE)};
    }
    if (!AssignProcessToJobObject(job_, process)) {
        const DWORD error = GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.job.assign_failed"),
                SandboxNativeError::win32(error)};
    }
    return {true, {}, {}};
}

SandboxValueResult<bool> JobLimits::close() noexcept
{
    if (!validHandle(job_)) {
        return {true, {}, {}};
    }
    if (!CloseHandle(job_)) {
        const DWORD error = GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.job.close_failed"),
                SandboxNativeError::win32(error)};
    }
    job_ = nullptr;
    return {true, {}, {}};
}

void JobLimits::closeBestEffort() noexcept
{
    (void)close();
}

JobLimits::JobLimits(const HANDLE job) noexcept : job_(job) {}
