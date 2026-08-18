#pragma once

#include <QtGlobal>

#include <qt_windows.h>

#include <optional>

struct SandboxResourceLimits final
{
    quint32 activeProcessLimit = 1;
    quint64 processMemoryBytes = 512ULL * 1024ULL * 1024ULL;
};

class JobLimits final
{
public:
    JobLimits() = default;
    ~JobLimits();

    JobLimits(const JobLimits &) = delete;
    JobLimits &operator=(const JobLimits &) = delete;
    JobLimits(JobLimits &&other) noexcept;
    JobLimits &operator=(JobLimits &&other) noexcept;

    static std::optional<JobLimits> create(const SandboxResourceLimits &limits);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] HANDLE nativeHandle() const noexcept;
    bool assignProcess(HANDLE process) const noexcept;
    void reset() noexcept;

private:
    explicit JobLimits(HANDLE job) noexcept;

    HANDLE job_ = nullptr;
};
