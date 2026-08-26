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
