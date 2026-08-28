#pragma once

#include "UpdateLifecycleCoordinator.h"

#include <QVector>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class AppRuntimeCoordinator;

struct TabLaunchAuthority final
{
    QString tabId;
    quint64 runtimeIncarnation = 0;

    friend bool operator==(const TabLaunchAuthority &,
                           const TabLaunchAuthority &) = default;
};

enum class TabLaunchIntent
{
    ActivateCurrent,
    ReloadCurrent,
    RestartPinned,
};

struct FullAttemptKey final
{
    TabLaunchAuthority tab;
    WorkerAttemptKey attempt;
    quint64 leaseAuthorityEpoch = 0;

    friend bool operator==(const FullAttemptKey &, const FullAttemptKey &)
        = default;
};

struct AuthorityDrainBatch final
{
    quint64 id = 0;
    QVector<AuthorityAdmissionToken::RevocationTicket> tickets;
    qint64 monotonicDeadlineMs = 0;
};

enum class AppRuntimeActionKind
{
    None,
    Launch,
    Stop,
    Revoke,
    AwaitAuthorityDrain,
    IsolateSession,
    RecoverFromLkg,
    TrustedCrash,
    FailedClosed,
};

struct AppRuntimeAction final
{
    AppRuntimeActionKind kind = AppRuntimeActionKind::None;
    QString tabId;
    quint64 runtimeIncarnation = 0;
    std::optional<WorkerLaunchRequest> launch;
    std::optional<AuthorityDrainBatch> drain;
};

enum class AppRuntimeResultCode
{
    Applied,
    IgnoredStale,
    Rejected,
    FailedClosed,
};

struct AppRuntimeResult final
{
    AppRuntimeResultCode code = AppRuntimeResultCode::Applied;
    QString stableError;
    QVector<AppRuntimeAction> actions;
    quint32 nativeError = 0;
};

class AppRuntimeCoordinator final
{
public:
    using DrainConsumer = std::function<void(const AuthorityDrainBatch &)>;

    AppRuntimeCoordinator(QString appId,
                          PackageStore &store,
                          PackageInstaller &installer,
                          WorkerSupervisionPolicy supervisionPolicy,
                          LifecycleClock clock = LifecycleClock::system(),
                          EventRecorder *recorder = nullptr,
                          DrainConsumer drainConsumer = {},
                          qint64 authorityDrainTimeoutMs = 10'000);
    AppRuntimeCoordinator(QString appId,
                          PackageStore &store,
                          PackageInstaller &installer,
                          WorkerSupervisionPolicy supervisionPolicy,
                          LifecycleClock clock,
                          DrainConsumer drainConsumer,
                          qint64 authorityDrainTimeoutMs = 10'000);

    [[nodiscard]] AppRuntimeResult installAndActivate(const QString &packagePath,
                                                       qint64 nowMs);
    [[nodiscard]] AppRuntimeResult startOffline(qint64 nowMs);
    [[nodiscard]] AppRuntimeResult requestTabLaunch(
        const TabLaunchAuthority &tab,
        const QString &route,
        TabLaunchIntent intent,
        qint64 nowMs);

    // This is the plan-level compatibility entry point. New Host code should
    // pass the complete WorkerLaunchRequest overload below.
    [[nodiscard]] AppRuntimeResult admitAuthenticatedWorker(
        const FullAttemptKey &key,
        const VerifiedPackageLease &lease);
    [[nodiscard]] AppRuntimeResult admitAuthenticatedWorker(
        const FullAttemptKey &key,
        const VerifiedPackageLease &lease,
        qint64 receivedMonotonicMs);
    [[nodiscard]] AppRuntimeResult admitAuthenticatedWorker(
        const WorkerLaunchRequest &request,
        qint64 receivedMonotonicMs = -1);

    [[nodiscard]] AppRuntimeResult heartbeat(
        const FullAttemptKey &key,
        qint64 receivedMonotonicMs);
    [[nodiscard]] AppRuntimeResult checkHealth(qint64 nowMs);
    [[nodiscard]] AppRuntimeResult workerExited(
        const FullAttemptKey &key,
        WorkerExitReason reason,
        qint64 nowMs);
    [[nodiscard]] AppRuntimeResult workerAdmissionFailed(
        const FullAttemptKey &key,
        const QString &stableError,
        qint64 nowMs = -1);
    [[nodiscard]] AppRuntimeResult workerCleanupFailed(
        const FullAttemptKey &key,
        const QString &stableError,
        qint64 nowMs = -1);
    [[nodiscard]] AppRuntimeResult workerCleanupFailed(
        const FullAttemptKey &key,
        const QString &stableError,
        quint32 nativeError,
        qint64 nowMs = -1);

    [[nodiscard]] AppRuntimeResult authorityDrainTimedOut(
        quint64 batchId,
        qint64 nowMs);
    [[nodiscard]] AppRuntimeResult authorityDrainCompleted(
        quint64 batchId,
        qint64 nowMs);
    [[nodiscard]] AppRuntimeResult closeTab(const TabLaunchAuthority &tab,
                                            qint64 nowMs = -1);
    [[nodiscard]] AppRuntimeResult beginShutdown(qint64 nowMs = -1);

private:
    struct VersionDescriptor final
    {
        VerifiedPackageLease lease;
        ActivationBinding binding;
    };

    struct TabState final
    {
        TabState(TabLaunchAuthority value,
                 WorkerSupervisionPolicy policy);

        TabLaunchAuthority authority;
        std::unique_ptr<WorkerSupervisor> supervisor;
        WorkerLaunchRequest request;
        VerifiedPackageLease pinnedLease;
        std::shared_ptr<AuthorityAdmissionToken> token;
        bool hasRequest = false;
        bool admitted = false;
        bool healthy = false;
        bool candidate = false;
        bool revoked = false;
        bool retired = false;
        bool failedClosed = false;
        qint64 attemptStartMs = 0;
    };

    struct PendingDrain final
    {
        AuthorityDrainBatch batch;
        QVector<TabLaunchAuthority> affected;
        bool timedOut = false;
        bool terminalFailure = false;
        QString failureError;
    };
    struct AuthorityLess
    {
        bool operator()(const TabLaunchAuthority &left,
                        const TabLaunchAuthority &right) const noexcept;
    };

    [[nodiscard]] qint64 resolveNow(qint64 nowMs) const noexcept;
    [[nodiscard]] static bool validAuthority(const TabLaunchAuthority &tab);
    [[nodiscard]] static std::optional<VersionDescriptor> descriptorFromResult(
        const InstallResult &result,
        const ActivationBinding &binding);
    [[nodiscard]] TabState *findTab(const TabLaunchAuthority &tab) const;
    [[nodiscard]] TabState *findTab(const FullAttemptKey &key) const;
    [[nodiscard]] AppRuntimeResult staleResult() const;
    [[nodiscard]] AppRuntimeResult rejectedResult(const QString &error) const;
    [[nodiscard]] AppRuntimeResult failedClosedResult(
        const QString &error,
        quint32 nativeError = 0) const;
    [[nodiscard]] AppRuntimeAction makeTabAction(
        AppRuntimeActionKind kind,
        const TabLaunchAuthority &tab) const;
    [[nodiscard]] AppRuntimeResult launchForTab(
        const TabLaunchAuthority &tab,
        const QString &route,
        const VersionDescriptor &descriptor,
        PackageRevalidationMode mode,
        bool recovery,
        qint64 nowMs,
        bool reuseActivation = false,
        bool emitRetireActions = true);
    [[nodiscard]] AppRuntimeResult restartTab(TabState &state,
                                              qint64 nowMs,
                                              bool recovery = true);
    [[nodiscard]] bool keyMatches(const TabState &state,
                                  const FullAttemptKey &key) const;
    [[nodiscard]] bool leaseMatches(const TabState &state,
                                    const VerifiedPackageLease &lease) const;
    [[nodiscard]] bool candidateLease(
        const VerifiedPackageLease &lease) const;
    [[nodiscard]] bool currentCandidateTab(const TabState &state) const;
    [[nodiscard]] AppRuntimeResult failClosedTab(
        TabState &state,
        const QString &stableError,
        bool trustedCrash = false);
    void appendDrainTimeoutActions(AppRuntimeResult &result,
                                   const PendingDrain &pending) const;
    [[nodiscard]] AppRuntimeResult promoteCandidate(qint64 nowMs);
    [[nodiscard]] AppRuntimeResult beginCandidateRollback(qint64 nowMs);
    [[nodiscard]] AppRuntimeResult finishDrain(qint64 nowMs);
    [[nodiscard]] AppRuntimeResult timeoutDrain(qint64 nowMs);
    [[nodiscard]] bool allTicketsDrained() const noexcept;
    [[nodiscard]] quint64 issueLeaseEpoch() noexcept;
    void revokeSilently(TabState &state) noexcept;
    void record(SafeEventPhase phase,
                SafeEventCode code,
                const QString &version,
                qint64 nowMs) const;

    QString appId_;
    PackageStore &store_;
    PackageInstaller &installer_;
    WorkerSupervisionPolicy supervisionPolicy_;
    LifecycleClock clock_;
    EventRecorder *recorder_ = nullptr;
    DrainConsumer drainConsumer_;
    qint64 authorityDrainTimeoutMs_ = 10'000;

    std::optional<VersionDescriptor> current_;
    std::optional<VersionDescriptor> candidate_;
    std::optional<VersionDescriptor> lkg_;
    std::map<TabLaunchAuthority,
             std::unique_ptr<TabState>,
             AuthorityLess>
        tabs_;
    std::optional<PendingDrain> pendingDrain_;
    quint64 nextLeaseAuthorityEpoch_ = 1;
    quint64 nextDrainId_ = 1;
    bool candidatePromoted_ = false;
    bool rollbackStarted_ = false;
    bool shuttingDown_ = false;
    bool failedClosed_ = false;
};
