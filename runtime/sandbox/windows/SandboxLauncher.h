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
#include <mutex>
#include <vector>

#ifdef Q_BROWSER_SANDBOX_TESTING
namespace qbrowser_sandbox_testing
{
struct SandboxProcessTestHooks final
{
    bool forceCloseWaitTimeout = false;
    std::function<bool(const QString &)> failAclRestore;
};

void setSandboxProcessTestHooks(SandboxProcessTestHooks hooks);
void resetSandboxProcessTestHooks();
[[nodiscard]] const SandboxProcessTestHooks &sandboxProcessTestHooks();
}
#endif

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
    bool waitForFinished(int timeoutMs) const noexcept;
    [[nodiscard]] DWORD exitCode() const noexcept;
    void requestTerminateNoWait(DWORD exitCode = ERROR_PROCESS_ABORTED) noexcept;
    void terminate(DWORD exitCode = ERROR_PROCESS_ABORTED) noexcept;
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

class SandboxLauncher final
{
public:
    static SandboxLaunchResult launch(const SandboxLaunchConfig &config,
                                      WorkerPipeEnds &&workerPipeEnds);
};
