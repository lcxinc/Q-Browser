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
    reset();
}

JobLimits::JobLimits(JobLimits &&other) noexcept
    : job_(std::exchange(other.job_, nullptr))
{
}

JobLimits &JobLimits::operator=(JobLimits &&other) noexcept
{
    if (this != &other) {
        reset();
        job_ = std::exchange(other.job_, nullptr);
    }
    return *this;
}

std::optional<JobLimits> JobLimits::create(
    const SandboxResourceLimits &limits)
{
    if (limits.activeProcessLimit != 1
        || limits.processMemoryBytes < 16ULL * 1024ULL * 1024ULL
        || limits.processMemoryBytes
            > static_cast<quint64>(std::numeric_limits<SIZE_T>::max())) {
        return std::nullopt;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!validHandle(job)) {
        return std::nullopt;
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
        CloseHandle(job);
        return std::nullopt;
    }
    return JobLimits(job);
}

bool JobLimits::isValid() const noexcept
{
    return validHandle(job_);
}

HANDLE JobLimits::nativeHandle() const noexcept
{
    return job_;
}

bool JobLimits::assignProcess(const HANDLE process) const noexcept
{
    return isValid() && validHandle(process)
        && AssignProcessToJobObject(job_, process) != FALSE;
}

void JobLimits::reset() noexcept
{
    if (validHandle(job_)) {
        CloseHandle(job_);
    }
    job_ = nullptr;
}

JobLimits::JobLimits(const HANDLE job) noexcept : job_(job) {}
