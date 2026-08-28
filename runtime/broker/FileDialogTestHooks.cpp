#include "FileDialogTestHooks.h"

#ifdef Q_BROWSER_BROKER_TESTING

#include <mutex>
#include <utility>

namespace qbrowser_broker_testing
{
namespace
{
FileDialogTestHooks hooks;
std::mutex hooksMutex;
}

void setFileDialogTestHooks(FileDialogTestHooks newHooks)
{
    const std::scoped_lock lock(hooksMutex);
    hooks = std::move(newHooks);
}

void resetFileDialogTestHooks()
{
    const std::scoped_lock lock(hooksMutex);
    hooks = {};
}

FileDialogTestHooks fileDialogTestHooks()
{
    const std::scoped_lock lock(hooksMutex);
    return hooks;
}
}

#endif
