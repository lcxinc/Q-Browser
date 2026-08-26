#include "RuntimePackageAuthority.h"

#include <atomic>
#include <utility>

namespace
{
#ifdef Q_BROWSER_HOST_TESTING
std::atomic<qsizetype> liveAuthorities{0};
#endif
}

RuntimePackageAuthority::RuntimePackageAuthority(
    QString packageStoreRoot,
    QByteArray trustedPublicKeyPem,
    InstallPolicy policy)
    : store_(std::move(packageStoreRoot))
    , installer_(store_, std::move(trustedPublicKeyPem), std::move(policy))
{
#ifdef Q_BROWSER_HOST_TESTING
    liveAuthorities.fetch_add(1, std::memory_order_relaxed);
#endif
}

RuntimePackageAuthority::~RuntimePackageAuthority()
{
#ifdef Q_BROWSER_HOST_TESTING
    liveAuthorities.fetch_sub(1, std::memory_order_relaxed);
#endif
}

PackageStore &RuntimePackageAuthority::store() noexcept
{
    return store_;
}

PackageInstaller &RuntimePackageAuthority::installer() noexcept
{
    return installer_;
}

InstallResult RuntimePackageAuthority::reverifyInstalledVersion(
    const QString &appId,
    const ActivationBinding &expected) const
{
    return installer_.reverifyInstalledVersion(appId, expected);
}

InstallResult RuntimePackageAuthority::revalidateWorkerLaunch(
    const WorkerLaunchRequest &request,
    std::shared_ptr<const ImmutablePackageGuard> retainedGuard) const
{
    if (request.revalidationMode == PackageRevalidationMode::PinnedLease) {
        return installer_.reverifyPinnedLease(
            request.lease, std::move(retainedGuard));
    }
    const ActivationBinding expected{
        request.lease.versionDirectory,
        request.lease.digestHex,
        request.lease.activationGenerationAtIssue};
    return installer_.reverifyInstalledVersion(
        request.lease.appId, expected, std::move(retainedGuard));
}

#ifdef Q_BROWSER_HOST_TESTING
qsizetype RuntimePackageAuthority::liveCountForTesting() noexcept
{
    return liveAuthorities.load(std::memory_order_relaxed);
}
#endif
