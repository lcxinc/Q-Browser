#pragma once

#ifdef Q_BROWSER_HOST_TESTING

#include "UpdateLifecycleCoordinator.h"
#include "WorkerLaunchRequest.h"

#include <QString>

#include <functional>

namespace qbrowser_host_testing
{
struct InstalledPackageWorkerLauncherTestHooks final
{
    std::function<void(const WorkerLaunchRequest &)>
        beforeBindingValidation;
    std::function<void(const WorkerLaunchRequest &)>
        afterBindingValidationBeforeProcessLaunch;
    std::function<void(const WorkerLaunchRequest &)>
        afterProcessStartBeforeHandshake;
    std::function<void(const WorkerLaunchRequest &, quint32)>
        afterHandshakeBeforeCompletionQueued;
    std::function<void(const WorkerLaunchRequest &, bool)>
        afterAttachPublicationBeforeRealization;
    std::function<void(const WorkerLaunchRequest &)>
        beforeCommittedAttachRealization;
    std::function<void(const WorkerLaunchRequest &,
                       UpdateLifecycleCoordinator &)>
        beforeAdmissionDecision;
    std::function<bool(const QString &)> failWorkerTempCleanup;
    std::function<void()> beforeRetirementCleanup;
    std::function<void()> afterFailureSignalBeforeLifecycleEnqueue;
    std::function<void()> afterUnexpectedExitSignalBeforeExitCallback;
    std::function<void()> duringExitCallbackBeforeLifecycleEnqueue;
    std::function<void()> afterExitCallbackBeforePendingLaunch;
    std::function<void(bool)> afterLaunchFinishedBeforeThreadReturn;
    bool failLaunchThreadStart = false;
    bool failObserverThreadStart = false;
    bool throwProcessSharedAllocation = false;
    bool throwAttachRealization = false;
};

void setInstalledPackageWorkerLauncherTestHooks(
    InstalledPackageWorkerLauncherTestHooks hooks);
void resetInstalledPackageWorkerLauncherTestHooks();
[[nodiscard]] InstalledPackageWorkerLauncherTestHooks
installedPackageWorkerLauncherTestHooks();
[[nodiscard]] bool consumeLaunchThreadStartFailureForTesting();
[[nodiscard]] bool consumeObserverThreadStartFailureForTesting();
[[nodiscard]] qsizetype installedPackageWorkerLiveRetirementContexts();
[[nodiscard]] qsizetype installedPackageWorkerActiveLaunchThreads();
[[nodiscard]] qsizetype installedPackageWorkerActiveObservers();
}

#endif
