#include "AppRuntimeCoordinator.h"

#include "EventRecorder.h"
#include "PackageStore.h"

#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace {

AppRuntimeResult resultWith(const AppRuntimeResultCode code,
                            QString stableError = {})
{
    AppRuntimeResult result;
    result.code = code;
    result.stableError = std::move(stableError);
    return result;
}

bool sameLeaseIdentity(const VerifiedPackageLease &left,
                       const VerifiedPackageLease &right) noexcept
{
    return left.appId == right.appId && left.version == right.version
        && left.versionDirectory == right.versionDirectory
        && left.packageDirectory == right.packageDirectory
        && left.entryPoint == right.entryPoint
        && left.permissions.network.hosts == right.permissions.network.hosts
        && left.permissions.network.methods
               == right.permissions.network.methods
        && left.permissions.storage == right.permissions.storage
        && left.permissions.clipboardWrite == right.permissions.clipboardWrite
        && left.permissions.clipboardRead == right.permissions.clipboardRead
        && left.permissions.fileOpen == right.permissions.fileOpen
        && left.digestHex == right.digestHex;
}

quint64 nextCounter(const quint64 current) noexcept
{
    return current == std::numeric_limits<quint64>::max() ? 1 : current + 1;
}

qint64 boundedDeadline(const qint64 nowMs, const qint64 timeoutMs) noexcept
{
    if (timeoutMs <= 0) return nowMs;
    if (nowMs > std::numeric_limits<qint64>::max() - timeoutMs) {
        return std::numeric_limits<qint64>::max();
    }
    return nowMs + timeoutMs;
}

} // namespace

AppRuntimeCoordinator::TabState::TabState(
    TabLaunchAuthority value,
    const WorkerSupervisionPolicy policy)
    : authority(std::move(value))
    , supervisor(std::make_unique<WorkerSupervisor>(
          policy, WorkerSupervisor::Callback{}, WorkerSupervisor::Callback{}))
{
}

bool AppRuntimeCoordinator::AuthorityLess::operator()(
    const TabLaunchAuthority &left,
    const TabLaunchAuthority &right) const noexcept
{
    if (left.tabId != right.tabId) return left.tabId < right.tabId;
    return left.runtimeIncarnation < right.runtimeIncarnation;
}

AppRuntimeCoordinator::AppRuntimeCoordinator(
    QString appId,
    PackageStore &store,
    PackageInstaller &installer,
    const WorkerSupervisionPolicy supervisionPolicy,
    LifecycleClock clock,
    EventRecorder *recorder,
    DrainConsumer drainConsumer,
    const qint64 authorityDrainTimeoutMs)
    : appId_(std::move(appId))
    , store_(store)
    , installer_(installer)
    , supervisionPolicy_(supervisionPolicy)
    , clock_(std::move(clock))
    , recorder_(recorder)
    , drainConsumer_(std::move(drainConsumer))
    , authorityDrainTimeoutMs_(authorityDrainTimeoutMs > 0
                                   ? authorityDrainTimeoutMs
                                   : 10'000)
{
    if (!clock_.isValid()) clock_ = LifecycleClock::system();
}

AppRuntimeCoordinator::AppRuntimeCoordinator(
    QString appId,
    PackageStore &store,
    PackageInstaller &installer,
    const WorkerSupervisionPolicy supervisionPolicy,
    LifecycleClock clock,
    DrainConsumer drainConsumer,
    const qint64 authorityDrainTimeoutMs)
    : AppRuntimeCoordinator(std::move(appId),
                            store,
                            installer,
                            supervisionPolicy,
                            std::move(clock),
                            nullptr,
                            std::move(drainConsumer),
                            authorityDrainTimeoutMs)
{
}

qint64 AppRuntimeCoordinator::resolveNow(const qint64 nowMs) const noexcept
{
    if (nowMs >= 0) return nowMs;
    return clock_.isValid() ? clock_.steadyNowMilliseconds() : 0;
}

bool AppRuntimeCoordinator::validAuthority(const TabLaunchAuthority &tab)
{
    return !tab.tabId.isEmpty() && tab.tabId.size() <= 128
        && tab.runtimeIncarnation != 0;
}

AppRuntimeCoordinator::TabState *AppRuntimeCoordinator::findTab(
    const TabLaunchAuthority &tab) const
{
    const auto found = tabs_.find(tab);
    return found == tabs_.cend() ? nullptr : found->second.get();
}

AppRuntimeCoordinator::TabState *AppRuntimeCoordinator::findTab(
    const FullAttemptKey &key) const
{
    TabState *state = findTab(key.tab);
    if (state == nullptr || state->revoked || state->retired
        || state->failedClosed || !state->hasRequest) {
        return nullptr;
    }
    return keyMatches(*state, key) ? state : nullptr;
}

AppRuntimeResult AppRuntimeCoordinator::staleResult() const
{
    return resultWith(AppRuntimeResultCode::IgnoredStale,
                      QStringLiteral("stale_runtime_event"));
}

AppRuntimeResult AppRuntimeCoordinator::rejectedResult(
    const QString &error) const
{
    return resultWith(AppRuntimeResultCode::Rejected, error);
}

AppRuntimeResult AppRuntimeCoordinator::failedClosedResult(
    const QString &error,
    const quint32 nativeError) const
{
    AppRuntimeResult result = resultWith(AppRuntimeResultCode::FailedClosed,
                                         error);
    result.nativeError = nativeError;
    return result;
}

AppRuntimeAction AppRuntimeCoordinator::makeTabAction(
    const AppRuntimeActionKind kind,
    const TabLaunchAuthority &tab) const
{
    return {kind, tab.tabId, tab.runtimeIncarnation, std::nullopt, std::nullopt};
}

std::optional<AppRuntimeCoordinator::VersionDescriptor>
AppRuntimeCoordinator::descriptorFromResult(
    const InstallResult &result,
    const ActivationBinding &binding)
{
    if (!result.succeeded() || result.appId.isEmpty() || result.version.isEmpty()
        || result.path.isEmpty() || result.entryPoint.isEmpty()
        || binding.currentDirectory.isEmpty()
        || binding.versionDigestHex.isEmpty() || binding.generation <= 0) {
        return std::nullopt;
    }
    VersionDescriptor descriptor;
    descriptor.binding = binding;
    descriptor.lease.appId = result.appId;
    descriptor.lease.version = result.version;
    descriptor.lease.versionDirectory = QFileInfo(result.path).fileName();
    descriptor.lease.packageDirectory = result.path;
    descriptor.lease.entryPoint = result.entryPoint;
    descriptor.lease.permissions = result.permissions;
    descriptor.lease.digestHex = binding.versionDigestHex;
    descriptor.lease.activationGenerationAtIssue = binding.generation;
    if (descriptor.lease.versionDirectory.isEmpty()
        || descriptor.lease.versionDirectory != binding.currentDirectory) {
        return std::nullopt;
    }
    return descriptor;
}

quint64 AppRuntimeCoordinator::issueLeaseEpoch() noexcept
{
    const quint64 issued = nextLeaseAuthorityEpoch_;
    nextLeaseAuthorityEpoch_ = nextCounter(nextLeaseAuthorityEpoch_);
    return issued == 0 ? issueLeaseEpoch() : issued;
}

void AppRuntimeCoordinator::revokeSilently(TabState &state) noexcept
{
    if (state.token != nullptr) {
        (void)state.token->beginRevoke();
    }
    state.revoked = true;
    state.admitted = false;
}

AppRuntimeResult AppRuntimeCoordinator::installAndActivate(
    const QString &packagePath,
    const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (shuttingDown_) return rejectedResult(QStringLiteral("shutdown"));
    if (failedClosed_) return failedClosedResult(QStringLiteral("app_failed_closed"));
    if (pendingDrain_.has_value()) {
        return pendingDrain_->timedOut || pendingDrain_->terminalFailure
                   ? failedClosedResult(QStringLiteral("authority_drain_pending"))
                   : rejectedResult(QStringLiteral("authority_drain_pending"));
    }
    if (packagePath.isEmpty()) {
        return rejectedResult(QStringLiteral("invalid_package_path"));
    }

    const ActivationStateResult before = store_.activationState(appId_);
    std::optional<VersionDescriptor> previousCurrent;
    if (before.hasValue() && !before.state.current.isEmpty()) {
        const auto beforeBinding = store_.bindingForState(before.state);
        if (beforeBinding.has_value()) {
            const InstallResult verified = installer_.verifyInstalled(
                appId_, before.state.current);
            previousCurrent = descriptorFromResult(verified, *beforeBinding);
        }
    }

    const InstallResult installed = installer_.install(packagePath);
    if (!installed.succeeded() || !installed.activationBinding.has_value()
        || installed.appId != appId_) {
        return rejectedResult(installed.stableError.isEmpty()
                                  ? QStringLiteral("package_install_rejected")
                                  : installed.stableError);
    }
    const auto descriptor = descriptorFromResult(
        installed, *installed.activationBinding);
    if (!descriptor.has_value()) {
        return rejectedResult(QStringLiteral("package_descriptor_invalid"));
    }
    if (!lkg_.has_value() && previousCurrent.has_value()) lkg_ = previousCurrent;
    current_ = descriptor;
    candidate_ = descriptor;
    candidatePromoted_ = false;
    rollbackStarted_ = false;
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::startOffline(const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (shuttingDown_) return rejectedResult(QStringLiteral("shutdown"));
    if (failedClosed_) return failedClosedResult(QStringLiteral("app_failed_closed"));
    if (pendingDrain_.has_value()) {
        return rejectedResult(QStringLiteral("authority_drain_pending"));
    }
    const ActivationStateResult state = store_.activationState(appId_);
    if (!state.hasValue() || state.state.current.isEmpty()) {
        return rejectedResult(QStringLiteral("state_unavailable"));
    }
    const auto binding = store_.bindingForState(state.state);
    if (!binding.has_value()) {
        return rejectedResult(QStringLiteral("state_invalid"));
    }
    const InstallResult verified = installer_.reverifyInstalledVersion(
        appId_, *binding);
    auto current = descriptorFromResult(verified, *binding);
    if (!current.has_value()
        && !state.state.lastKnownGood.isEmpty()
        && state.state.lastKnownGood != state.state.current) {
        const InstallResult lkgVerified = installer_.verifyInstalled(
            appId_, state.state.lastKnownGood);
        const qsizetype separator = state.state.lastKnownGood.lastIndexOf(
            QLatin1Char('-'));
        if (lkgVerified.succeeded() && separator > 0) {
            const ActivationBinding lkgBinding{
                state.state.lastKnownGood,
                state.state.lastKnownGood.sliced(separator + 1).toLatin1(),
                state.state.generation};
            if (const auto lkgCandidate = descriptorFromResult(
                    lkgVerified, lkgBinding);
                lkgCandidate.has_value()) {
                const PackageStoreResult recovered =
                    store_.recoverLastKnownGood(appId_, *binding);
                if (!recovered.succeeded()
                    || !recovered.activationBinding.has_value()) {
                    return rejectedResult(QStringLiteral("lkg_recovery_failed"));
                }
                current = descriptorFromResult(lkgVerified,
                                               *recovered.activationBinding);
            }
        }
    }
    if (!current.has_value()) {
        return rejectedResult(QStringLiteral("installed_content_invalid"));
    }
    current_ = current;
    candidate_ = current;
    candidatePromoted_ = state.state.lastKnownGood == state.state.current;

    if (!state.state.lastKnownGood.isEmpty()
        && state.state.lastKnownGood != state.state.current) {
        const InstallResult lkgVerified = installer_.verifyInstalled(
            appId_, state.state.lastKnownGood);
        const QString directory = state.state.lastKnownGood;
        const qsizetype separator = directory.lastIndexOf(QLatin1Char('-'));
        if (!lkgVerified.succeeded() || separator <= 0) {
            return rejectedResult(QStringLiteral("lkg_unavailable"));
        }
        const ActivationBinding lkgBinding{
            directory,
            directory.sliced(separator + 1).toLatin1(),
            state.state.generation};
        lkg_ = descriptorFromResult(lkgVerified, lkgBinding);
        if (!lkg_.has_value()) {
            return rejectedResult(QStringLiteral("lkg_unavailable"));
        }
    } else if (candidatePromoted_) {
        lkg_ = current;
    }
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::requestTabLaunch(
    const TabLaunchAuthority &tab,
    const QString &route,
    const TabLaunchIntent intent,
    const qint64 nowMs)
{
    if (shuttingDown_) return rejectedResult(QStringLiteral("shutdown"));
    if (failedClosed_) return failedClosedResult(QStringLiteral("app_failed_closed"));
    if (pendingDrain_.has_value()) {
        return pendingDrain_->timedOut || pendingDrain_->terminalFailure
                   ? failedClosedResult(QStringLiteral("authority_drain_pending"))
                   : rejectedResult(QStringLiteral("authority_drain_pending"));
    }
    if (!validAuthority(tab) || route.isEmpty() || route.size() > 256
        || !route.startsWith(QLatin1Char('/')) || route.contains(QLatin1Char('?'))
        || route.contains(QLatin1Char('#')) || route.contains(QLatin1Char('\\'))) {
        return rejectedResult(QStringLiteral("invalid_launch_request"));
    }
    const qint64 now = resolveNow(nowMs);
    const auto exact = tabs_.find(tab);
    if (exact != tabs_.end()
        && (exact->second->retired || exact->second->failedClosed)) {
        return exact->second->failedClosed
                   ? failedClosedResult(QStringLiteral("authority_failed_closed"))
                   : rejectedResult(QStringLiteral("authority_retired"));
    }

    std::optional<TabLaunchAuthority> previousIncarnation;
    for (const auto &entry : tabs_) {
        if (entry.first.tabId == tab.tabId
            && entry.first.runtimeIncarnation != tab.runtimeIncarnation
            && !entry.second->retired && !entry.second->failedClosed) {
            previousIncarnation = entry.first;
            break;
        }
    }
    AppRuntimeResult replacement;
    if (previousIncarnation.has_value()) {
        replacement = closeTab(*previousIncarnation, now);
    }

    TabState *existing = findTab(tab);
    const VersionDescriptor *descriptor = nullptr;
    std::optional<VersionDescriptor> pinned;
    PackageRevalidationMode mode = PackageRevalidationMode::PinnedLease;
    if (intent == TabLaunchIntent::RestartPinned) {
        if (existing == nullptr || !existing->hasRequest
            || existing->pinnedLease.versionDirectory.isEmpty()) {
            return rejectedResult(QStringLiteral("pinned_lease_unavailable"));
        }
        pinned.emplace();
        pinned->lease = existing->pinnedLease;
        pinned->binding = {pinned->lease.versionDirectory,
                           pinned->lease.digestHex,
                           pinned->lease.activationGenerationAtIssue};
        descriptor = &*pinned;
        mode = PackageRevalidationMode::PinnedLease;
    } else if (current_.has_value()) {
        descriptor = &*current_;
        mode = PackageRevalidationMode::PinnedLease;
    } else {
        return rejectedResult(QStringLiteral("current_unavailable"));
    }
    if (intent == TabLaunchIntent::ActivateCurrent && existing != nullptr
        && existing->hasRequest && existing->admitted && !existing->revoked
        && !existing->retired && !existing->failedClosed
        && sameLeaseIdentity(existing->pinnedLease, descriptor->lease)
        && existing->pinnedLease.activationGenerationAtIssue
               == descriptor->lease.activationGenerationAtIssue) {
        // A healthy worker already owns this tab/runtime authority.  A user
        // route change updates the authority's restart route without creating
        // a second process; the GUI dispatches the corresponding RouteLoad.
        existing->request.route = route;
        return replacement;
    }
    AppRuntimeResult launched = launchForTab(tab, route, *descriptor, mode,
                                             false, now);
    replacement.actions += launched.actions;
    if (launched.code != AppRuntimeResultCode::Applied) {
        replacement.code = launched.code;
        replacement.stableError = launched.stableError;
        replacement.nativeError = launched.nativeError;
    }
    return replacement;
}

AppRuntimeResult AppRuntimeCoordinator::launchForTab(
    const TabLaunchAuthority &tab,
    const QString &route,
    const VersionDescriptor &descriptor,
    const PackageRevalidationMode mode,
    const bool recovery,
    const qint64 nowMs,
    const bool reuseActivation,
    const bool emitRetireActions)
{
    if (shuttingDown_) return rejectedResult(QStringLiteral("shutdown"));
    if (failedClosed_) return failedClosedResult(QStringLiteral("app_failed_closed"));
    if (pendingDrain_.has_value()) {
        return rejectedResult(QStringLiteral("authority_drain_pending"));
    }
    auto found = tabs_.find(tab);
    AppRuntimeResult replacement;
    if (found == tabs_.end()) {
        found = tabs_.emplace(
                     tab,
                     std::make_unique<TabState>(tab, supervisionPolicy_))
                    .first;
    } else {
        if (emitRetireActions && found->second->hasRequest
            && !found->second->retired && !found->second->revoked) {
            replacement.actions.push_back(
                makeTabAction(AppRuntimeActionKind::Revoke, tab));
            replacement.actions.push_back(
                makeTabAction(AppRuntimeActionKind::Stop, tab));
        }
        revokeSilently(*found->second);
    }
    TabState &state = *found->second;
    if (!reuseActivation || state.supervisor == nullptr) {
        state.supervisor = std::make_unique<WorkerSupervisor>(
            supervisionPolicy_, WorkerSupervisor::Callback{},
            WorkerSupervisor::Callback{});
    }
    const WorkerActivationId activation = reuseActivation
        ? state.supervisor->activeActivation()
        : state.supervisor->beginActivation(nowMs);
    const std::optional<WorkerAttemptId> attempt =
        state.supervisor->beginAttempt(activation, nowMs);
    if (!attempt.has_value()) {
        state.failedClosed = true;
        replacement.code = AppRuntimeResultCode::FailedClosed;
        replacement.stableError = QStringLiteral("attempt_unavailable");
        replacement.actions.push_back(
            makeTabAction(AppRuntimeActionKind::FailedClosed, tab));
        return replacement;
    }

    VerifiedPackageLease lease = descriptor.lease;
    lease.leaseAuthorityEpoch = issueLeaseEpoch();
    state.token = std::make_shared<AuthorityAdmissionToken>();
    state.request = WorkerLaunchRequest{
        tab.tabId,
        tab.runtimeIncarnation,
        route,
        lease,
        state.token,
        {activation, *attempt},
        mode,
        recovery};
    state.pinnedLease = lease;
    state.hasRequest = true;
    state.admitted = false;
    state.healthy = false;
    state.candidate = candidateLease(lease);
    state.revoked = false;
    state.retired = false;
    state.failedClosed = false;
    state.attemptStartMs = nowMs;

    AppRuntimeAction action{AppRuntimeActionKind::Launch,
                            tab.tabId,
                            tab.runtimeIncarnation,
                            state.request,
                            std::nullopt};
    AppRuntimeResult result = std::move(replacement);
    result.actions.push_back(std::move(action));
    return result;
}

bool AppRuntimeCoordinator::keyMatches(const TabState &state,
                                       const FullAttemptKey &key) const
{
    return state.request.tabId == key.tab.tabId
        && state.request.runtimeIncarnation == key.tab.runtimeIncarnation
        && state.request.attempt == key.attempt
        && state.request.lease.leaseAuthorityEpoch == key.leaseAuthorityEpoch;
}

bool AppRuntimeCoordinator::leaseMatches(
    const TabState &state,
    const VerifiedPackageLease &lease) const
{
    return state.hasRequest && state.request.lease == lease;
}

bool AppRuntimeCoordinator::candidateLease(
    const VerifiedPackageLease &lease) const
{
    return candidate_.has_value()
        && sameLeaseIdentity(candidate_->lease, lease)
        && !candidatePromoted_;
}

bool AppRuntimeCoordinator::currentCandidateTab(const TabState &state) const
{
    return state.candidate && !candidatePromoted_ && candidate_.has_value()
        && sameLeaseIdentity(state.pinnedLease, candidate_->lease);
}

AppRuntimeResult AppRuntimeCoordinator::failClosedTab(
    TabState &state,
    const QString &stableError,
    const bool trustedCrash)
{
    if (state.failedClosed) return staleResult();
    AppRuntimeResult result = failedClosedResult(
        stableError.isEmpty() ? QStringLiteral("worker_failed_closed")
                              : stableError);
    if (state.token != nullptr) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke,
                                               state.authority));
    }
    result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                           state.authority));
    result.actions.push_back(makeTabAction(AppRuntimeActionKind::IsolateSession,
                                           state.authority));
    result.actions.push_back(makeTabAction(
        trustedCrash ? AppRuntimeActionKind::TrustedCrash
                     : AppRuntimeActionKind::FailedClosed,
        state.authority));
    revokeSilently(state);
    state.failedClosed = true;
    return result;
}

void AppRuntimeCoordinator::appendDrainTimeoutActions(
    AppRuntimeResult &result,
    const PendingDrain &pending) const
{
    for (const TabLaunchAuthority &tab : pending.affected) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::FailedClosed,
                                                tab));
    }
    for (const TabLaunchAuthority &tab : pending.affected) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                                tab));
    }
    for (const TabLaunchAuthority &tab : pending.affected) {
        result.actions.push_back(makeTabAction(
            AppRuntimeActionKind::IsolateSession, tab));
    }
}

AppRuntimeResult AppRuntimeCoordinator::admitAuthenticatedWorker(
    const FullAttemptKey &key,
    const VerifiedPackageLease &lease)
{
    TabState *state = findTab(key.tab);
    if (state == nullptr) return staleResult();
    if (state->revoked || state->retired || state->failedClosed
        || !state->hasRequest || !keyMatches(*state, key)) {
        return state->revoked || state->retired || state->failedClosed
                   ? rejectedResult(QStringLiteral("authority_revoked"))
                   : staleResult();
    }
    return admitAuthenticatedWorker(
        key, lease, std::max(resolveNow(-1), state->attemptStartMs));
}

AppRuntimeResult AppRuntimeCoordinator::admitAuthenticatedWorker(
    const FullAttemptKey &key,
    const VerifiedPackageLease &lease,
    const qint64 receivedMonotonicMs)
{
    TabState *state = findTab(key.tab);
    if (state == nullptr) return staleResult();
    if (state->revoked || state->retired || state->failedClosed
        || !state->hasRequest) {
        return rejectedResult(QStringLiteral("authority_revoked"));
    }
    if (!keyMatches(*state, key)) return staleResult();
    if (!leaseMatches(*state, lease)) {
        return rejectedResult(QStringLiteral("lease_mismatch"));
    }
    if (state->token == nullptr) {
        return rejectedResult(QStringLiteral("authority_revoked"));
    }
    auto use = state->token->tryAcquireUse();
    if (!use.has_value()
        || !use->publishIfStillAdmitted([] { return true; })) {
        return rejectedResult(QStringLiteral("authority_revoked"));
    }
    if (state->admitted) return {};
    if (receivedMonotonicMs < state->attemptStartMs) {
        return rejectedResult(QStringLiteral("admission_timestamp_invalid"));
    }
    if (!state->supervisor->authenticatedHandshake(
            key.attempt, receivedMonotonicMs)) {
        return rejectedResult(QStringLiteral("admission_rejected"));
    }
    state->admitted = true;
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::admitAuthenticatedWorker(
    const WorkerLaunchRequest &request,
    const qint64 receivedMonotonicMs)
{
    const FullAttemptKey key{{request.tabId, request.runtimeIncarnation},
                             request.attempt,
                             request.lease.leaseAuthorityEpoch};
    TabState *state = findTab(key);
    if (state == nullptr) return staleResult();
    if (!(state->request == request)) {
        return rejectedResult(QStringLiteral("launch_authority_mismatch"));
    }
    TabState *current = findTab(key.tab);
    const qint64 effectiveNow = receivedMonotonicMs >= 0
        ? receivedMonotonicMs
        : (current == nullptr ? resolveNow(-1)
                              : std::max(resolveNow(-1),
                                         current->attemptStartMs));
    return admitAuthenticatedWorker(key, request.lease, effectiveNow);
}

AppRuntimeResult AppRuntimeCoordinator::heartbeat(
    const FullAttemptKey &key,
    const qint64 receivedMonotonicMs)
{
    TabState *state = findTab(key);
    if (state == nullptr) return staleResult();
    if (!state->admitted) {
        return rejectedResult(QStringLiteral("worker_not_admitted"));
    }
    const WorkerSupervisionAction healthAction = state->supervisor->checkHealth(
        key.attempt, receivedMonotonicMs);
    if (healthAction == WorkerSupervisionAction::IgnoredStaleAttempt
        || healthAction == WorkerSupervisionAction::IgnoredDuplicateFailure) {
        return staleResult();
    }
    if (healthAction == WorkerSupervisionAction::Restart) {
        return restartTab(*state, receivedMonotonicMs);
    }
    if (healthAction == WorkerSupervisionAction::CrashLoopRollback
        || healthAction == WorkerSupervisionAction::StartupRollback) {
        if (currentCandidateTab(*state)) {
            return beginCandidateRollback(receivedMonotonicMs);
        }
        return failClosedTab(*state, QStringLiteral("worker_crash_loop"), true);
    }
    state->supervisor->heartbeat(key.attempt, receivedMonotonicMs);
    if (!state->healthy
        && state->supervisor->isHealthy(key.attempt, receivedMonotonicMs)) {
        state->healthy = true;
        if (currentCandidateTab(*state)) {
            return promoteCandidate(receivedMonotonicMs);
        }
    }
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::promoteCandidate(const qint64 nowMs)
{
    if (candidatePromoted_ || !candidate_.has_value()) return {};
    const PackageStoreResult marked = store_.markCurrentLastKnownGood(
        appId_, candidate_->binding);
    if (!marked.succeeded() || !marked.activationBinding.has_value()) {
        return failedClosedResult(QStringLiteral("lkg_commit_failed"));
    }
    candidatePromoted_ = true;
    candidate_->binding = *marked.activationBinding;
    candidate_->lease.activationGenerationAtIssue =
        marked.activationBinding->generation;
    current_ = candidate_;
    lkg_ = candidate_;
    record(SafeEventPhase::Health, SafeEventCode::Healthy,
           candidate_->lease.version, nowMs);
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::checkHealth(const qint64 nowMs)
{
    if (shuttingDown_) return rejectedResult(QStringLiteral("shutdown"));
    AppRuntimeResult result;
    for (auto &entry : tabs_) {
        TabState &state = *entry.second;
        if (state.revoked || state.retired || state.failedClosed
            || !state.hasRequest || !state.admitted) {
            continue;
        }
        const std::optional<WorkerAttemptId> active =
            state.supervisor->activeAttempt();
        if (!active.has_value()) continue;
        const FullAttemptKey key{state.authority,
                                 {state.supervisor->activeActivation(), *active},
                                 state.request.lease.leaseAuthorityEpoch};
        const WorkerSupervisionAction action =
            state.supervisor->checkHealth(key.attempt, nowMs);
        if (action == WorkerSupervisionAction::Restart) {
            const AppRuntimeResult restarted = restartTab(state, nowMs);
            result.actions += restarted.actions;
            if (restarted.code != AppRuntimeResultCode::Applied) {
                result.code = restarted.code;
                result.stableError = restarted.stableError;
            }
        } else if (action == WorkerSupervisionAction::CrashLoopRollback
                   || action == WorkerSupervisionAction::StartupRollback) {
            const AppRuntimeResult rollback = currentCandidateTab(state)
                ? beginCandidateRollback(nowMs)
                : failClosedTab(state, QStringLiteral("worker_crash_loop"),
                                true);
            result.actions += rollback.actions;
            if (rollback.code != AppRuntimeResultCode::Applied) {
                result.code = rollback.code;
                result.stableError = rollback.stableError;
            }
            break;
        }
    }
    return result;
}

AppRuntimeResult AppRuntimeCoordinator::restartTab(TabState &state,
                                                   const qint64 nowMs,
                                                   const bool recovery)
{
    if (!state.hasRequest || state.pinnedLease.versionDirectory.isEmpty()) {
        state.failedClosed = true;
        return failedClosedResult(QStringLiteral("pinned_lease_unavailable"));
    }
    VersionDescriptor descriptor;
    descriptor.lease = state.pinnedLease;
    descriptor.binding = {descriptor.lease.versionDirectory,
                          descriptor.lease.digestHex,
                          descriptor.lease.activationGenerationAtIssue};
    return launchForTab(state.authority, state.request.route, descriptor,
                        PackageRevalidationMode::PinnedLease, recovery, nowMs,
                        state.supervisor != nullptr
                            && state.supervisor->state()
                                   == WorkerSupervisorState::Stopped);
}

AppRuntimeResult AppRuntimeCoordinator::workerExited(
    const FullAttemptKey &key,
    const WorkerExitReason reason,
    const qint64 nowMs)
{
    TabState *state = findTab(key);
    if (state == nullptr) return staleResult();
    const WorkerSupervisionAction action = state->supervisor->workerExited(
        key.attempt, reason, nowMs);
    if (action == WorkerSupervisionAction::IgnoredStaleAttempt
        || action == WorkerSupervisionAction::IgnoredDuplicateFailure) {
        return staleResult();
    }
    if (reason == WorkerExitReason::Clean) {
        revokeSilently(*state);
        state->retired = true;
        return {};
    }
    if (action == WorkerSupervisionAction::Restart) {
        return restartTab(*state, nowMs);
    }
    if (action == WorkerSupervisionAction::CrashLoopRollback
        || action == WorkerSupervisionAction::StartupRollback) {
        if (currentCandidateTab(*state)) {
            return beginCandidateRollback(nowMs);
        }
        return failClosedTab(*state, QStringLiteral("worker_crash_loop"), true);
    }
    return {};
}

AppRuntimeResult AppRuntimeCoordinator::workerAdmissionFailed(
    const FullAttemptKey &key,
    const QString &stableError,
    const qint64 nowMs)
{
    TabState *state = findTab(key);
    if (state == nullptr) return staleResult();
    if (currentCandidateTab(*state)) {
        return beginCandidateRollback(resolveNow(nowMs));
    }
    return failClosedTab(*state,
                         stableError.isEmpty()
                             ? QStringLiteral("worker_admission_failed")
                             : stableError);
}

AppRuntimeResult AppRuntimeCoordinator::workerCleanupFailed(
    const FullAttemptKey &key,
    const QString &stableError,
    const qint64 nowMs)
{
    return workerCleanupFailed(key, stableError, 0U, nowMs);
}

AppRuntimeResult AppRuntimeCoordinator::workerCleanupFailed(
    const FullAttemptKey &key,
    const QString &stableError,
    const quint32 nativeError,
    const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    TabState *state = findTab(key);
    if (state == nullptr) return staleResult();
    revokeSilently(*state);
    state->failedClosed = true;
    AppRuntimeResult result = failedClosedResult(
        stableError.isEmpty() ? QStringLiteral("worker_cleanup_failed")
                              : stableError,
        nativeError);
    result.actions.push_back(makeTabAction(AppRuntimeActionKind::FailedClosed,
                                            state->authority));
    return result;
}

AppRuntimeResult AppRuntimeCoordinator::beginCandidateRollback(
    const qint64 nowMs)
{
    if (rollbackStarted_ || pendingDrain_.has_value()) {
        return staleResult();
    }
    if (!candidate_.has_value()) {
        return failedClosedResult(QStringLiteral("candidate_unavailable"));
    }

    // Linearize authority revocation before any store transition or drain
    // scheduling. The package rollback is allowed to fail, but no affected
    // lease is ever reopened after this point.
    PendingDrain pending;
    pending.batch.id = nextDrainId_;
    nextDrainId_ = nextCounter(nextDrainId_);
    pending.batch.monotonicDeadlineMs = boundedDeadline(
        nowMs, authorityDrainTimeoutMs_);
    for (auto &entry : tabs_) {
        TabState &state = *entry.second;
        if (!state.hasRequest || state.revoked || state.retired
            || state.failedClosed || !state.candidate
            || !sameLeaseIdentity(state.pinnedLease, candidate_->lease)) {
            continue;
        }
        pending.affected.push_back(state.authority);
        if (state.token != nullptr) {
            pending.batch.tickets.push_back(state.token->beginRevoke());
        }
        state.revoked = true;
        state.admitted = false;
    }

    const PackageStoreResult rolled = store_.rollback(appId_, candidate_->binding);
    if (!rolled.succeeded() || !rolled.activationBinding.has_value()) {
        for (const TabLaunchAuthority &tab : pending.affected) {
            tabs_.at(tab)->failedClosed = true;
        }
        pending.terminalFailure = true;
        pending.failureError = QStringLiteral("rollback_unavailable");
        pendingDrain_ = pending;
        rollbackStarted_ = true;
        failedClosed_ = true;
        AppRuntimeResult result = failedClosedResult(
            pending.failureError);
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke,
                                                    tab));
        }
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                                    tab));
        }
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(
                AppRuntimeActionKind::IsolateSession, tab));
        }
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::FailedClosed,
                                                {}));
        result.actions.push_back({AppRuntimeActionKind::AwaitAuthorityDrain,
                                  {}, 0, std::nullopt, pending.batch});
        if (drainConsumer_) drainConsumer_(pending.batch);
        return result;
    }
    const InstallResult recovered = installer_.verifyInstalled(
        appId_, QFileInfo(rolled.path).fileName());
    const auto recoveredDescriptor = descriptorFromResult(
        recovered, *rolled.activationBinding);
    if (!recoveredDescriptor.has_value()) {
        for (const TabLaunchAuthority &tab : pending.affected) {
            tabs_.at(tab)->failedClosed = true;
        }
        pending.terminalFailure = true;
        pending.failureError = QStringLiteral("lkg_unavailable");
        pendingDrain_ = pending;
        rollbackStarted_ = true;
        failedClosed_ = true;
        AppRuntimeResult result = failedClosedResult(
            pending.failureError);
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke,
                                                    tab));
        }
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                                    tab));
        }
        for (const TabLaunchAuthority &tab : pending.affected) {
            result.actions.push_back(makeTabAction(
                AppRuntimeActionKind::IsolateSession, tab));
        }
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::FailedClosed,
                                                {}));
        result.actions.push_back({AppRuntimeActionKind::AwaitAuthorityDrain,
                                  {}, 0, std::nullopt, pending.batch});
        if (drainConsumer_) drainConsumer_(pending.batch);
        return result;
    }
    current_ = recoveredDescriptor;
    lkg_ = recoveredDescriptor;
    candidatePromoted_ = true;
    rollbackStarted_ = true;

    AppRuntimeResult result;
    for (const TabLaunchAuthority &tab : pending.affected) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke,
                                                tab));
    }
    for (const TabLaunchAuthority &tab : pending.affected) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                                tab));
    }
    result.actions.push_back(
        {AppRuntimeActionKind::AwaitAuthorityDrain,
         {},
         0,
         std::nullopt,
         pending.batch});
    pendingDrain_ = std::move(pending);
    if (drainConsumer_) drainConsumer_(pendingDrain_->batch);
    return result;
}

bool AppRuntimeCoordinator::allTicketsDrained() const noexcept
{
    if (!pendingDrain_.has_value()) return true;
    return std::ranges::all_of(
        pendingDrain_->batch.tickets,
        [](const AuthorityAdmissionToken::RevocationTicket &ticket) {
            return ticket.isDrained();
        });
}

AppRuntimeResult AppRuntimeCoordinator::timeoutDrain(const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (!pendingDrain_.has_value()) return staleResult();
    if (pendingDrain_->timedOut) return staleResult();
    pendingDrain_->timedOut = true;
    // A deadline breach is terminal for the app runtime.  Even if every
    // outstanding use eventually drains, reopening the authority would make
    // a late publication indistinguishable from a successful rollback.
    failedClosed_ = true;
    AppRuntimeResult result = failedClosedResult(
        QStringLiteral("authority_drain_timeout"));
    appendDrainTimeoutActions(result, *pendingDrain_);
    return result;
}

AppRuntimeResult AppRuntimeCoordinator::finishDrain(const qint64 nowMs)
{
    if (!pendingDrain_.has_value()) return staleResult();
    if (!allTicketsDrained()) {
        return rejectedResult(QStringLiteral("authority_drain_pending"));
    }
    const PendingDrain completed = *pendingDrain_;
    const QVector<TabLaunchAuthority> affected = completed.affected;
    pendingDrain_.reset();
    if (completed.timedOut || completed.terminalFailure || shuttingDown_
        || failedClosed_) {
        for (const TabLaunchAuthority &tab : affected) tabs_.erase(tab);
        if (completed.terminalFailure) {
            return failedClosedResult(
                completed.failureError.isEmpty()
                    ? QStringLiteral("authority_drain_failed")
                    : completed.failureError);
        }
        if (completed.timedOut) {
            return failedClosedResult(QStringLiteral("authority_drain_timeout"));
        }
        return {};
    }
    AppRuntimeResult result;
    for (const TabLaunchAuthority &tab : affected) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::RecoverFromLkg,
                                                tab));
        TabState *state = findTab(tab);
        if (state == nullptr || !lkg_.has_value()) continue;
        const AppRuntimeResult launch = launchForTab(
            tab, state->request.route, *lkg_,
            PackageRevalidationMode::PinnedLease, true, nowMs, false, false);
        result.actions += launch.actions;
    }
    rollbackStarted_ = false;
    if (lkg_.has_value()) {
        record(SafeEventPhase::Rollback, SafeEventCode::Recovered,
               lkg_->lease.version, nowMs);
    }
    return result;
}

AppRuntimeResult AppRuntimeCoordinator::authorityDrainTimedOut(
    const quint64 batchId,
    const qint64 nowMs)
{
    if (!pendingDrain_.has_value() || pendingDrain_->batch.id != batchId) {
        return staleResult();
    }
    if (nowMs <= pendingDrain_->batch.monotonicDeadlineMs) {
        return rejectedResult(QStringLiteral("authority_drain_deadline_pending"));
    }
    return timeoutDrain(nowMs);
}

AppRuntimeResult AppRuntimeCoordinator::authorityDrainCompleted(
    const quint64 batchId,
    const qint64 nowMs)
{
    if (!pendingDrain_.has_value() || pendingDrain_->batch.id != batchId) {
        return staleResult();
    }
    AppRuntimeResult timeoutResult;
    if (nowMs > pendingDrain_->batch.monotonicDeadlineMs) {
        if (!pendingDrain_->timedOut) {
            timeoutResult = timeoutDrain(nowMs);
        } else {
            timeoutResult = failedClosedResult(
                QStringLiteral("authority_drain_timeout"));
            appendDrainTimeoutActions(timeoutResult, *pendingDrain_);
        }
        if (!allTicketsDrained()) return timeoutResult;
        (void)finishDrain(nowMs);
        return timeoutResult;
    }
    return finishDrain(nowMs);
}

AppRuntimeResult AppRuntimeCoordinator::closeTab(
    const TabLaunchAuthority &tab,
    const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (!validAuthority(tab)) return rejectedResult(QStringLiteral("invalid_tab"));
    const auto found = tabs_.find(tab);
    if (found == tabs_.end()) return staleResult();
    if (found->second->retired) return staleResult();
    AppRuntimeResult result;
    if (found->second->token != nullptr && !found->second->revoked) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke, tab));
    }
    if (!found->second->revoked) {
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop, tab));
    }
    revokeSilently(*found->second);
    found->second->retired = true;
    return result;
}

AppRuntimeResult AppRuntimeCoordinator::beginShutdown(const qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (shuttingDown_) return staleResult();
    shuttingDown_ = true;
    AppRuntimeResult result;
    QVector<TabLaunchAuthority> authorities;
    authorities.reserve(static_cast<qsizetype>(tabs_.size()));
    for (const auto &entry : tabs_) authorities.push_back(entry.first);
    for (const TabLaunchAuthority &tab : authorities) {
        const TabState *state = findTab(tab);
        if (state == nullptr || state->retired) continue;
        if (!state->revoked && state->token != nullptr) {
            result.actions.push_back(makeTabAction(AppRuntimeActionKind::Revoke,
                                                    tab));
        }
    }
    for (const TabLaunchAuthority &tab : authorities) {
        const TabState *state = findTab(tab);
        if (state == nullptr || state->retired || state->revoked) continue;
        result.actions.push_back(makeTabAction(AppRuntimeActionKind::Stop,
                                                tab));
    }
    for (const TabLaunchAuthority &tab : authorities) {
        if (TabState *state = findTab(tab); state != nullptr
            && !state->retired) {
            revokeSilently(*state);
            state->retired = true;
        }
    }
    return result;
}

void AppRuntimeCoordinator::record(const SafeEventPhase phase,
                                   const SafeEventCode code,
                                   const QString &version,
                                   const qint64 nowMs) const
{
    if (recorder_ == nullptr) return;
    const SafeEventResult event = SafeEvent::create(
        std::max<qint64>(0, nowMs), appId_, version, phase, code, 0,
        QStringLiteral("/"), {});
    if (event.hasValue()) (void)recorder_->record(event.value());
}
