#pragma once

#include "AclGrant.h"
#include "JobLimits.h"
#include "SandboxTrustBoundary.h"
#include "WinPipeTransport.h"

#include <QString>
#include <QStringList>

#include <qt_windows.h>

#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef Q_BROWSER_SANDBOX_TESTING
namespace qbrowser_sandbox_testing
{
struct SandboxProcessTestHooks final
{
    bool forceCloseWaitTimeout = false;
    std::function<bool(const QString &)> failAclRestore;
    std::function<void(const QString &)> beforeAclGrant;
};

void setSandboxProcessTestHooks(SandboxProcessTestHooks hooks);
void resetSandboxProcessTestHooks();
[[nodiscard]] const SandboxProcessTestHooks &sandboxProcessTestHooks();
}
#endif

enum class SandboxProcessWaitResult
{
    Finished,
    Timeout,
    Error,
};

class SandboxProcessWaitHandle final
{
public:
    SandboxProcessWaitHandle() noexcept = default;
    ~SandboxProcessWaitHandle();

    SandboxProcessWaitHandle(const SandboxProcessWaitHandle &) = delete;
    SandboxProcessWaitHandle &operator=(const SandboxProcessWaitHandle &) = delete;
    SandboxProcessWaitHandle(SandboxProcessWaitHandle &&other) noexcept;
    SandboxProcessWaitHandle &operator=(SandboxProcessWaitHandle &&other) noexcept;

    [[nodiscard]] SandboxProcessWaitResult wait(int timeoutMs) const noexcept;

private:
    friend class SandboxProcess;
    explicit SandboxProcessWaitHandle(HANDLE handle) noexcept;

    HANDLE handle_ = nullptr;
};

class SandboxProcess final
{
public:
    SandboxProcess() = default;
    ~SandboxProcess();

    SandboxProcess(const SandboxProcess &) = delete;
    SandboxProcess &operator=(const SandboxProcess &) = delete;
    SandboxProcess(SandboxProcess &&other) noexcept;
    SandboxProcess &operator=(SandboxProcess &&other) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] HANDLE nativeProcessHandle() const noexcept;
    [[nodiscard]] DWORD processId() const noexcept;
    [[nodiscard]] QString appContainerSid() const;
    [[nodiscard]] SandboxValueResult<SandboxProcessWaitHandle>
        duplicateWaitHandle() const noexcept;
    bool waitForFinished(int timeoutMs) const noexcept;
    [[nodiscard]] DWORD exitCode() const noexcept;
    void requestTerminateNoWait(DWORD exitCode = ERROR_PROCESS_ABORTED) noexcept;
    void terminate(DWORD exitCode = ERROR_PROCESS_ABORTED) noexcept;
    [[nodiscard]] SandboxValueResult<bool> closeExecution() noexcept;
    [[nodiscard]] SandboxValueResult<bool> close() noexcept;
#ifdef Q_BROWSER_SANDBOX_TESTING
    [[nodiscard]] static SandboxProcess adoptForTesting(
        HANDLE process,
        DWORD processId,
        JobLimits job,
        std::vector<AclGrant> grants,
        QString appContainerSid) noexcept;
    [[nodiscard]] qsizetype pendingGrantCountForTesting() const noexcept;
#endif

private:
    friend class SandboxLauncher;
    SandboxProcess(HANDLE process,
                   DWORD processId,
                   JobLimits job,
                   std::vector<AclGrant> grants,
                   QString appContainerSid) noexcept;
    [[nodiscard]] SandboxValueResult<bool> closeExecutionLocked() noexcept;
    [[nodiscard]] SandboxValueResult<bool> closeGrantsLocked() noexcept;
    void closeBestEffort() noexcept;

    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    JobLimits job_;
    std::vector<AclGrant> grants_;
    QString appContainerSid_;
    mutable std::mutex mutex_;
    std::mutex closeMutex_;
};

struct SandboxLaunchResult final
{
    std::optional<SandboxProcess> process;
    QString errorCode;
    SandboxNativeError nativeError;
};

struct SandboxPreparedLaunchState;

class SandboxPreparedLaunch final
{
public:
    SandboxPreparedLaunch() noexcept = default;
    ~SandboxPreparedLaunch();

    SandboxPreparedLaunch(const SandboxPreparedLaunch &) = delete;
    SandboxPreparedLaunch &operator=(const SandboxPreparedLaunch &) = delete;
    SandboxPreparedLaunch(SandboxPreparedLaunch &&other) noexcept;
    SandboxPreparedLaunch &operator=(SandboxPreparedLaunch &&other) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] SandboxValueResult<bool> close() noexcept;

private:
    friend class SandboxLauncher;
    explicit SandboxPreparedLaunch(
        std::unique_ptr<SandboxPreparedLaunchState> state) noexcept;
    void closeBestEffort() noexcept;

    std::unique_ptr<SandboxPreparedLaunchState> state_;
};

class SandboxLauncher final
{
public:
    [[nodiscard]] static SandboxValueResult<SandboxPreparedLaunch> prepare(
        const SandboxLaunchConfig &config);
    static SandboxLaunchResult launch(SandboxPreparedLaunch &prepared,
                                      WorkerPipeEnds &&workerPipeEnds);
    static SandboxLaunchResult launch(const SandboxLaunchConfig &config,
                                      WorkerPipeEnds &&workerPipeEnds);

private:
    [[nodiscard]] static SandboxValueResult<SandboxPreparedLaunch> prepare(
        const SandboxLaunchConfig &config,
        bool requireMembershipSeal);
};
