#pragma once

#ifdef Q_BROWSER_HOST_TESTING

#include <QtGlobal>

#include <functional>

namespace qbrowser_host_testing {

struct HostWorkerSessionTestHooks final
{
    std::function<void(quint64)> beforeIoThreadQuit;
};

void setHostWorkerSessionTestHooks(HostWorkerSessionTestHooks hooks);
void resetHostWorkerSessionTestHooks();
void runBeforeIoThreadQuitHook(quint64 generation);

} // namespace qbrowser_host_testing

#endif
