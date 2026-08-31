#pragma once

#include "AuthorityAdmissionToken.h"
#include "PackageInstaller.h"
#include "WorkerSupervisor.h"

#include <QString>

#include <memory>

enum class PackageRevalidationMode
{
    CurrentActivation,
    PinnedLease,
};

struct WorkerLaunchRequest final
{
    QString tabId;
    quint64 runtimeIncarnation = 0;
    QString route;
    VerifiedPackageLease lease;
    std::shared_ptr<AuthorityAdmissionToken> admission;
    WorkerAttemptKey attempt;
    PackageRevalidationMode revalidationMode =
        PackageRevalidationMode::CurrentActivation;
    bool recovery = false;

    friend bool operator==(const WorkerLaunchRequest &,
                           const WorkerLaunchRequest &) = default;
};

// Route is mutable after a Worker is attached.  Runtime ownership must instead
// be compared using the immutable launch authority that admitted the attempt.
[[nodiscard]] inline bool hasSameWorkerLaunchAuthority(
    const WorkerLaunchRequest &left,
    const WorkerLaunchRequest &right) noexcept
{
    return left.tabId == right.tabId
        && left.runtimeIncarnation == right.runtimeIncarnation
        && left.lease == right.lease
        && left.admission == right.admission
        && left.attempt == right.attempt
        && left.revalidationMode == right.revalidationMode
        && left.recovery == right.recovery;
}
