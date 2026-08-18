#include "PackageStoreTestHooks.h"

#ifdef Q_BROWSER_PACKAGE_STORE_TESTING

#include <utility>

namespace qbrowser_package_store_testing
{
namespace
{
PackageStoreTestHooks hooks;
}

void setPackageStoreTestHooks(PackageStoreTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetPackageStoreTestHooks()
{
    hooks = {};
}

const PackageStoreTestHooks &packageStoreTestHooks()
{
    return hooks;
}
}

#endif
