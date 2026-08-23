#pragma once

#ifdef Q_BROWSER_HOST_TESTING

#include "UpdateLifecycleCoordinator.h"

#include <QString>

#include <functional>

namespace qbrowser_host_testing
{
struct InstalledPackageWorkerLauncherTestHooks final
{
    std::function<void(const UpdateLaunchRequest &)>
        beforeBindingValidation;
    std::function<void(const UpdateLaunchRequest &)>
        afterBindingValidationBeforeProcessLaunch;
    std::function<void(const UpdateLaunchRequest &)>
        afterProcessStartBeforeHandshake;
    std::function<void(quint32)> afterHandshakeBeforeCompletionQueued;
    std::function<void(const UpdateLaunchRequest &,
                       UpdateLifecycleCoordinator &)>
        beforeAdmissionDecision;
    std::function<bool(const QString &)> failWorkerTempCleanup;
    std::function<void()> beforeRetirementCleanup;
    std::function<void()> afterFailureSignalBeforeLifecycleEnqueue;
    bool failLaunchThreadStart = false;
    bool failObserverThreadStart = false;
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
