#pragma once

#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING

#include <QString>

#include <functional>

namespace qbrowser_package_installer_testing
{
struct PackageInstallerTestHooks final
{
    std::function<void(const QString &)> afterAppIdPrecheckBeforeSourceCopy;
    std::function<void(const QString &)> beforeCandidateCommit;
    std::function<void(const QString &)> afterCandidateScanBeforeSeal;
    std::function<void(const QString &, const QString &)> beforeCandidatePublish;
    std::function<void(const QString &, const QString &)> beforeActivate;
    std::function<void(const QString &, const QString &)> afterVerifyInstalled;
};

void setPackageInstallerTestHooks(PackageInstallerTestHooks hooks);
void resetPackageInstallerTestHooks();
[[nodiscard]] const PackageInstallerTestHooks &packageInstallerTestHooks();
}

#endif
