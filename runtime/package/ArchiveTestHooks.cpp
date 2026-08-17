#include "ArchiveTestHooks.h"

#ifdef Q_BROWSER_ARCHIVE_TESTING

#include <utility>

namespace qbrowser_archive_testing
{
namespace
{
ArchiveTestHooks hooks;
}

void setArchiveTestHooks(ArchiveTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetArchiveTestHooks()
{
    hooks = {};
}

const ArchiveTestHooks &archiveTestHooks()
{
    return hooks;
}
}

#endif
