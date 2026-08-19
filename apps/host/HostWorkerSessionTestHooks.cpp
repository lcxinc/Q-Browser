#include "HostWorkerSessionTestHooks.h"

#ifdef Q_BROWSER_HOST_TESTING

#include <QMutex>
#include <QMutexLocker>

#include <utility>

namespace qbrowser_host_testing {
namespace {
QMutex hooksMutex;
HostWorkerSessionTestHooks currentHooks;
} // namespace

void setHostWorkerSessionTestHooks(HostWorkerSessionTestHooks hooks)
{
    const QMutexLocker lock(&hooksMutex);
    currentHooks = std::move(hooks);
}

void resetHostWorkerSessionTestHooks()
{
    const QMutexLocker lock(&hooksMutex);
    currentHooks = {};
}

void runBeforeIoThreadQuitHook(const quint64 generation)
{
    std::function<void(quint64)> hook;
    {
        const QMutexLocker lock(&hooksMutex);
        hook = currentHooks.beforeIoThreadQuit;
    }
    if (hook) hook(generation);
}

} // namespace qbrowser_host_testing

#endif
