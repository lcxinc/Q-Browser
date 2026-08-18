#pragma once

#include "AclGrant.h"
#include "JobLimits.h"
#include "WinPipeTransport.h"

#include <QString>
#include <QStringList>

#include <qt_windows.h>

#include <optional>
#include <vector>

struct SandboxLaunchConfig final
{
    QString appId;
    QString executablePath;
    QString packageDirectory;
    QString tempDirectory;
    QStringList arguments;
    SandboxResourceLimits resourceLimits;
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
    [[nodiscard]] HANDLE nativeProcessHandle() const noexcept;
    [[nodiscard]] DWORD processId() const noexcept;
    [[nodiscard]] const QString &appContainerSid() const noexcept;
    bool waitForFinished(int timeoutMs) const noexcept;
    [[nodiscard]] DWORD exitCode() const noexcept;
    void terminate(DWORD exitCode = ERROR_PROCESS_ABORTED) noexcept;
    void close() noexcept;

private:
    friend class SandboxLauncher;
    SandboxProcess(HANDLE process,
                   DWORD processId,
                   JobLimits job,
                   std::vector<AclGrant> grants,
                   QString appContainerSid) noexcept;

    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    JobLimits job_;
    std::vector<AclGrant> grants_;
    QString appContainerSid_;
};

struct SandboxLaunchResult final
{
    std::optional<SandboxProcess> process;
    QString errorCode;
    quint32 nativeError = ERROR_SUCCESS;
};

class SandboxLauncher final
{
public:
    static SandboxLaunchResult launch(const SandboxLaunchConfig &config,
                                      WorkerPipeEnds &&workerPipeEnds);
};
