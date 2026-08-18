#pragma once

#ifdef Q_BROWSER_PACKAGE_STORE_TESTING

#include <QString>

#include <functional>

namespace qbrowser_package_store_testing
{
struct PackageStoreTestHooks final
{
    std::function<void(const QString &, const QString &)>
        afterActivationLockAcquired;
};

void setPackageStoreTestHooks(PackageStoreTestHooks hooks);
void resetPackageStoreTestHooks();
[[nodiscard]] const PackageStoreTestHooks &packageStoreTestHooks();
}

#endif
