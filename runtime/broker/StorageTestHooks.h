#pragma once

#ifdef Q_BROWSER_BROKER_TESTING

#include <QString>

#include <functional>

namespace qbrowser_broker_testing
{
struct StorageTestHooks final
{
    std::function<void()> afterMembershipFrozen;
    std::function<bool(const QString &, qsizetype)> allowAclApply;
    std::function<bool(const QString &, qsizetype)> allowAclPostcheck;
    std::function<void(const QString &)> afterInitializationLockAcquired;
    std::function<void(const QString &)> initializationLockContended;
    std::function<void(const QString &)> storageOperationLockContended;
    std::function<void(const QString &)> afterStorageOperationLockAcquired;
    std::function<void(const QString &)> beforeStorageIo;
    std::function<bool(const QString &, qsizetype)> allowPermissionApply;
    std::function<bool(const QString &, qsizetype)> allowPermissionPostcheck;
};

void setStorageTestHooks(StorageTestHooks hooks);
void resetStorageTestHooks();
[[nodiscard]] const StorageTestHooks &storageTestHooks();
}

#endif
