#pragma once

#include "PackageInstaller.h"
#include "PackageStore.h"

#include <QByteArray>
#include <QString>

class RuntimePackageAuthority final
{
public:
    RuntimePackageAuthority(QString packageStoreRoot,
                            QByteArray trustedPublicKeyPem,
                            InstallPolicy policy);
    ~RuntimePackageAuthority();

    RuntimePackageAuthority(const RuntimePackageAuthority &) = delete;
    RuntimePackageAuthority &operator=(const RuntimePackageAuthority &) = delete;

    [[nodiscard]] PackageStore &store() noexcept;
    [[nodiscard]] PackageInstaller &installer() noexcept;
    [[nodiscard]] InstallResult reverifyInstalledVersion(
        const QString &appId,
        const ActivationBinding &expected) const;

#ifdef Q_BROWSER_HOST_TESTING
    [[nodiscard]] static qsizetype liveCountForTesting() noexcept;
#endif

private:
    PackageStore store_;
    PackageInstaller installer_;
};
