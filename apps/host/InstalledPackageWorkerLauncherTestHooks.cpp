#include "InstalledPackageWorkerLauncherTestHooks.h"

#ifdef Q_BROWSER_HOST_TESTING

#include <utility>
#include <mutex>

namespace qbrowser_host_testing
{
namespace
{
InstalledPackageWorkerLauncherTestHooks currentHooks;
std::mutex hooksMutex;
}

void setInstalledPackageWorkerLauncherTestHooks(
    InstalledPackageWorkerLauncherTestHooks hooks)
{
    std::lock_guard lock(hooksMutex);
    currentHooks = std::move(hooks);
}

void resetInstalledPackageWorkerLauncherTestHooks()
{
    std::lock_guard lock(hooksMutex);
    currentHooks = {};
}

InstalledPackageWorkerLauncherTestHooks
installedPackageWorkerLauncherTestHooks()
{
    std::lock_guard lock(hooksMutex);
    return currentHooks;
}
}

#endif
