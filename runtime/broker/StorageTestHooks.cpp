#include "StorageTestHooks.h"

#ifdef Q_BROWSER_BROKER_TESTING

#include <utility>

namespace qbrowser_broker_testing
{
namespace
{
StorageTestHooks hooks;
}

void setStorageTestHooks(StorageTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetStorageTestHooks()
{
    hooks = {};
}

const StorageTestHooks &storageTestHooks()
{
    return hooks;
}
}

#endif
