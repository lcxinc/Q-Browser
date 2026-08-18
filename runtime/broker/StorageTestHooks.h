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
};

void setStorageTestHooks(StorageTestHooks hooks);
void resetStorageTestHooks();
[[nodiscard]] const StorageTestHooks &storageTestHooks();
}

#endif
