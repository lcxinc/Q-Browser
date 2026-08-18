#include "PackageInstallerTestHooks.h"

#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING

#include <utility>

namespace qbrowser_package_installer_testing
{
namespace
{
PackageInstallerTestHooks hooks;
}

void setPackageInstallerTestHooks(PackageInstallerTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetPackageInstallerTestHooks()
{
    hooks = {};
}

const PackageInstallerTestHooks &packageInstallerTestHooks()
{
    return hooks;
}
}

#endif
