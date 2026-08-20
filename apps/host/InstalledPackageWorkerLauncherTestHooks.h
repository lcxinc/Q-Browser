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
        afterBindingValidationBeforeProcessLaunch;
    std::function<void(const UpdateLaunchRequest &)>
        afterProcessStartBeforeHandshake;
    std::function<void(quint32)> afterHandshakeBeforeCompletionQueued;
    std::function<bool(const QString &)> failWorkerTempCleanup;
};

void setInstalledPackageWorkerLauncherTestHooks(
    InstalledPackageWorkerLauncherTestHooks hooks);
void resetInstalledPackageWorkerLauncherTestHooks();
[[nodiscard]] InstalledPackageWorkerLauncherTestHooks
installedPackageWorkerLauncherTestHooks();
}

#endif
