#include "FileDialogTestHooks.h"

#ifdef Q_BROWSER_BROKER_TESTING

#include <utility>

namespace qbrowser_broker_testing
{
namespace
{
FileDialogTestHooks hooks;
}

void setFileDialogTestHooks(FileDialogTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetFileDialogTestHooks()
{
    hooks = {};
}

const FileDialogTestHooks &fileDialogTestHooks()
{
    return hooks;
}
}

#endif
