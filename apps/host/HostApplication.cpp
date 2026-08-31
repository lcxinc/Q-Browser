#include "HostApplication.h"

#include "EventRecorder.h"
#include "AppTabRuntimeController.h"
#include "BrowserChrome.h"
#include "HostCapabilityRuntime.h"
#include "FileDialogCoordinator.h"
#include "HostGestureRouter.h"
#include "HostOwnedFileAuthority.h"
#include "HostWorkerSessionController.h"
#include "InstalledPackageWorkerLauncher.h"
#include "InstalledPackageWorkerLauncherTestHooks.h"
#include "MainWindow.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "RuntimePackageAuthority.h"
#include "AppRuntimeCoordinator.h"
#include "SandboxTrustBoundary.h"
#include "UpdateLifecycleCoordinator.h"
#include "TabCapabilityAuthority.h"
#include "WorkerRetirementManager.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSessionProfile.h"

#include <QPointer>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QRegularExpression>
#include <QFile>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

struct HostApplication::WorkerAttachContext final
{
    std::unique_ptr<IpcSession> session;
    std::unique_ptr<WorkerSurface> surface;
    std::shared_ptr<void> processLifetime;
    WorkerLaunchRequest launchRequest;
    quint32 processId = 0;
    std::function<void()> stopProcess;
    std::shared_ptr<HostCapabilityRuntime> capability;
};

#ifdef Q_BROWSER_HOST_TESTING
struct HostApplication::PackageStartupTestingState final
{
    struct HeldReply final
    {
        PackageOperationKind kind = PackageOperationKind::StartupOffline;
        AppRuntimeResult result;
        std::shared_ptr<RuntimePackageAuthority> authority;
    };

    bool holdNextReply = false;
    QHash<quint64, HeldReply> heldReplies;
    QString restoredRoutePackageId;
};
#endif

namespace
{
constexpr qsizetype MaximumPendingLifecycleOperations = 512;

void recordHostDiagnosticPhase(const QByteArray &phase)
{
    if (!qEnvironmentVariableIsSet("Q_BROWSER_HOST_DIAGNOSTIC_PHASES")) return;
    QFile standardError;
    if (!standardError.open(stderr, QIODevice::WriteOnly,
                            QFileDevice::DontCloseHandle)) {
        return;
    }
    (void)standardError.write(QByteArrayLiteral("qbrowser-host phase: ")
                              + phase + '\n');
    (void)standardError.flush();
}

QString pilotRouteTemplate(const QString &route)
{
    static const QHash<QString, QString> literals{
        {QStringLiteral("/login"), QStringLiteral("/login")},
        {QStringLiteral("/dashboard"), QStringLiteral("/dashboard")},
        {QStringLiteral("/orders"), QStringLiteral("/orders")},
        {QStringLiteral("/customers"), QStringLiteral("/customers")},
        {QStringLiteral("/files"), QStringLiteral("/files")},
        {QStringLiteral("/settings"), QStringLiteral("/settings")},
    };
    const auto literal = literals.constFind(route);
    if (literal != literals.cend()) return *literal;
    static const QRegularExpression order(
        QStringLiteral(R"(^/orders/[^/]+$)"));
    static const QRegularExpression orderEdit(
        QStringLiteral(R"(^/orders/[^/]+/edit$)"));
    static const QRegularExpression customer(
        QStringLiteral(R"(^/customers/[^/]+$)"));
    if (orderEdit.match(route).hasMatch()) return QStringLiteral("/orders/:id/edit");
    if (order.match(route).hasMatch()) return QStringLiteral("/orders/:id");
    if (customer.match(route).hasMatch()) return QStringLiteral("/customers/:id");
    return {};
}

qint64 hostMonotonicNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString appHeartbeatKey(const WorkerLaunchRequest &request)
{
    return QStringLiteral("%1/%2/%3/%4/%5")
        .arg(request.tabId)
        .arg(request.runtimeIncarnation)
        .arg(request.attempt.activation.value)
        .arg(request.attempt.attempt.value)
        .arg(request.lease.leaseAuthorityEpoch);
}

class HostLifecycleShutdownHandoff final
{
public:
    void complete(
        std::optional<UpdateLifecycleShutdownCleanup> cleanupOwner) noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (state_ != State::Pending) return;
            cleanupOwner_ = std::move(cleanupOwner);
            state_ = State::Completed;
        }
        changed_.notify_all();
    }

    void fail() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (state_ != State::Pending) return;
            state_ = State::Failed;
        }
        changed_.notify_all();
    }

    [[nodiscard]] WorkerRetirementAttemptResult attempt() noexcept
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(
                lock, std::chrono::seconds(10),
                [this] { return state_ != State::Pending; })) {
            return {false,
                    QStringLiteral("host.launch.retirement_unavailable")};
        }
        if (state_ == State::Failed) {
            return {false,
                    QStringLiteral("host.launch.retirement_unavailable")};
        }
        if (!cleanupOwner_.has_value()) return {true, {}};

        const ImmutablePackageGuardCloseResult closed = cleanupOwner_->close();
        if (closed.value.has_value()) {
            cleanupOwner_.reset();
            nativeError_ = 0;
            return {true, {}};
        }
        nativeError_ = closed.nativeError;
        return {
            false,
            closed.errorCode.isEmpty()
                ? QStringLiteral("package.immutable_restore_failed")
                : closed.errorCode};
    }

    [[nodiscard]] quint32 nativeError() const noexcept
    {
        std::lock_guard lock(mutex_);
        return nativeError_;
    }

private:
    enum class State
    {
        Pending,
        Completed,
        Failed,
    };

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Pending;
    std::optional<UpdateLifecycleShutdownCleanup> cleanupOwner_;
    quint32 nativeError_ = 0;
};

class HostLifecycleRuntime final : public QObject
{
public:
    using FailurePublisher = std::function<void(const QString &, quint32)>;

    explicit HostLifecycleRuntime(FailurePublisher publishFailure)
        : publishFailure_(std::move(publishFailure))
        , shutdownHandoff_(
              std::make_shared<HostLifecycleShutdownHandoff>())
    {
    }

    [[nodiscard]] bool initialize(
        QString appId,
        QString packageStoreRoot,
        QByteArray trustedPublicKeyPem,
        InstallPolicy installPolicy,
        const WorkerSupervisionPolicy supervisionPolicy,
        QString telemetryDirectory)
    {
        if (QThread::currentThread() != thread() || authority_ != nullptr
            || recorder_ != nullptr || coordinator_ != nullptr
            || appCoordinator_ != nullptr || appId.isEmpty()
            || packageStoreRoot.isEmpty() || trustedPublicKeyPem.isEmpty()
            || telemetryDirectory.isEmpty()) {
            return false;
        }
        try {
            auto authority = std::make_shared<RuntimePackageAuthority>(
                std::move(packageStoreRoot),
                std::move(trustedPublicKeyPem), std::move(installPolicy));
            EventRecorderConfig recorderConfig;
            recorderConfig.directoryPath = std::move(telemetryDirectory);
            auto recorder = std::make_unique<EventRecorder>(
                std::move(recorderConfig));
            auto coordinator = std::make_unique<UpdateLifecycleCoordinator>(
                appId, authority->store(), authority->installer(),
                supervisionPolicy,
                [](const UpdateLaunchRequest &) { return false; },
                LifecycleClock::system(), recorder.get(),
                QStringLiteral("package-telemetry"), 1);
            auto appCoordinator = std::make_unique<AppRuntimeCoordinator>(
                appId, authority->store(), authority->installer(),
                supervisionPolicy, LifecycleClock::system(), recorder.get(),
                AppRuntimeCoordinator::DrainConsumer{}, 10'000);
            authority_ = std::move(authority);
            recorder_ = std::move(recorder);
            coordinator_ = std::move(coordinator);
            appCoordinator_ = std::move(appCoordinator);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool reserve()
    {
        if (!accepting_.load(std::memory_order_acquire)) return false;
        qsizetype pending = pending_.load(std::memory_order_relaxed);
        do {
            if (pending >= MaximumPendingLifecycleOperations) return false;
        } while (!pending_.compare_exchange_weak(
            pending, pending + 1, std::memory_order_acq_rel));
        if (!accepting_.load(std::memory_order_acquire)) {
            release();
            return false;
        }
        return true;
    }

    void release() noexcept { pending_.fetch_sub(1, std::memory_order_release); }

    void execute(std::function<void(UpdateLifecycleCoordinator &)> operation)
    {
        struct PendingRelease final {
            HostLifecycleRuntime *runtime;
            ~PendingRelease() { runtime->release(); }
        } releaseOnExit{this};
        if (!accepting_.load(std::memory_order_acquire) || !operation
            || coordinator_ == nullptr) return;
        operation(*coordinator_);
    }

    void executeApp(std::function<void(AppRuntimeCoordinator &)> operation)
    {
        struct PendingRelease final {
            HostLifecycleRuntime *runtime;
            ~PendingRelease() { runtime->release(); }
        } releaseOnExit{this};
        if (!accepting_.load(std::memory_order_acquire) || !operation
            || appCoordinator_ == nullptr) return;
        operation(*appCoordinator_);
    }

    [[nodiscard]] std::shared_ptr<RuntimePackageAuthority> authority() const
        noexcept
    {
        return authority_;
    }

    [[nodiscard]] AppRuntimeResult executeStartupOperation(
        const bool installing,
        const QString &packagePath,
        const qint64 nowMs)
    {
        if (QThread::currentThread() != thread()
            || appCoordinator_ == nullptr) {
            AppRuntimeResult failed;
            failed.code = AppRuntimeResultCode::FailedClosed;
            failed.stableError = QStringLiteral(
                "host.runtime.initialization_failed");
            return failed;
        }
        return installing
            ? appCoordinator_->installAndActivate(packagePath, nowMs)
            : appCoordinator_->startOffline(nowMs);
    }

    void closeAdmission() noexcept
    {
        accepting_.store(false, std::memory_order_release);
    }

    void armShutdownOnThreadExit() noexcept
    {
        shutdownOnThreadExitRequested_.store(true,
                                              std::memory_order_release);
    }

    void shutdownOnThreadExit() noexcept
    {
        if (!shutdownOnThreadExitRequested_.exchange(
                false, std::memory_order_acq_rel)) {
            return;
        }
        (void)shutdown();
    }

    [[nodiscard]] std::shared_ptr<HostLifecycleShutdownHandoff>
    reserveShutdownHandoff() noexcept
    {
        bool expected = false;
        if (!shutdownHandoffRegistrationStarted_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return shutdownHandoffRegistered_.load(std::memory_order_acquire)
                ? shutdownHandoff_ : nullptr;
        }
        try {
            const std::shared_ptr<HostLifecycleShutdownHandoff> handoff =
                shutdownHandoff_;
            const WorkerRetirementManager::Ticket ticket =
                WorkerRetirementManager::instance().retire(
                    [handoff] { return handoff->attempt(); },
                    [handoff, publishFailure = publishFailure_](
                        const bool succeeded, const QString &stableError) {
                        if (!succeeded && publishFailure) {
                            publishFailure(stableError,
                                           handoff->nativeError());
                        }
                    });
            if (ticket == 0) {
                shutdownHandoffRegistrationStarted_.store(
                    false, std::memory_order_release);
                return nullptr;
            }
            shutdownHandoffRegistered_.store(true, std::memory_order_release);
            return handoff;
        } catch (...) {
            shutdownHandoffRegistrationStarted_.store(
                false, std::memory_order_release);
            return nullptr;
        }
    }

    [[nodiscard]] bool shutdown()
    {
        closeAdmission();
        if (!shutdownStarted_) {
            shutdownStarted_ = true;
            UpdateLifecycleShutdownResult appCleanup;
            if (appCoordinator_ != nullptr) {
                (void)appCoordinator_->beginShutdown();
                appCleanup = appCoordinator_->beginHostShutdownCleanup();
            }
            if (coordinator_ == nullptr) {
                if (shutdownHandoffRegistered_.load(
                        std::memory_order_acquire)) {
                    shutdownHandoff_->complete(
                        std::move(appCleanup.cleanupOwner));
                }
                if (!appCleanup.succeeded() && publishFailure_) {
                    publishFailure_(appCleanup.stableError,
                                    appCleanup.nativeError);
                }
                return true;
            }
            UpdateLifecycleShutdownResult result =
                coordinator_->beginHostShutdown();
            if (appCleanup.cleanupOwner.has_value()) {
                if (result.cleanupOwner.has_value()) {
                    result.cleanupOwner->append(
                        std::move(*appCleanup.cleanupOwner));
                } else {
                    result.cleanupOwner =
                        std::move(appCleanup.cleanupOwner);
                }
            }
            if (result.stableError.isEmpty()
                && !appCleanup.stableError.isEmpty()) {
                result.stableError = std::move(appCleanup.stableError);
                result.nativeError = appCleanup.nativeError;
            }
            const bool succeeded = result.succeeded();
            const bool handoffRegistered =
                shutdownHandoffRegistered_.load(std::memory_order_acquire);
            if (handoffRegistered) {
                shutdownHandoff_->complete(std::move(result.cleanupOwner));
            }
            if (publishFailure_) {
                if (!succeeded) {
                    publishFailure_(result.stableError, result.nativeError);
                }
            }
            if (handoffRegistered || succeeded) return true;
            if (!result.cleanupOwner.has_value()) return true;
            pendingShutdownCleanup_.emplace(
                std::move(*result.cleanupOwner));
        }
        if (submitShutdownCleanup()) return true;
        if (publishFailure_) {
            publishFailure_(
                QStringLiteral("host.launch.retirement_unavailable"), 0U);
        }
        scheduleShutdownCleanupRetry();
        return false;
    }

private:
    [[nodiscard]] bool submitShutdownCleanup() noexcept
    {
        try {
            if (sharedShutdownCleanup_ == nullptr
                && pendingShutdownCleanup_.has_value()) {
                sharedShutdownCleanup_ =
                    std::make_shared<UpdateLifecycleShutdownCleanup>(
                        std::move(*pendingShutdownCleanup_));
                pendingShutdownCleanup_.reset();
            }
            if (sharedShutdownCleanup_ == nullptr) return true;
            const std::shared_ptr<UpdateLifecycleShutdownCleanup> cleanupOwner =
                sharedShutdownCleanup_;
            const WorkerRetirementManager::Ticket ticket =
                WorkerRetirementManager::instance().retire(
                    [cleanupOwner] {
                        const ImmutablePackageGuardCloseResult closed =
                            cleanupOwner->close();
                        if (closed.value.has_value()) {
                            return WorkerRetirementAttemptResult{true, {}};
                        }
                        return WorkerRetirementAttemptResult{
                            false,
                            closed.errorCode.isEmpty()
                                ? QStringLiteral(
                                      "package.immutable_restore_failed")
                                : closed.errorCode};
                    },
                    [publishFailure = publishFailure_](
                        const bool succeeded, const QString &stableError) {
                        if (!succeeded && publishFailure) {
                            publishFailure(stableError, 0U);
                        }
                    });
            if (ticket == 0) return false;
            sharedShutdownCleanup_.reset();
            return true;
        } catch (...) {
            return false;
        }
    }

    void scheduleShutdownCleanupRetry()
    {
        if (shutdownCleanupRetryScheduled_) return;
        shutdownCleanupRetryScheduled_ = true;
        QTimer::singleShot(25, this, [this] {
            shutdownCleanupRetryScheduled_ = false;
            if (submitShutdownCleanup()) {
                if (QThread *const owningThread = thread()) owningThread->quit();
                return;
            }
            scheduleShutdownCleanupRetry();
        });
    }

    std::shared_ptr<RuntimePackageAuthority> authority_;
    std::unique_ptr<EventRecorder> recorder_;
    std::unique_ptr<UpdateLifecycleCoordinator> coordinator_;
    std::unique_ptr<AppRuntimeCoordinator> appCoordinator_;
    FailurePublisher publishFailure_;
    std::shared_ptr<HostLifecycleShutdownHandoff> shutdownHandoff_;
    std::optional<UpdateLifecycleShutdownCleanup> pendingShutdownCleanup_;
    std::shared_ptr<UpdateLifecycleShutdownCleanup> sharedShutdownCleanup_;
    std::atomic<qsizetype> pending_{0};
    std::atomic_bool accepting_{true};
    std::atomic_bool shutdownHandoffRegistrationStarted_{false};
    std::atomic_bool shutdownHandoffRegistered_{false};
    std::atomic_bool shutdownOnThreadExitRequested_{false};
    bool shutdownStarted_ = false;
    bool shutdownCleanupRetryScheduled_ = false;
};
}

HostApplication::HostApplication(QUrl mockOrigin, QObject *parent)
    : QObject(parent), mockOrigin_(std::move(mockOrigin)),
      fileDialogCoordinator_(std::make_unique<FileDialogCoordinator>())
{
}

HostApplication::HostApplication(HostRuntimeConfig runtimeConfig,
                                 QObject *parent)
    : QObject(parent)
    , runtimeConfig_(std::move(runtimeConfig))
    , mockOrigin_(runtimeConfig_->mockOrigin())
    , fileDialogCoordinator_(std::make_unique<FileDialogCoordinator>())
{
}

HostApplication::~HostApplication()
{
    acceptingLifecycle_.store(false, std::memory_order_release);
    if (updateHealthTimer_ != nullptr) updateHealthTimer_->stop();
    if (installedPackageLauncher_ != nullptr) installedPackageLauncher_->cancel();
    clearPendingWorkerContext();
    const QList<QPointer<AppTabRuntimeController>> appRuntimeSnapshot =
        appTabRuntimeControllers_.values();
    for (const QPointer<AppTabRuntimeController> &controller :
         appRuntimeSnapshot) {
        if (!controller.isNull()) {
            controller->close(QStringLiteral("host.application.stopping"));
        }
    }
    detachWorkerContext(QStringLiteral("host.application.stopping"));
    if (fileDialogCoordinator_ != nullptr) {
        fileDialogCoordinator_->shutdown();
    }

    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(updateLifecycleRuntime_.data()));
    std::shared_ptr<HostLifecycleShutdownHandoff> shutdownHandoff;
    if (runtime) {
        runtime->closeAdmission();
        // Reserve retirement admission synchronously; only the detached
        // retirement attempt waits for the queued lifecycle shutdown.
        shutdownHandoff = runtime->reserveShutdownHandoff();
        runtime->armShutdownOnThreadExit();
    }
    QThread *const lifecycleThread = updateLifecycleThread_;
    if (runtime && lifecycleThread != nullptr) {
        if (!lifecycleThread->isRunning()) {
            (void)runtime->shutdown();
            lifecycleThread->quit();
        } else {
#ifdef Q_BROWSER_HOST_TESTING
            const bool forceQueueFailure =
                forceLifecycleShutdownQueueFailureForTesting_;
#else
            constexpr bool forceQueueFailure = false;
#endif
            const bool queued = forceQueueFailure
                ? false
                : QMetaObject::invokeMethod(
                      runtime,
                      [runtime, lifecycleThread, shutdownHandoff] {
                          if (!runtime) {
                              if (shutdownHandoff) shutdownHandoff->fail();
                              lifecycleThread->quit();
                              return;
                          }
                          if (runtime->shutdown()) lifecycleThread->quit();
                      },
                      Qt::QueuedConnection);
            if (!queued) lifecycleThread->quit();
        }
    } else if (runtime) {
        (void)runtime->shutdown();
    } else if (lifecycleThread != nullptr) {
        lifecycleThread->quit();
    }
    updateLifecycleRuntime_ = nullptr;
    updateLifecycleThread_ = nullptr;
    if (mainWindow_ != nullptr) {
        MainWindow *const retiringWindow = mainWindow_.release();
        retiringWindow->deleteLater();
    }
}

bool HostApplication::enqueueLifecycle(
    std::function<void(UpdateLifecycleCoordinator &)> operation)
{
#ifdef Q_BROWSER_HOST_TESTING
    if (lifecycleQueueFullForTesting_) return false;
#endif
    if (!operation || !acceptingLifecycle_.load(std::memory_order_acquire))
        return false;
    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(updateLifecycleRuntime_.data()));
    if (!runtime || !runtime->reserve()) return false;
    const bool queued = QMetaObject::invokeMethod(
        runtime,
        [runtime, operation = std::move(operation)]() mutable {
            if (runtime) runtime->execute(std::move(operation));
        }, Qt::QueuedConnection);
    if (!queued && runtime) runtime->release();
    return queued;
}

bool HostApplication::enqueueAppRuntime(
    std::function<void(AppRuntimeCoordinator &)> operation)
{
#ifdef Q_BROWSER_HOST_TESTING
    if (lifecycleQueueFullForTesting_) return false;
#endif
    if (appRuntimeFailedClosed_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!operation || !acceptingLifecycle_.load(std::memory_order_acquire)) {
        return false;
    }
    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(updateLifecycleRuntime_.data()));
    if (!runtime || !runtime->reserve()) return false;
    const bool queued = QMetaObject::invokeMethod(
        runtime,
        [runtime, operation = std::move(operation)]() mutable {
            if (runtime) runtime->executeApp(std::move(operation));
        },
        Qt::QueuedConnection);
    if (!queued && runtime) runtime->release();
    return queued;
}

bool HostApplication::enqueuePackageRuntime(
    std::function<void(AppRuntimeCoordinator &)> operation)
{
#ifdef Q_BROWSER_HOST_TESTING
    if (lifecycleQueueFullForTesting_) return false;
#endif
    if (!operation || !acceptingLifecycle_.load(std::memory_order_acquire)) {
        return false;
    }
    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(updateLifecycleRuntime_.data()));
    if (!runtime || !runtime->reserve()) return false;
    const bool queued = QMetaObject::invokeMethod(
        runtime,
        [runtime, operation = std::move(operation)]() mutable {
            if (runtime) runtime->executeApp(std::move(operation));
        },
        Qt::QueuedConnection);
    if (!queued && runtime) runtime->release();
    return queued;
}

quint64 HostApplication::runtimeIncarnationForTab(const QString &tabId)
{
    if (tabId.isEmpty()) return 0;
    const auto existing = appRuntimeIncarnations_.constFind(tabId);
    if (existing != appRuntimeIncarnations_.cend()) return *existing;
    if (nextCapabilityRuntimeIncarnation_ == 0
        || nextCapabilityRuntimeIncarnation_
               == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    const quint64 incarnation = nextCapabilityRuntimeIncarnation_++;
    if (incarnation == 0) return 0;
    appRuntimeIncarnations_.insert(tabId, incarnation);
    return incarnation;
}

quint64 HostApplication::advanceRuntimeIncarnationForTab(const QString &tabId)
{
    if (tabId.isEmpty()) return 0;
    if (runtimeIncarnationForTab(tabId) == 0) return 0;
    if (nextCapabilityRuntimeIncarnation_ == 0
        || nextCapabilityRuntimeIncarnation_
               == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    const quint64 incarnation = nextCapabilityRuntimeIncarnation_++;
    if (incarnation == 0) return 0;
    appRuntimeIncarnations_[tabId] = incarnation;
    return incarnation;
}

AppTabRuntimeController *HostApplication::ensureAppTabRuntimeController(
    const QString &tabId)
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || packageAuthority_ == nullptr || mainWindow_ == nullptr
        || gestureRouter_ == nullptr || fileDialogCoordinator_ == nullptr
        || tabId.isEmpty()) {
        return nullptr;
    }
    TabController *const tab = mainWindow_->tabController(tabId);
    if (tab == nullptr) return nullptr;
    if (AppTabRuntimeController *const existing =
            tab->appRuntimeController();
        existing != nullptr) {
        appTabRuntimeControllers_.insert(tabId, existing);
        return existing;
    }

    const SandboxApprovedRoots roots{
        runtimeConfig_->packageStoreRoot(), runtimeConfig_->sandboxTempRoot(),
        runtimeConfig_->immutableRuntimeRoots()};
    QPointer<HostApplication> guard(this);
    const AppTabRuntimeController::AdmissionCallback admission =
        [guard](const WorkerLaunchRequest &request,
                InstalledPackageWorkerLauncher::AdmissionCompletion complete) {
            if (!guard || !complete) return false;
            const bool queued = guard->enqueueAppRuntime(
                [guard, request, complete = std::move(complete)](
                    AppRuntimeCoordinator &coordinator) mutable {
#ifdef Q_BROWSER_HOST_TESTING
                    std::optional<AppRuntimeResult> injectedResult;
                    const auto admissionHooks = qbrowser_host_testing::
                        installedPackageWorkerLauncherTestHooks();
                    if (admissionHooks.beforeAdmissionDecision) {
                        injectedResult = admissionHooks.beforeAdmissionDecision(
                            request, coordinator);
                    }
#endif
                    const AppRuntimeResult result =
                        coordinator.admitAuthenticatedWorker(request);
                    const bool accepted =
                        result.code == AppRuntimeResultCode::Applied;
                    bool ignoredStale =
                        result.code == AppRuntimeResultCode::IgnoredStale;
#ifdef Q_BROWSER_HOST_TESTING
                    const bool synchronouslySuperseded = injectedResult.has_value()
                        && std::ranges::any_of(
                            injectedResult->actions,
                            [&request](const AppRuntimeAction &action) {
                                return action.kind
                                           == AppRuntimeActionKind::Launch
                                    && action.launch.has_value()
                                    && !hasSameWorkerLaunchAuthority(
                                        *action.launch, request);
                            });
                    if (synchronouslySuperseded
                        && result.code == AppRuntimeResultCode::Rejected
                        && result.stableError
                               == QStringLiteral("launch_authority_mismatch")) {
                        ignoredStale = true;
                    }
#endif
                    complete({accepted,
                              accepted || ignoredStale
                                  ? QString{}
                                  : (result.stableError.isEmpty()
                                         ? QStringLiteral(
                                               "host.launch.admission_rejected")
                                         : result.stableError),
                              ignoredStale,
                              accepted
                                  ? std::optional<WorkerLaunchRequest>(request)
                                  : std::nullopt});
                    if (!result.actions.isEmpty()
                        || (result.code != AppRuntimeResultCode::Applied
                            && !ignoredStale)) {
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, result] {
                                if (guard) guard->handleAppRuntimeResult(result);
                            },
                            Qt::QueuedConnection);
                    }
#ifdef Q_BROWSER_HOST_TESTING
                    if (injectedResult.has_value()
                        && (!injectedResult->actions.isEmpty()
                            || injectedResult->code
                                   != AppRuntimeResultCode::Applied)) {
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, injected = std::move(*injectedResult)] {
                                if (guard) {
                                    guard->handleAppRuntimeResult(injected);
                                }
                            },
                            Qt::QueuedConnection);
                    }
#endif
                });
            if (!queued && guard) {
                guard->failClosedAppRuntime(
                    QStringLiteral("host.runtime.critical_event_dropped"));
            }
            return queued;
        };
    auto controller = std::make_unique<AppTabRuntimeController>(
        tabId, tab, mainWindow_.get(), gestureRouter_.get(),
        fileDialogCoordinator_.get(), packageAuthority_,
        roots, runtimeConfig_->workerExecutable(),
        runtimeConfig_->sandboxTempRoot(), runtimeConfig_->mockOrigin(),
        runtimeConfig_->storageDirectory(),
        static_cast<quintptr>(mainWindow_->winId()), admission,
        appRuntimeTransportGate_, appRuntimeTransportMutex_);
    if (!controller->isAccepting()) return nullptr;

    AppTabRuntimeController *const raw = controller.get();
    const QPointer<HostApplication> hostGuard(this);
    const QPointer<AppTabRuntimeController> runtimeGuard(raw);
    connect(raw, &AppTabRuntimeController::ready, this,
            [hostGuard, runtimeGuard](const WorkerLaunchRequest &request,
                                      const quint32 processId) {
                const WorkerLaunchRequest readyRequest = request;
                if (!hostGuard || !runtimeGuard) return;
                emit hostGuard->packageWorkerReadyForTab(
                    readyRequest.tabId, readyRequest.runtimeIncarnation,
                    readyRequest.lease.appId, readyRequest.lease.version,
                    readyRequest.lease.packageDirectory,
                    readyRequest.attempt.activation.value,
                    readyRequest.attempt.attempt.value, processId,
                    readyRequest.lease.leaseAuthorityEpoch);
                if (!hostGuard || !runtimeGuard) return;
                if (hostGuard->mainWindow_ != nullptr
                    && hostGuard->mainWindow_->tabModel() != nullptr
                    && hostGuard->mainWindow_->tabModel()->activeId()
                           == readyRequest.tabId) {
                    emit hostGuard->packageWorkerReady(
                        readyRequest.lease.appId, readyRequest.lease.version,
                        readyRequest.lease.packageDirectory,
                        readyRequest.attempt.activation.value,
                        readyRequest.attempt.attempt.value, processId);
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::workerExited, this,
            [hostGuard, runtimeGuard](const WorkerLaunchRequest &request,
                                      const bool expected) {
                const WorkerLaunchRequest exitedRequest = request;
                if (!hostGuard || !runtimeGuard) return;
                emit hostGuard->packageWorkerExitedForTab(
                    exitedRequest.tabId, exitedRequest.runtimeIncarnation,
                    exitedRequest.attempt.activation.value,
                    exitedRequest.attempt.attempt.value,
                    exitedRequest.lease.leaseAuthorityEpoch, expected);
                if (!hostGuard || !runtimeGuard) return;
                if (!expected) {
                    if (hostGuard->mainWindow_ != nullptr
                        && hostGuard->mainWindow_->tabModel() != nullptr
                        && hostGuard->mainWindow_->tabModel()->activeId()
                               == exitedRequest.tabId) {
                        emit hostGuard->packageWorkerExited(
                            exitedRequest.attempt.activation.value,
                            exitedRequest.attempt.attempt.value);
                        if (!hostGuard || !runtimeGuard) return;
                    }
                }
                const WorkerExitReason reason = expected
                    ? WorkerExitReason::Clean : WorkerExitReason::Crashed;
                const FullAttemptKey key{
                    {exitedRequest.tabId, exitedRequest.runtimeIncarnation},
                    exitedRequest.attempt,
                    exitedRequest.lease.leaseAuthorityEpoch};
                const bool queued = hostGuard->enqueueAppRuntime(
                    [hostGuard, key, reason](
                        AppRuntimeCoordinator &coordinator) {
                        const AppRuntimeResult result =
                            coordinator.workerExited(key, reason, -1);
                        if (!result.actions.isEmpty()
                            || result.code != AppRuntimeResultCode::Applied) {
                            QMetaObject::invokeMethod(
                                hostGuard,
                                [hostGuard, result] {
                                    if (hostGuard) {
                                        hostGuard->handleAppRuntimeResult(result);
                                    }
                                },
                                Qt::QueuedConnection);
                        }
                    });
                if (!queued && hostGuard) {
                    hostGuard->failClosedAppRuntime(
                        QStringLiteral("host.runtime.critical_event_dropped"));
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::workerFailed, this,
            [this](const WorkerLaunchRequest &request, const QString &error,
                   const quint32 nativeError) {
                const bool cleanupFailure = error == QStringLiteral(
                        "host.launch.temp_cleanup_failed")
                    || error == QStringLiteral("host.launch.process_wait_failed")
                    || error == QStringLiteral("host.launch.process_cleanup_failed")
                    || error.startsWith(QStringLiteral("sandbox.process."))
                    || error.startsWith(QStringLiteral("sandbox.job."))
                    || error.startsWith(QStringLiteral("sandbox.acl."))
                    || error.startsWith(QStringLiteral("package.immutable_"));
                const bool admissionFailure =
                    error.startsWith(QStringLiteral("host.launch.admission_"));
                const FullAttemptKey key{
                    {request.tabId, request.runtimeIncarnation}, request.attempt,
                    request.lease.leaseAuthorityEpoch};
                QPointer<HostApplication> guard(this);
                guard->handleAppRuntimeFailure(error, nativeError);
                if (!guard) return;
#ifdef Q_BROWSER_HOST_TESTING
                const auto hooks = qbrowser_host_testing::
                    installedPackageWorkerLauncherTestHooks();
                if (hooks.afterFailureSignalBeforeLifecycleEnqueue) {
                    hooks.afterFailureSignalBeforeLifecycleEnqueue();
                    if (!guard) return;
                }
#endif
                const bool queued = guard->enqueueAppRuntime(
                    [guard, key, error, nativeError, cleanupFailure,
                     admissionFailure](AppRuntimeCoordinator &coordinator) {
                        const AppRuntimeResult result = cleanupFailure
                            ? coordinator.workerCleanupFailed(
                                  key, error, nativeError, -1)
                            : admissionFailure
                            ? coordinator.workerAdmissionFailed(key, error, -1)
                            : coordinator.workerExited(
                                  key, WorkerExitReason::StartupFailure, -1);
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, result] {
                                if (guard) guard->handleAppRuntimeResult(result);
                            },
                            Qt::QueuedConnection);
                    });
                if (!queued && guard) {
                    guard->failClosedAppRuntime(
                        QStringLiteral("host.runtime.critical_event_dropped"));
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::launcherTerminalFailure, this,
            [this](const QString &, const QString &error,
                   const quint32 nativeError) {
                failClosedAppRuntime(error, nativeError);
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::heartbeatObserved, this,
            [this](const WorkerLaunchRequest &request,
                   const quint64 generation) {
                Q_UNUSED(generation);
                const qint64 receivedMonotonicMs = hostMonotonicNowMs();
                const FullAttemptKey key{
                    {request.tabId, request.runtimeIncarnation}, request.attempt,
                    request.lease.leaseAuthorityEpoch};
                const QString coalesceKey = appHeartbeatKey(request);
                {
                    std::lock_guard lock(appHeartbeatMutex_);
                    const bool alreadyQueued =
                        pendingAppHeartbeatTimes_.contains(coalesceKey);
                    pendingAppHeartbeatTimes_.insert(coalesceKey,
                                                     receivedMonotonicMs);
                    if (alreadyQueued) return;
                }
                QPointer<HostApplication> guard(this);
                const bool queued = enqueueAppRuntime(
                    [guard, key, coalesceKey, receivedMonotonicMs](
                        AppRuntimeCoordinator &coordinator) {
                        const qint64 latestReceived = guard != nullptr
                            ? guard->takePendingAppHeartbeat(coalesceKey)
                                  .value_or(receivedMonotonicMs)
                            : receivedMonotonicMs;
                        const AppRuntimeResult result = coordinator.heartbeat(
                            key, latestReceived);
                        if (!result.actions.isEmpty()
                            || result.code != AppRuntimeResultCode::Applied) {
                            QMetaObject::invokeMethod(
                                guard,
                                [guard, result] {
                                    if (guard) guard->handleAppRuntimeResult(result);
                                },
                                Qt::QueuedConnection);
                        }
                    });
                if (!queued) (void)takePendingAppHeartbeat(coalesceKey);
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::routeLoadAcknowledged, this,
            [hostGuard, runtimeGuard](const WorkerLaunchRequest &request,
                                      const QString &route,
                                      const quint64 sessionGeneration) {
                const WorkerLaunchRequest acknowledgedRequest = request;
                const QString acknowledgedRoute = route;
                const auto isCurrentAcknowledgement = [&] {
                    if (!hostGuard || !runtimeGuard
                        || hostGuard->mainWindow_ == nullptr
                        || hostGuard->mainWindow_->tabModel() == nullptr
                        || sessionGeneration == 0
                        || hostGuard->appRuntimeIncarnations_.value(
                               acknowledgedRequest.tabId, 0)
                               != acknowledgedRequest.runtimeIncarnation
                        || hostGuard->appTabRuntimeControllers_.value(
                               acknowledgedRequest.tabId) != runtimeGuard) {
                        return false;
                    }
                    TabController *const tab =
                        hostGuard->mainWindow_->tabController(
                            acknowledgedRequest.tabId);
                    HostWorkerSessionController *const session =
                        runtimeGuard->sessionController();
                    const std::optional<WorkerLaunchRequest> current =
                        runtimeGuard->currentRequest();
                    if (tab == nullptr
                        || tab->appRuntimeController() != runtimeGuard
                        || !current.has_value()
                        || !hasSameWorkerLaunchAuthority(
                               *current, acknowledgedRequest)
                        || session == nullptr
                        || session->state()
                               != HostWorkerSessionState::Running
                        || session->generation() != sessionGeneration
                        || session->pendingRouteLoadCount() != 0) {
                        return false;
                    }
                    const int tabIndex =
                        hostGuard->mainWindow_->tabModel()->indexOfId(
                            acknowledgedRequest.tabId);
                    if (tabIndex < 0) return false;
                    const BrowserTabSnapshot snapshot =
                        hostGuard->mainWindow_->tabModel()->snapshotAt(
                            tabIndex);
                    const BrowserAddress trusted = BrowserAddress::parse(
                        snapshot.address, QStringLiteral("pilot"));
                    return snapshot.kind == BrowserTabKind::App
                        && trusted.isValid()
                        && trusted.kind() == BrowserAddressKind::App
                        && trusted.canonical() == snapshot.address
                        && trusted.appPath() == acknowledgedRoute;
                };
                if (!isCurrentAcknowledgement()) return;
                (void)hostGuard->mainWindow_->tabModel()->setLoadState(
                    acknowledgedRequest.tabId, false, 100);
                if (!isCurrentAcknowledgement()) return;
                emit hostGuard->routeLoadAcknowledgedForTab(
                    acknowledgedRequest.tabId,
                    acknowledgedRequest.runtimeIncarnation,
                    acknowledgedRequest.attempt.activation.value,
                    acknowledgedRequest.attempt.attempt.value,
                    acknowledgedRequest.lease.leaseAuthorityEpoch,
                    sessionGeneration, acknowledgedRoute);
                if (!isCurrentAcknowledgement()) return;
                const QString routeTemplate = pilotRouteTemplate(
                    acknowledgedRoute);
                if (routeTemplate.isEmpty()) return;
                WorkerLaunchRequest telemetryRequest = acknowledgedRequest;
                telemetryRequest.route = acknowledgedRoute;
                (void)hostGuard->enqueueAppRuntime(
                    [telemetryRequest = std::move(telemetryRequest),
                     routeTemplate](
                        AppRuntimeCoordinator &coordinator) {
                        coordinator.recordRouteLoadAcknowledged(
                            telemetryRequest, routeTemplate, 0);
                    });
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::pageMetadataChanged, this,
            [this](const WorkerLaunchRequest &request, const QString &title,
                   const QString &) {
                if (mainWindow_ == nullptr || mainWindow_->tabModel() == nullptr
                    || mainWindow_->tabModel()->indexOfId(request.tabId) < 0) {
                    return;
                }
                (void)mainWindow_->tabModel()->setTitle(request.tabId, title);
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::capabilityRequestObserved, this,
            [hostGuard, runtimeGuard](const WorkerLaunchRequest &request,
                                      const QString &capability,
                                      const QString &operation,
                                      const QVariantMap &payload,
                                      const quint64 sessionGeneration) {
                const WorkerLaunchRequest observedRequest = request;
                const QString observedCapability = capability;
                const QString observedOperation = operation;
                const QVariantMap observedPayload = payload;
                if (!hostGuard || !runtimeGuard) return;
                emit hostGuard->workerCapabilityRequestObservedForTab(
                    observedRequest.tabId,
                    observedRequest.runtimeIncarnation, sessionGeneration,
                    observedCapability, observedOperation, observedPayload,
                    observedRequest.lease.leaseAuthorityEpoch);
                if (!hostGuard || !runtimeGuard) return;
                if (hostGuard->mainWindow_ != nullptr
                    && hostGuard->mainWindow_->tabModel() != nullptr
                    && hostGuard->mainWindow_->tabModel()->activeId()
                           == observedRequest.tabId) {
                    emit hostGuard->workerCapabilityRequestObserved(
                        observedCapability, observedOperation,
                        observedPayload);
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::failed, this,
            [this](const WorkerLaunchRequest &request, const QString &error,
                   const quint64) {
                const FullAttemptKey key{
                    {request.tabId, request.runtimeIncarnation}, request.attempt,
                    request.lease.leaseAuthorityEpoch};
                QPointer<HostApplication> guard(this);
                const bool queued = enqueueAppRuntime(
                    [guard, key, error](AppRuntimeCoordinator &coordinator) {
                        const AppRuntimeResult result = coordinator.workerExited(
                            key, WorkerExitReason::Crashed, -1);
                        Q_UNUSED(error);
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, result] {
                                if (guard) guard->handleAppRuntimeResult(result);
                            },
                            Qt::QueuedConnection);
                    });
                if (!queued) {
                    failClosedAppRuntime(
                        QStringLiteral("host.runtime.critical_event_dropped"));
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::retired, this,
            [this](const QString &tabId, const quint64) {
                appTabRuntimeControllers_.remove(tabId);
            },
            Qt::DirectConnection);

    tab->adoptAppRuntimeController(std::move(controller));
    appTabRuntimeControllers_.insert(tabId, raw);
    return raw;
}

void HostApplication::handleAppLaunchRequested(
    const QString &tabId, const quint64 navigationIncarnation,
    const QString &packageId, const QString &route,
    const bool reload)
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || mainWindow_ == nullptr || route.isEmpty()
        || packageId != runtimeConfig_->appId()) {
        return;
    }
    TabController *const tab = mainWindow_->tabController(tabId);
    if (tab == nullptr || mainWindow_->tabModel() == nullptr) return;
    if (appRuntimeFailedClosed_.load(std::memory_order_acquire)) {
        tab->showTrustedErrorForNavigation(
            navigationIncarnation,
            QStringLiteral("The package runtime is unavailable."));
        return;
    }
    if (!ensureAppTabRuntimeController(tabId)) {
        tab->showTrustedErrorForNavigation(
            navigationIncarnation,
            QStringLiteral("The package worker is unavailable."));
        return;
    }
    AppTabRuntimeController *const runtime = tab->appRuntimeController();
    if (runtime == nullptr) return;
    const int tabIndex = mainWindow_->tabModel()->indexOfId(tabId);
    if (tabIndex < 0) return;
    const BrowserTabSnapshot snapshot =
        mainWindow_->tabModel()->snapshotAt(tabIndex);
    const BrowserAddress trustedAddress = BrowserAddress::parse(
        snapshot.address, QStringLiteral("pilot"));
    if (snapshot.kind != BrowserTabKind::App || !trustedAddress.isValid()
        || trustedAddress.kind() != BrowserAddressKind::App
        || trustedAddress.canonical() != snapshot.address
        || trustedAddress.appPath() != route) {
        return;
    }
    const bool supersedesUnmaterializedLaunch = !reload
        && pendingAppNavigationIncarnations_.contains(tabId)
        && !runtime->currentRequest().has_value()
        && !runtime->hasWorkerContext();
    const quint64 runtimeIncarnation = reload
            || supersedesUnmaterializedLaunch
        ? advanceRuntimeIncarnationForTab(tabId)
        : runtimeIncarnationForTab(tabId);
    if (runtimeIncarnation == 0) {
        tab->showTrustedErrorForNavigation(
            navigationIncarnation,
            QStringLiteral("The package runtime is unavailable."));
        return;
    }
    const TabLaunchAuthority authority{tabId, runtimeIncarnation};
    const TabLaunchIntent intent = reload
        ? TabLaunchIntent::ReloadCurrent
        : TabLaunchIntent::ActivateCurrent;
    QPointer<HostApplication> guard(this);
    const bool queued = enqueueAppRuntime(
        [guard, authority, route, intent, tabId, navigationIncarnation](
            AppRuntimeCoordinator &coordinator) {
            const AppRuntimeResult result = coordinator.requestTabLaunch(
                authority, route, intent, -1);
            QMetaObject::invokeMethod(
                guard,
                [guard, result, tabId, navigationIncarnation] {
                    if (!guard) return;
                    if (result.code != AppRuntimeResultCode::Applied) {
                        if (TabController *const tab =
                                guard->mainWindow_ != nullptr
                                    ? guard->mainWindow_->tabController(tabId)
                                    : nullptr) {
                            tab->showTrustedErrorForNavigation(
                                navigationIncarnation,
                                result.stableError.isEmpty()
                                    ? QStringLiteral(
                                          "The package worker is unavailable.")
                                    : result.stableError);
                        }
                    }
                    guard->handleAppRuntimeResult(result);
                },
                Qt::QueuedConnection);
        });
    if (queued) {
        pendingAppNavigationIncarnations_[tabId] = navigationIncarnation;
        if (!reload && runtime->currentRequest().has_value()
            && runtime->currentRequest()->runtimeIncarnation
                == runtimeIncarnation) {
            (void)runtime->retargetPendingLaunch(
                runtimeIncarnation, navigationIncarnation, route,
                snapshot.address);
        }
#ifdef Q_BROWSER_HOST_TESTING
        emit appLaunchIntentQueuedForTesting(tabId,
                                             static_cast<int>(intent));
#endif
    }
    if (!queued) {
        tab->showTrustedErrorForNavigation(
            navigationIncarnation,
            QStringLiteral("The package runtime queue is unavailable."));
        failClosedAppRuntime(
            QStringLiteral("host.runtime.critical_event_dropped"));
    }
}

void HostApplication::handleAppStopRequested(
    const QString &tabId, const quint64 navigationIncarnation,
    const quint64 runtimeIncarnation)
{
    Q_UNUSED(navigationIncarnation);
    const quint64 effectiveRuntimeIncarnation = runtimeIncarnation != 0
        ? runtimeIncarnation : appRuntimeIncarnations_.value(tabId, 0);
    if (tabId.isEmpty() || effectiveRuntimeIncarnation == 0
        || appRuntimeIncarnations_.value(tabId, 0)
               != effectiveRuntimeIncarnation) {
        return;
    }
    appRuntimeIncarnations_.remove(tabId);
    pendingAppNavigationIncarnations_.remove(tabId);
    const TabLaunchAuthority authority{tabId, effectiveRuntimeIncarnation};
    QPointer<HostApplication> guard(this);
    const bool queued = enqueueAppRuntime(
        [guard, authority](AppRuntimeCoordinator &coordinator) {
            const AppRuntimeResult result = coordinator.closeTab(authority, -1);
            if (!result.actions.isEmpty()
                || result.code != AppRuntimeResultCode::Applied) {
                QMetaObject::invokeMethod(
                    guard,
                    [guard, result] {
                        if (guard) guard->handleAppRuntimeResult(result);
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!queued) {
        failClosedAppRuntime(
            QStringLiteral("host.runtime.critical_event_dropped"));
    }
}

void HostApplication::handleAppRuntimeFailure(const QString &stableError,
                                              const quint32 nativeError)
{
    if (stableError.isEmpty()) return;
    emit updateLifecycleFailed(stableError, nativeError);
}

void HostApplication::failClosedAppRuntime(const QString &stableError,
                                           const quint32 nativeError)
{
    const QString error = stableError.isEmpty()
        ? QStringLiteral("host.runtime.critical_event_dropped")
        : stableError;
    if (appRuntimeFailedClosed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (appRuntimeTransportMutex_ != nullptr) {
        std::lock_guard lock(*appRuntimeTransportMutex_);
        if (appRuntimeTransportGate_ != nullptr) {
            appRuntimeTransportGate_->store(false, std::memory_order_release);
        }
    } else if (appRuntimeTransportGate_ != nullptr) {
        appRuntimeTransportGate_->store(false, std::memory_order_release);
    }
    QPointer<HostApplication> guard(this);
    (void)QMetaObject::invokeMethod(
        this,
        [guard, error, nativeError] {
            if (!guard) return;
            if (guard->installedPackageLauncher_ != nullptr) {
                guard->installedPackageLauncher_->cancel();
            }
            guard->detachWorkerContext(error);
            const QList<QPointer<AppTabRuntimeController>> controllers =
                guard->appTabRuntimeControllers_.values();
            for (const QPointer<AppTabRuntimeController> &controller :
                 controllers) {
                if (controller != nullptr) {
                    controller->failClosed(error);
                    controller->stop(error);
                }
            }
            guard->handleAppRuntimeFailure(error, nativeError);
        },
        Qt::QueuedConnection);
}

std::optional<qint64> HostApplication::takePendingAppHeartbeat(
    const QString &key) noexcept
{
    if (key.isEmpty()) return std::nullopt;
    std::lock_guard lock(appHeartbeatMutex_);
    const auto found = pendingAppHeartbeatTimes_.find(key);
    if (found == pendingAppHeartbeatTimes_.end()) return std::nullopt;
    const qint64 result = found.value();
    pendingAppHeartbeatTimes_.erase(found);
    return result;
}

void HostApplication::handleAppRuntimeResult(const AppRuntimeResult &result)
{
    if (result.code == AppRuntimeResultCode::Rejected
        || result.code == AppRuntimeResultCode::FailedClosed) {
        handleAppRuntimeFailure(
            result.stableError.isEmpty()
                ? QStringLiteral("host.runtime.app_runtime_failed")
                : result.stableError,
            result.nativeError);
    }
    if (appRuntimeFailedClosed_.load(std::memory_order_acquire)) {
        return;
    }
    for (const AppRuntimeAction &action : result.actions) {
        TabController *tab = mainWindow_ != nullptr
            ? mainWindow_->tabController(action.tabId) : nullptr;
        AppTabRuntimeController *runtime = tab != nullptr
            ? tab->appRuntimeController() : nullptr;
        const quint64 expectedRuntime = appRuntimeIncarnations_.value(
            action.tabId, 0);
        const quint64 currentRuntime = runtime != nullptr
            && runtime->currentRequest().has_value()
            ? runtime->currentRequest()->runtimeIncarnation
            : runtime != nullptr ? runtime->runtimeIncarnation() : 0;
        const bool actionTargetsExpectedRuntime =
            action.runtimeIncarnation != 0
            && expectedRuntime == action.runtimeIncarnation
            && (action.kind == AppRuntimeActionKind::Launch
                || currentRuntime == 0
                || currentRuntime == action.runtimeIncarnation);
        const bool retirementAction =
            action.kind == AppRuntimeActionKind::Revoke
            || action.kind == AppRuntimeActionKind::Stop
            || action.kind == AppRuntimeActionKind::IsolateSession;
        const bool actionRetiresObservedRuntime = retirementAction
            && action.runtimeIncarnation != 0 && currentRuntime != 0
            && currentRuntime == action.runtimeIncarnation;
        if (!actionTargetsExpectedRuntime && !actionRetiresObservedRuntime
            && action.kind != AppRuntimeActionKind::AwaitAuthorityDrain) {
            continue;
        }
        switch (action.kind) {
        case AppRuntimeActionKind::Launch: {
            if (!action.launch.has_value()) break;
            if (tab == nullptr
                || action.launch->tabId != action.tabId
                || action.launch->runtimeIncarnation
                       != action.runtimeIncarnation) {
                break;
            }
            if (runtime == nullptr) runtime = ensureAppTabRuntimeController(
                action.tabId);
            if (runtime == nullptr) {
                const QPointer<HostApplication> hostGuard(this);
                handleAppRuntimeFailure(
                    QStringLiteral("host.runtime.app_launch_rejected"));
                if (!hostGuard) return;
                break;
            }
            quint64 navigationIncarnation =
                pendingAppNavigationIncarnations_.value(
                    action.tabId, tab->incarnation());
            if (action.launch->recovery) {
                const int tabIndex = mainWindow_ != nullptr
                    && mainWindow_->tabModel() != nullptr
                    ? mainWindow_->tabModel()->indexOfId(action.tabId) : -1;
                if (tab->lifecycle() == BrowserTabLifecycle::Closing
                    || tab->lifecycle() == BrowserTabLifecycle::Retired
                    || tabIndex < 0) {
                    break;
                }
                const BrowserTabSnapshot snapshot =
                    mainWindow_->tabModel()->snapshotAt(tabIndex);
                const BrowserAddress trusted = BrowserAddress::parse(
                    snapshot.address, QStringLiteral("pilot"));
                if (snapshot.kind != BrowserTabKind::App
                    || !trusted.isValid()
                    || trusted.kind() != BrowserAddressKind::App
                    || trusted.canonical() != snapshot.address
                    || trusted.appPath() != action.launch->route) {
                    break;
                }
                // Detaching the crashed surface advances the tab's
                // navigation incarnation while preserving its App
                // descriptor.  A coordinator recovery must bind to that
                // exact post-detach incarnation; user-driven launches still
                // require their originally queued incarnation below.
                navigationIncarnation = tab->incarnation();
                pendingAppNavigationIncarnations_[action.tabId] =
                    navigationIncarnation;
            }
            const QPointer<HostApplication> hostGuard(this);
            const QPointer<TabController> tabGuard(tab);
            const QPointer<AppTabRuntimeController> runtimeGuard(runtime);
            const WorkerLaunchRequest launchRequest = *action.launch;
            const int launchTabIndex = mainWindow_ != nullptr
                    && mainWindow_->tabModel() != nullptr
                ? mainWindow_->tabModel()->indexOfId(action.tabId) : -1;
            const QString launchCanonicalAddress = launchTabIndex >= 0
                ? mainWindow_->tabModel()->snapshotAt(launchTabIndex).address
                : QString();
            const auto dropPendingLaunch =
                [hostGuard, runtimeGuard, launchRequest](
                    const bool reachedLauncher) {
                    if (!hostGuard) return;
                    if (reachedLauncher && runtimeGuard) {
                        (void)runtimeGuard->cancelLaunchIfCurrent(
                            launchRequest,
                            QStringLiteral("host.worker.launch_stale"));
                    }
                    if (!hostGuard) return;
                    const bool queued = hostGuard->enqueueAppRuntime(
                        [launchRequest](AppRuntimeCoordinator &coordinator) {
                            (void)coordinator.cancelPendingLaunch(
                                launchRequest);
                        });
                    if (!queued && hostGuard) {
                        hostGuard->failClosedAppRuntime(QStringLiteral(
                            "host.runtime.critical_event_dropped"));
                    }
                };
            const auto isCurrentLaunchTarget = [&] {
                if (!hostGuard || !tabGuard || !runtimeGuard
                    || hostGuard->mainWindow_ == nullptr
                    || hostGuard->mainWindow_->tabModel() == nullptr
                    || navigationIncarnation == 0
                    || tabGuard->incarnation() != navigationIncarnation
                    || tabGuard->lifecycle()
                           == BrowserTabLifecycle::Closing
                    || tabGuard->lifecycle()
                           == BrowserTabLifecycle::Retired
                    || hostGuard->mainWindow_->tabController(action.tabId)
                           != tabGuard.data()
                    || tabGuard->appRuntimeController()
                           != runtimeGuard.data()
                    || hostGuard->appRuntimeIncarnations_.value(
                           action.tabId, 0)
                           != action.runtimeIncarnation) {
                    return false;
                }
                const int tabIndex =
                    hostGuard->mainWindow_->tabModel()->indexOfId(
                        action.tabId);
                if (tabIndex < 0) return false;
                const BrowserTabSnapshot snapshot =
                    hostGuard->mainWindow_->tabModel()->snapshotAt(tabIndex);
                const BrowserAddress trusted = BrowserAddress::parse(
                    snapshot.address, QStringLiteral("pilot"));
                return snapshot.kind == BrowserTabKind::App
                    && trusted.isValid()
                    && trusted.kind() == BrowserAddressKind::App
                    && trusted.canonical() == snapshot.address
                    && trusted.appPath() == launchRequest.route;
            };
            if (!isCurrentLaunchTarget()) {
                dropPendingLaunch(false);
                break;
            }
            bool launchAccepted = tabGuard->prepareAppLaunch(
                    launchRequest.lease.appId, navigationIncarnation);
            bool reachedLauncher = false;
#ifdef Q_BROWSER_HOST_TESTING
            if (hostGuard) {
                std::function<void()> afterPrepare = std::move(
                    hostGuard->afterPrepareAppLaunchHookForTesting_);
                hostGuard->afterPrepareAppLaunchHookForTesting_ = {};
                if (afterPrepare) afterPrepare();
            }
#endif
            if (!hostGuard) return;
            if (!isCurrentLaunchTarget()) {
                dropPendingLaunch(false);
                break;
            }
            if (launchAccepted) {
                reachedLauncher = true;
                launchAccepted = runtimeGuard->requestLaunch(
                    launchRequest, navigationIncarnation,
                    launchCanonicalAddress);
            }
            if (!hostGuard) return;
            if (!isCurrentLaunchTarget()) {
                dropPendingLaunch(true);
                break;
            }
            if (launchAccepted) {
                const std::optional<WorkerLaunchRequest> current =
                    runtimeGuard->currentRequest();
                if (!current.has_value()
                    || !hasSameWorkerLaunchAuthority(
                        *current, launchRequest)) {
                    dropPendingLaunch(true);
                    break;
                }
            }
            if (!launchAccepted) {
                dropPendingLaunch(reachedLauncher);
                handleAppRuntimeFailure(
                    QStringLiteral("host.runtime.app_launch_rejected"));
                if (!hostGuard) return;
            }
            break;
        }
        case AppRuntimeActionKind::Stop:
            if (runtime != nullptr) {
                runtime->stop(QStringLiteral("host.app_runtime.stop"));
            }
            break;
        case AppRuntimeActionKind::IsolateSession:
            if (runtime != nullptr) {
                runtime->stop(QStringLiteral("host.app_runtime.isolate"));
            }
            break;
        case AppRuntimeActionKind::TrustedCrash:
        case AppRuntimeActionKind::FailedClosed:
            if (tab != nullptr && tab->lifecycle() != BrowserTabLifecycle::Closing
                && tab->lifecycle() != BrowserTabLifecycle::Retired) {
                tab->showTrustedError(
                    result.stableError.isEmpty()
                        ? QStringLiteral("The package worker is unavailable.")
                        : result.stableError,
                    true);
            }
            break;
        case AppRuntimeActionKind::AwaitAuthorityDrain:
            if (!action.drain.has_value()) break;
            {
                const AuthorityDrainBatch batch = *action.drain;
                QPointer<HostApplication> guard(this);
                try {
                    std::thread([guard, batch] {
                        bool drained = true;
                        for (const auto &ticket : batch.tickets) {
                            if (!ticket.waitUntil(batch.monotonicDeadlineMs)) {
                                drained = false;
                                break;
                            }
                        }
                        if (!guard) return;
                        const qint64 observedNow = hostMonotonicNowMs();
                        const qint64 completionNow = drained
                            ? observedNow
                            : std::max(observedNow,
                                       batch.monotonicDeadlineMs + 1);
                        const bool posted = QMetaObject::invokeMethod(
                            guard,
                            [guard, batch, drained, completionNow] {
                                if (!guard) return;
                                const bool queued = guard->enqueueAppRuntime(
                                    [guard, batch, drained, completionNow](
                                        AppRuntimeCoordinator &coordinator) {
                                        const AppRuntimeResult result = drained
                                            ? coordinator.authorityDrainCompleted(
                                                  batch.id, completionNow)
                                            : coordinator.authorityDrainTimedOut(
                                                  batch.id, completionNow);
                                        QMetaObject::invokeMethod(
                                            guard,
                                            [guard, result] {
                                                if (guard) {
                                                    guard->handleAppRuntimeResult(result);
                                                }
                                            },
                                            Qt::QueuedConnection);
                                    });
                                if (!queued) {
                                    guard->failClosedAppRuntime(
                                        QStringLiteral(
                                            "host.runtime.critical_event_dropped"));
                                }
                            },
                            Qt::QueuedConnection);
                        if (!posted && guard) {
                            guard->failClosedAppRuntime(
                                QStringLiteral(
                                    "host.runtime.critical_event_dropped"));
                        }
                    }).detach();
                } catch (...) {
                    failClosedAppRuntime(
                        QStringLiteral("host.runtime.drain_executor_unavailable"));
                }
            }
            break;
        case AppRuntimeActionKind::Revoke:
        case AppRuntimeActionKind::RecoverFromLkg:
        case AppRuntimeActionKind::None:
            break;
        }
    }
}

#ifdef Q_BROWSER_HOST_TESTING
void HostApplication::forceLifecycleQueueFullForTesting(const bool full) noexcept
{
    lifecycleQueueFullForTesting_ = full;
}

void HostApplication::forceLifecycleShutdownQueueFailureForTesting() noexcept
{
    forceLifecycleShutdownQueueFailureForTesting_ = true;
}

bool HostApplication::retryWorkerCleanupForTesting()
{
    return WorkerRetirementManager::instance().retryFatal();
}

HostGestureRouter *HostApplication::gestureRouterForTesting() const noexcept
{
    return gestureRouter_.get();
}

void HostApplication::holdNextPackageStartupReplyForTesting()
{
    if (packageStartupTesting_ == nullptr) {
        packageStartupTesting_ =
            std::make_unique<PackageStartupTestingState>();
    }
    packageStartupTesting_->holdNextReply = true;
}

quint64 HostApplication::supersedePackageStartupForTesting(
    const QString &packagePath)
{
    if (packagePath.isEmpty()
        || packageStartupPhase_ != PackageStartupPhase::Verifying
        || packageStartupTesting_ == nullptr) {
        return 0;
    }
    const auto previous = packageStartupTesting_->heldReplies.constFind(
        startupIncarnation_);
    if (previous == packageStartupTesting_->heldReplies.cend()
        || previous->authority == nullptr) {
        return 0;
    }
    const quint64 operationIncarnation = issuePackageOperationIncarnation();
    if (operationIncarnation == 0) return 0;
    const quint64 priorStartupIncarnation = startupIncarnation_;
    const std::shared_ptr<RuntimePackageAuthority> replyAuthority =
        previous->authority;
    startupIncarnation_ = operationIncarnation;
    pendingPackageOperations_.insert(operationIncarnation);
    QPointer<HostApplication> guard(this);
    const bool queued = enqueueAppRuntime(
        [guard, operationIncarnation, replyAuthority,
         packagePath](AppRuntimeCoordinator &coordinator) {
            AppRuntimeResult result = coordinator.installAndActivate(
                packagePath, hostMonotonicNowMs());
            if (!guard) return;
            (void)QMetaObject::invokeMethod(
                guard,
                [guard, operationIncarnation, replyAuthority,
                 result = std::move(result)] {
                    if (guard) {
                        guard->handlePackageOperationResult(
                            PackageOperationKind::StartupInstall,
                            operationIncarnation, result, replyAuthority);
                    }
                },
                Qt::QueuedConnection);
        });
    if (queued) return operationIncarnation;
    pendingPackageOperations_.remove(operationIncarnation);
    startupIncarnation_ = priorStartupIncarnation;
    return 0;
}

bool HostApplication::deliverHeldPackageStartupReplyForTesting(
    const quint64 operationIncarnation)
{
    if (packageStartupTesting_ == nullptr || operationIncarnation == 0
        || !pendingPackageOperations_.contains(operationIncarnation)) {
        return false;
    }
    auto reply = packageStartupTesting_->heldReplies.find(
        operationIncarnation);
    if (reply == packageStartupTesting_->heldReplies.end()) return false;
    const PackageStartupTestingState::HeldReply held = *reply;
    packageStartupTesting_->heldReplies.erase(reply);
    const bool preserveNextHold = packageStartupTesting_->holdNextReply;
    packageStartupTesting_->holdNextReply = false;
    QPointer<HostApplication> guard(this);
    handlePackageOperationResult(
        held.kind, operationIncarnation, held.result, held.authority);
    if (guard && preserveNextHold) {
        guard->packageStartupTesting_->holdNextReply = true;
    }
    return true;
}

void HostApplication::setRestoredRoutePackageIdForTesting(QString packageId)
{
    if (packageStartupTesting_ == nullptr) {
        packageStartupTesting_ =
            std::make_unique<PackageStartupTestingState>();
    }
    packageStartupTesting_->restoredRoutePackageId = std::move(packageId);
}

void HostApplication::setAfterPrepareAppLaunchHookForTesting(
    std::function<void()> hook)
{
    afterPrepareAppLaunchHookForTesting_ = std::move(hook);
}
#endif

bool HostApplication::requestPackageInstall(const QString &packagePath)
{
    return requestPackageInstall(packagePath, {});
}

bool HostApplication::requestPackageInstall(
    const QString &packagePath,
    std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority)
{
    if (packagePath.isEmpty() || packageStartupPhase_
            != PackageStartupPhase::Applied) {
        return false;
    }
    const quint64 operationIncarnation = issuePackageOperationIncarnation();
    if (operationIncarnation == 0) return false;
    pendingPackageOperations_.insert(operationIncarnation);
    if (enqueuePackageOperation(
            PackageOperationKind::Install, operationIncarnation, packagePath,
            std::move(sourceAuthority))) {
        return true;
    }
    pendingPackageOperations_.remove(operationIncarnation);
    emit updateLifecycleFailed(
        QStringLiteral("host.runtime.install_queue_failed"));
    return false;
}

bool HostApplication::requestOfflineStart()
{
    if (packageStartupPhase_ != PackageStartupPhase::Applied) return false;
    const quint64 operationIncarnation = issuePackageOperationIncarnation();
    if (operationIncarnation == 0) return false;
    pendingPackageOperations_.insert(operationIncarnation);
    if (enqueuePackageOperation(PackageOperationKind::Offline,
                                operationIncarnation)) {
        return true;
    }
    pendingPackageOperations_.remove(operationIncarnation);
    emit updateLifecycleFailed(
        QStringLiteral("host.runtime.offline_start_queue_failed"));
    return false;
}

quint64 HostApplication::issuePackageOperationIncarnation() noexcept
{
    if (nextPackageOperationIncarnation_ == 0
        || nextPackageOperationIncarnation_
               == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    return nextPackageOperationIncarnation_++;
}

bool HostApplication::enqueuePackageOperation(
    const PackageOperationKind kind,
    const quint64 operationIncarnation,
    QString packagePath,
    std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority)
{
    if (operationIncarnation == 0
        || !pendingPackageOperations_.contains(operationIncarnation)
        || packageAuthority_ == nullptr) {
        return false;
    }
    const std::shared_ptr<RuntimePackageAuthority> replyAuthority =
        packageAuthority_;
    QPointer<HostApplication> guard(this);
    return enqueuePackageRuntime(
        [guard, kind, operationIncarnation,
         replyAuthority,
         packagePath = std::move(packagePath),
         sourceAuthority = std::move(sourceAuthority)](
            AppRuntimeCoordinator &coordinator) mutable {
            AppRuntimeResult result;
            if (sourceAuthority && !sourceAuthority->revalidate()) {
                result.code = AppRuntimeResultCode::Rejected;
                result.stableError = QStringLiteral(
                    "host.runtime.install_source_authority_changed");
            } else if (kind == PackageOperationKind::StartupInstall
                       || kind == PackageOperationKind::Install) {
                result = coordinator.installAndActivate(
                    packagePath, hostMonotonicNowMs());
            } else {
                result = coordinator.startOffline(hostMonotonicNowMs());
            }
            if (!guard) return;
            (void)QMetaObject::invokeMethod(
                guard,
                [guard, kind, operationIncarnation,
                 replyAuthority,
                 result = std::move(result)] {
                    if (guard) {
                        guard->handlePackageOperationResult(
                            kind, operationIncarnation, result,
                            replyAuthority);
                    }
                },
                Qt::QueuedConnection);
        });
}

void HostApplication::handlePackageOperationResult(
    const PackageOperationKind kind,
    const quint64 operationIncarnation,
    const AppRuntimeResult &result,
    std::shared_ptr<RuntimePackageAuthority> replyAuthority)
{
    if (operationIncarnation == 0
        || !pendingPackageOperations_.contains(operationIncarnation)) {
        return;
    }
#ifdef Q_BROWSER_HOST_TESTING
    if (packageStartupTesting_ != nullptr
        && packageStartupTesting_->heldReplies.contains(
            operationIncarnation)) {
        return;
    }
    const bool startupReply = kind == PackageOperationKind::StartupInstall
        || kind == PackageOperationKind::StartupOffline;
    if (startupReply && packageStartupTesting_ != nullptr
        && packageStartupTesting_->holdNextReply) {
        packageStartupTesting_->holdNextReply = false;
        packageStartupTesting_->heldReplies.insert(
            operationIncarnation,
            PackageStartupTestingState::HeldReply{
                kind, result, std::move(replyAuthority)});
        emit packageStartupReplyHeldForTesting(operationIncarnation);
        return;
    }
#endif
    pendingPackageOperations_.remove(operationIncarnation);
    const bool startup = kind == PackageOperationKind::StartupInstall
        || kind == PackageOperationKind::StartupOffline;
    if (startup
        && (packageStartupPhase_ != PackageStartupPhase::Verifying
            || startupIncarnation_ != operationIncarnation)) {
        return;
    }
    const QString fallbackError = kind == PackageOperationKind::StartupInstall
            || kind == PackageOperationKind::Install
        ? QStringLiteral("host.runtime.install_rejected")
        : QStringLiteral("host.runtime.offline_start_rejected");
    const auto fail = [this, startup, &result, &fallbackError] {
        if (startup) packageStartupPhase_ = PackageStartupPhase::Failed;
        emit updateLifecycleFailed(
            result.stableError.isEmpty() ? fallbackError : result.stableError,
            result.nativeError);
    };
    if (result.code != AppRuntimeResultCode::Applied
        || !result.actions.isEmpty() || !result.verifiedCurrent.has_value()
        || replyAuthority == nullptr
        || !runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package) {
        fail();
        return;
    }
    const VerifiedCurrentPackage &verified = *result.verifiedCurrent;
    if (verified.appId().isEmpty()
        || verified.appId() != runtimeConfig_->appId()
        || verified.version().isEmpty() || verified.digestHex().size() != 64) {
        fail();
        return;
    }

    packageAuthority_ = std::move(replyAuthority);
    verifiedCurrentAppId_ = verified.appId();
    verifiedCurrentVersion_ = verified.version();
    verifiedCurrentDigestHex_ = verified.digestHex();
    if (startup) {
        QPointer<HostApplication> lifetimeGuard(this);
        const bool applied = applyVerifiedPackageStartup(verified);
        if (!lifetimeGuard) return;
        if (!applied) {
            packageAuthority_.reset();
            verifiedCurrentAppId_.clear();
            verifiedCurrentVersion_.clear();
            verifiedCurrentDigestHex_.clear();
            packageStartupPhase_ = PackageStartupPhase::Failed;
            emit updateLifecycleFailed(
                QStringLiteral("host.runtime.session_apply_failed"));
            return;
        }
        packageStartupPhase_ = PackageStartupPhase::Applied;
        if (mainWindow_ != nullptr) mainWindow_->show();
        if (!lifetimeGuard) return;
        recordHostDiagnosticPhase("package-startup-applied");
    }
    emit packageActivationVerified(
        operationIncarnation, verified.appId(), verified.version(),
        verified.digestHex());
}

bool HostApplication::beginPackageStartup()
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || packageStartupPhase_ != PackageStartupPhase::NotStarted) {
        return false;
    }
    const quint64 operationIncarnation = issuePackageOperationIncarnation();
    if (operationIncarnation == 0) {
        packageStartupPhase_ = PackageStartupPhase::Failed;
        emit updateLifecycleFailed(
            QStringLiteral("host.runtime.startup_incarnation_exhausted"));
        return false;
    }
    startupIncarnation_ = operationIncarnation;
    packageStartupPhase_ = PackageStartupPhase::Verifying;
    pendingPackageOperations_.insert(operationIncarnation);
    const bool installing = runtimeConfig_->installPackage().has_value();
    const PackageOperationKind kind = installing
        ? PackageOperationKind::StartupInstall
        : PackageOperationKind::StartupOffline;
    const QString packagePath = installing
        ? *runtimeConfig_->installPackage() : QString{};
    std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority = installing
        ? runtimeConfig_->takeInstallPackageAuthority() : nullptr;
    if (enqueuePackageStartupInitialization(
            kind, operationIncarnation, packagePath,
            std::move(sourceAuthority))) {
        return true;
    }
    pendingPackageOperations_.remove(operationIncarnation);
    packageStartupPhase_ = PackageStartupPhase::Failed;
    emit updateLifecycleFailed(
        QStringLiteral("host.runtime.start_queue_failed"));
    return false;
}

bool HostApplication::enqueuePackageStartupInitialization(
    const PackageOperationKind kind,
    const quint64 operationIncarnation,
    QString packagePath,
    std::shared_ptr<const HostOwnedFileAuthority> sourceAuthority)
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || (kind != PackageOperationKind::StartupInstall
            && kind != PackageOperationKind::StartupOffline)
        || operationIncarnation == 0
        || !pendingPackageOperations_.contains(operationIncarnation)) {
        return false;
    }
    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(
            updateLifecycleRuntime_.data()));
    if (!runtime || !runtime->reserve()) return false;

    InstallPolicy installPolicy;
    installPolicy.expectedAppId = runtimeConfig_->appId();
    installPolicy.runtimeVersion = QStringLiteral("1.2.0");
    installPolicy.allowedImports = {QStringLiteral("QtQuick"),
                                    QStringLiteral("QtQuick.Layouts"),
                                    QStringLiteral("Company.Design")};
    installPolicy.preflight = [](const Manifest &, const QString &) {
        return true;
    };
    QPointer<HostApplication> guard(this);
    const bool queued = QMetaObject::invokeMethod(
        runtime,
        [runtime, guard, kind, operationIncarnation,
         appId = runtimeConfig_->appId(),
         packageStoreRoot = runtimeConfig_->packageStoreRoot(),
         trustedPublicKeyPem = runtimeConfig_->trustedPublicKeyPem(),
         installPolicy = std::move(installPolicy),
         supervisionPolicy = WorkerSupervisionPolicy{
             runtimeConfig_->healthWindowMs(),
             runtimeConfig_->heartbeatTimeoutMs()},
         telemetryDirectory = runtimeConfig_->telemetryDirectory(),
         packagePath = std::move(packagePath),
         sourceAuthority = std::move(sourceAuthority)]() mutable {
            struct PendingRelease final
            {
                QPointer<HostLifecycleRuntime> runtime;
                ~PendingRelease()
                {
                    if (runtime) runtime->release();
                }
            } releaseOnExit{runtime};
            AppRuntimeResult result;
            std::shared_ptr<RuntimePackageAuthority> replyAuthority;
            if (sourceAuthority && !sourceAuthority->revalidate()) {
                result.code = AppRuntimeResultCode::Rejected;
                result.stableError = QStringLiteral(
                    "host.runtime.install_source_authority_changed");
            } else if (!runtime
                       || !runtime->initialize(
                           std::move(appId), std::move(packageStoreRoot),
                           std::move(trustedPublicKeyPem),
                           std::move(installPolicy), supervisionPolicy,
                           std::move(telemetryDirectory))) {
                result.code = AppRuntimeResultCode::FailedClosed;
                result.stableError = QStringLiteral(
                    "host.runtime.initialization_failed");
            } else {
                replyAuthority = runtime->authority();
                result = runtime->executeStartupOperation(
                    kind == PackageOperationKind::StartupInstall,
                    packagePath, hostMonotonicNowMs());
            }
            if (!guard) return;
            (void)QMetaObject::invokeMethod(
                guard,
                [guard, kind, operationIncarnation,
                 result = std::move(result),
                 replyAuthority = std::move(replyAuthority)]() mutable {
                    if (guard) {
                        guard->handlePackageOperationResult(
                            kind, operationIncarnation, result,
                            std::move(replyAuthority));
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (!queued && runtime) runtime->release();
    return queued;
}

bool HostApplication::applyVerifiedPackageStartup(
    const VerifiedCurrentPackage &verified)
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || runtimeConfig_->appId() != verified.appId()
        || verifiedCurrentAppId_ != verified.appId()
        || mainWindow_ == nullptr || mainWindow_->isVisible()
        || mainWindow_->tabModel() == nullptr
        || !mainWindow_->tabModel()->isEmpty()
        || browserSessionStore_ == nullptr
        || !pendingBrowserSessionLoad_.has_value()) {
        return false;
    }
    QString routePackageId = verified.appId();
#ifdef Q_BROWSER_HOST_TESTING
    if (packageStartupTesting_ != nullptr
        && !packageStartupTesting_->restoredRoutePackageId.isEmpty()) {
        routePackageId = packageStartupTesting_->restoredRoutePackageId;
    }
#endif
    std::optional<RouteRegistry> currentRoutes = createPilotRouteRegistry(
        mockOrigin_, routePackageId);
    if (!currentRoutes.has_value()) return false;
    const QString verifiedAppId = verified.appId();
    const QString configuredAppId = runtimeConfig_->appId();
    RestoredAddressResolver resolver =
        [routes = std::move(*currentRoutes), verifiedAppId,
         configuredAppId](const BrowserAddress &untrusted)
            -> std::optional<BrowserTabKind> {
            const QString canonical = untrusted.canonical();
            const BrowserAddress trusted = BrowserAddress::parse(
                canonical, QStringLiteral("pilot"));
            if (!trusted.isValid() || trusted.canonical() != canonical) {
                return std::nullopt;
            }
            if (trusted.kind() == BrowserAddressKind::NewTab) {
                return BrowserTabKind::Host;
            }
            if (trusted.kind() != BrowserAddressKind::App) {
                return std::nullopt;
            }
            const RouteMatch matched = routes.match(trusted.appPath());
            if (!matched.isValid()) return std::nullopt;
            if (matched.record.engine == Engine::WebEngine) {
                return BrowserTabKind::Web;
            }
            if (matched.record.engine == Engine::QmlWorker
                && configuredAppId == verifiedAppId
                && matched.record.packageId == verifiedAppId) {
                return BrowserTabKind::App;
            }
            return std::nullopt;
        };

    QList<QRect> availableGeometries;
    const QList<QScreen *> screens = QGuiApplication::screens();
    availableGeometries.reserve(screens.size());
    for (const QScreen *const screen : screens) {
        if (screen != nullptr) {
            availableGeometries.append(screen->availableGeometry());
        }
    }
    const QScreen *const primaryScreen = QGuiApplication::primaryScreen();
    const QRect primaryGeometry = primaryScreen != nullptr
        ? primaryScreen->availableGeometry()
        : QRect(0, 0, 1280, 800);
    if (availableGeometries.isEmpty()) {
        availableGeometries.append(primaryGeometry);
    }
    const std::shared_ptr<BrowserSessionStore> durableStore =
        browserSessionStore_;
    const BrowserSessionSaveCallback save =
        [durableStore](const BrowserWindowSnapshot &snapshot) {
            return durableStore->save(snapshot);
        };
    QPointer<HostApplication> lifetimeGuard(this);
    const bool applied = mainWindow_->applyBrowserSessionLoadResult(
        *pendingBrowserSessionLoad_, resolver, primaryGeometry,
        availableGeometries, save);
    if (!lifetimeGuard) return false;
    if (applied) pendingBrowserSessionLoad_.reset();
    return applied;
}

bool HostApplication::initializePackageRuntime()
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() == HostRuntimeMode::TrustedShell) return true;
    QPointer<HostApplication> guard(this);
    auto *const runtime = new HostLifecycleRuntime(
        [guard](const QString &stableError, const quint32 nativeError) {
            if (!guard) return;
            (void)QMetaObject::invokeMethod(
                guard,
                [guard, stableError, nativeError] {
                    if (guard) {
                        emit guard->updateLifecycleFailed(
                            stableError, nativeError);
                    }
                }, Qt::QueuedConnection);
        });
    auto *const lifecycleThread = new QThread;
    lifecycleThread->setObjectName(QStringLiteral("host-update-lifecycle"));
    runtime->moveToThread(lifecycleThread);
    connect(lifecycleThread, &QThread::finished, runtime,
            [runtime] { runtime->shutdownOnThreadExit(); },
            Qt::DirectConnection);
    connect(lifecycleThread, &QThread::finished, runtime, &QObject::deleteLater);
    connect(lifecycleThread, &QThread::finished,
            lifecycleThread, &QObject::deleteLater);
    updateLifecycleRuntime_ = runtime;
    updateLifecycleThread_ = lifecycleThread;
    lifecycleThread->start();
    return true;
}

bool HostApplication::start()
{
    recordHostDiagnosticPhase("start-enter");
    const bool packageMode = runtimeConfig_.has_value()
        && runtimeConfig_->mode() == HostRuntimeMode::Package;
    if (mainWindow_) {
        if (!mainWindow_->hasValidWebSession()) {
            recordHostDiagnosticPhase("start-existing-window-retired");
            return false;
        }
        if (packageMode) {
            if (packageStartupPhase_ == PackageStartupPhase::Verifying) {
                recordHostDiagnosticPhase("start-existing-window-verifying");
                return true;
            }
            if (packageStartupPhase_ != PackageStartupPhase::Applied) {
                recordHostDiagnosticPhase("start-existing-window-failed");
                return false;
            }
        }
        mainWindow_->show();
        recordHostDiagnosticPhase("start-existing-window-shown");
        return true;
    }
    const QString routeAppId = runtimeConfig_.has_value()
            && runtimeConfig_->mode() == HostRuntimeMode::Package
            && !runtimeConfig_->appId().isEmpty()
        ? runtimeConfig_->appId() : QStringLiteral("com.qbrowser.pilot");
    auto routes = createPilotRouteRegistry(mockOrigin_, routeAppId);
    if (!routes.has_value()) return false;
    recordHostDiagnosticPhase("start-routes-ready");
    if (packageMode) {
        if (runtimeConfig_->browserStateAuthority() == nullptr) return false;
        browserSessionStore_ = std::make_shared<BrowserSessionStore>(
            runtimeConfig_->browserStateAuthority());
        pendingBrowserSessionLoad_ = browserSessionStore_->load();
    }
    auto window = packageMode
        ? std::make_unique<MainWindow>(
              std::move(*routes), mockOrigin_,
              MainWindowInitialState::DeferredSession)
        : std::make_unique<MainWindow>(std::move(*routes), mockOrigin_);
    recordHostDiagnosticPhase("start-main-window-created");
    if (!window->hasValidWebSession()) {
        return false;
    }
    window->setPackageRuntimeEnabled(packageMode);
    connect(window.get(), &MainWindow::appLaunchRequested, this,
            [this](const QString &tabId, const quint64 navigationIncarnation,
                   const QString &packageId, const QString &route) {
                handleAppLaunchRequested(tabId, navigationIncarnation,
                                         packageId, route);
            },
            Qt::DirectConnection);
    connect(window.get(), &MainWindow::appReloadRequested, this,
            [this](const QString &tabId, const quint64 navigationIncarnation,
                   const QString &packageId, const QString &route) {
                handleAppLaunchRequested(tabId, navigationIncarnation,
                                         packageId, route, true);
            },
            Qt::DirectConnection);
    connect(window.get(), &MainWindow::appStopRequested, this,
            [this](const QString &tabId, const quint64 navigationIncarnation,
                   const quint64 runtimeIncarnation) {
                handleAppStopRequested(tabId, navigationIncarnation,
                                        runtimeIncarnation);
            },
            Qt::DirectConnection);
    connect(window.get(), &MainWindow::tabClosing, this,
            [this](const QString &tabId, const quint64) {
                if (mainWindow_ == nullptr) return;
                const auto incarnation = appRuntimeIncarnations_.find(tabId);
                pendingAppNavigationIncarnations_.remove(tabId);
                if (incarnation == appRuntimeIncarnations_.end()) return;
                const TabLaunchAuthority authority{tabId, *incarnation};
                appRuntimeIncarnations_.erase(incarnation);
                QPointer<HostApplication> guard(this);
                const bool queued = enqueueAppRuntime(
                    [guard, authority](AppRuntimeCoordinator &coordinator) {
                        const AppRuntimeResult result =
                            coordinator.closeTab(authority, -1);
                        if (!result.actions.isEmpty()
                            || result.code != AppRuntimeResultCode::Applied) {
                            QMetaObject::invokeMethod(
                                guard,
                                [guard, result] {
                                    if (guard) guard->handleAppRuntimeResult(result);
                                },
                                Qt::QueuedConnection);
                        }
                    });
                if (!queued) {
                    failClosedAppRuntime(
                        QStringLiteral("host.runtime.critical_event_dropped"));
                }
            },
            Qt::DirectConnection);
    window->resize(1100, 720);
    mainWindow_ = std::move(window);
    gestureRouter_ = std::make_unique<HostGestureRouter>(
        static_cast<quintptr>(mainWindow_->winId()));
    mainWindow_->installEventFilter(this);
    connect(gestureRouter_.get(), &HostGestureRouter::browserCommandRequested,
            mainWindow_->browserChrome(), &BrowserChrome::dispatchCommand);
    connect(mainWindow_->tabModel(), &BrowserTabModel::activeTabChanged,
            this, [this] {
                if (workerSessionController_ != nullptr
                    && mainWindow_ != nullptr
                    && workerSessionController_->state()
                           == HostWorkerSessionState::Running) {
                    const QString owner = mainWindow_->tabModel() != nullptr
                        ? mainWindow_->tabModel()->activeId() : QString{};
                    const bool active = !owner.isEmpty()
                        && owner == (capabilityRuntime_ != nullptr
                                         ? capabilityRuntime_->authority().tabId
                                         : QString{});
                    (void)workerSessionController_->sendVisibilityChanged(active);
                }
                synchronizeGestureAuthority();
            },
            Qt::DirectConnection);
    connect(mainWindow_.get(), &MainWindow::currentUrlChanged, this,
            [this](const QString &) {
                synchronizeGestureAuthority();
                if (workerSessionController_ == nullptr
                    || mainWindow_ == nullptr
                    || workerSessionController_->state()
                           != HostWorkerSessionState::Running) {
                    return;
                }
                const QString owner = capabilityRuntime_ != nullptr
                    ? capabilityRuntime_->authority().tabId : QString{};
                if (owner.isEmpty()
                    || mainWindow_->tabModel() == nullptr
                    || mainWindow_->tabModel()->activeId() != owner) {
                    (void)workerSessionController_->sendVisibilityChanged(false);
                    return;
                }
                (void)workerSessionController_->sendVisibilityChanged(
                    mainWindow_->activeSurface() == HostSurfaceKind::Worker);
            },
            Qt::DirectConnection);
    if (QGuiApplication *const application =
            qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(application, &QGuiApplication::applicationStateChanged,
                this, [this] { synchronizeGestureAuthority(); },
                Qt::DirectConnection);
    }
    connect(mainWindow_.get(), &MainWindow::legacyWorkerRetirementRequested,
            this, [this](const QString &) {
                detachWorkerContext(
                    QStringLiteral("host.worker_context.tab_retired"));
            }, Qt::DirectConnection);
    if (!packageMode) {
        workerSessionController_ =
            std::make_unique<HostWorkerSessionController>(mainWindow_.get());
        recordHostDiagnosticPhase("start-session-controller-ready");
    }
    if (!initializePackageRuntime()) return false;
    recordHostDiagnosticPhase("start-package-runtime-ready");

    if (workerSessionController_ != nullptr) {
        connect(workerSessionController_.get(),
                &HostWorkerSessionController::failed, this, [this] {
                const std::optional<WorkerAttemptKey> failedKey = attachedWorkerKey_;
                detachWorkerContext(QStringLiteral("host.worker_session.failed"));
                if (failedKey.has_value()) {
                    const bool queued = enqueueLifecycle(
                        [failedKey](UpdateLifecycleCoordinator &coordinator) {
                            (void)coordinator.workerExited(
                                *failedKey, WorkerExitReason::Crashed);
                        });
                    if (!queued) {
                        failClosedAppRuntime(
                            QStringLiteral("host.runtime.critical_event_dropped"));
                    }
                }
            });
        connect(workerSessionController_.get(),
                &HostWorkerSessionController::heartbeatObserved,
                this, [this] {
                if (!attachedWorkerKey_.has_value()) return;
                const WorkerAttemptKey key = *attachedWorkerKey_;
                (void)enqueueLifecycle([key](UpdateLifecycleCoordinator &coordinator) {
                    (void)coordinator.heartbeat(key);
                });
            });
        connect(workerSessionController_.get(),
                &HostWorkerSessionController::routeLoadAcknowledged,
                this, [this](const QString &route) {
                const QString routeTemplate = pilotRouteTemplate(route);
                if (routeTemplate.isEmpty()) return;
                const qsizetype pending = workerSessionController_ != nullptr
                    ? workerSessionController_->pendingRouteLoadCount() : -1;
                (void)enqueueLifecycle(
                    [routeTemplate, pending](UpdateLifecycleCoordinator &coordinator) {
                        coordinator.recordRouteLoadAcknowledged(routeTemplate, pending);
                    });
            });
        connect(workerSessionController_.get(),
                &HostWorkerSessionController::capabilityRequestObserved,
                this, &HostApplication::workerCapabilityRequestObserved);
    }
    updateHealthTimer_ = std::make_unique<QTimer>();
    updateHealthTimer_->setInterval(50);
    connect(updateHealthTimer_.get(), &QTimer::timeout, this, [this] {
        synchronizeGestureAuthority();
        if (attachedWorkerKey_.has_value()) {
            const WorkerAttemptKey key = *attachedWorkerKey_;
            (void)enqueueLifecycle([key](UpdateLifecycleCoordinator &coordinator) {
                (void)coordinator.checkHealth(key);
            });
        }
        if (!appTabRuntimeControllers_.isEmpty()) {
            bool expectedPending = false;
            if (!appHealthCheckPending_.compare_exchange_strong(
                    expectedPending, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
            QPointer<HostApplication> guard(this);
            const qint64 healthNowMs = hostMonotonicNowMs();
            const bool queued = enqueueAppRuntime(
                [guard, healthNowMs](AppRuntimeCoordinator &coordinator) {
                    const AppRuntimeResult result = coordinator.checkHealth(
                        healthNowMs);
                    if (guard == nullptr) return;
                    const bool posted = QMetaObject::invokeMethod(
                        guard,
                        [guard, result] {
                            if (!guard) return;
                            guard->appHealthCheckPending_.store(
                                false, std::memory_order_release);
                            if (!result.actions.isEmpty()
                                || result.code != AppRuntimeResultCode::Applied) {
                                guard->handleAppRuntimeResult(result);
                            }
                        },
                        Qt::QueuedConnection);
                    if (!posted) {
                        guard->appHealthCheckPending_.store(
                            false, std::memory_order_release);
                    }
                });
            if (!queued) {
                appHealthCheckPending_.store(false, std::memory_order_release);
            }
        }
    });
    updateHealthTimer_->start();
    recordHostDiagnosticPhase("start-health-timer-ready");

    if (packageMode) {
        if (!beginPackageStartup()) return false;
        recordHostDiagnosticPhase("start-package-operation-queued");
    } else {
        mainWindow_->show();
        recordHostDiagnosticPhase("start-main-window-shown");
    }
    recordHostDiagnosticPhase("start-complete");
    return true;
}

bool HostApplication::eventFilter(QObject *const watched, QEvent *const event)
{
    if (watched == mainWindow_.get() && event != nullptr
        && gestureRouter_ != nullptr) {
        switch (event->type()) {
        case QEvent::WindowDeactivate:
        case QEvent::Hide:
        case QEvent::Close:
            gestureRouter_->hostDeactivated();
            break;
        case QEvent::WindowActivate:
            synchronizeGestureAuthority();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

bool HostApplication::attachWorkerSession(std::unique_ptr<IpcSession> session)
{
    return workerSessionController_ != nullptr
        && workerSessionController_->attach(std::move(session));
}

InstalledPackageWorkerLauncher::AttachResult
HostApplication::attachWorkerContext(
    InstalledPackageWorkerLauncher::CommittedAttachTransaction transaction)
{
    WorkerAttachContext context;
    context.launchRequest = transaction.request();
    context.processId = transaction.processId();
    const std::shared_ptr<SandboxProcess> process = transaction.takeProcess();
    context.session = transaction.takeSession();
    context.surface = transaction.takeSurface();
    context.processLifetime = std::static_pointer_cast<void>(process);
    context.stopProcess = [process] {
        if (process != nullptr)
            process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    };
    return realizeWorkerContext(std::move(context));
}

#ifdef Q_BROWSER_HOST_TESTING
InstalledPackageWorkerLauncher::AttachResult
HostApplication::attachWorkerContextForTesting(HostWorkerAttachContext supplied)
{
    WorkerAttachContext context;
    context.session = std::move(supplied.session);
    context.surface = std::move(supplied.surface);
    context.processLifetime = std::move(supplied.processLifetime);
    context.launchRequest = std::move(supplied.launchRequest);
    context.processId = supplied.processId;
    context.stopProcess = std::move(supplied.stopProcess);
    return realizeWorkerContext(std::move(context));
}
#endif

InstalledPackageWorkerLauncher::AttachResult
HostApplication::realizeWorkerContext(WorkerAttachContext context)
{
    const WorkerLaunchRequest &request = context.launchRequest;
    if (mainWindow_ == nullptr || workerSessionController_ == nullptr
        || gestureRouter_ == nullptr
        || fileDialogCoordinator_ == nullptr
        || context.session == nullptr || context.surface == nullptr
        || context.processLifetime == nullptr || !context.stopProcess
        || workerProcessLifetime_ != nullptr || pendingWorkerAttach_ != nullptr
        || !runtimeConfig_.has_value()
        || runtimeConfig_->mode() != HostRuntimeMode::Package
        || request.admission == nullptr || request.tabId.isEmpty()
        || request.runtimeIncarnation == 0
        || request.lease.appId != runtimeConfig_->appId()
        || request.lease.leaseAuthorityEpoch == 0
        || request.attempt.activation.value == 0
        || request.attempt.attempt.value == 0
        || context.session->appIdentity() != request.lease.appId)
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    BrowserTabModel *const tabModel = mainWindow_->tabModel();
    const QString tabId = tabModel != nullptr ? tabModel->activeId() : QString{};
    TabController *const owningTab = mainWindow_->tabController(tabId);
    if (tabId.isEmpty() || tabId != request.tabId || owningTab == nullptr
        || workerSessionController_->generation()
               == std::numeric_limits<quint64>::max()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    const quintptr workerWindowId = static_cast<quintptr>(
        context.surface->nativeWindowId());
    const TabCapabilityAuthority authority{
        request.tabId,
        request.runtimeIncarnation,
        request.lease.appId,
        context.processId,
        workerWindowId,
        workerSessionController_->generation() + 1,
        request.lease.leaseAuthorityEpoch};
    if (!authority.isValid()) {
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    QString capabilityError;
    auto capabilityRuntime = HostCapabilityRuntime::create(
        authority, request.admission, gestureRouter_.get(),
        request.lease.permissions, runtimeConfig_->mockOrigin(),
        runtimeConfig_->storageDirectory(),
        static_cast<quintptr>(mainWindow_->winId()), &capabilityError,
        fileDialogCoordinator_.get());
    if (capabilityRuntime == nullptr) {
        emit updateLifecycleFailed(
            capabilityError.isEmpty()
                ? QStringLiteral("host.capability.initialization_failed")
                : capabilityError);
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    context.capability = std::move(capabilityRuntime);
    if (context.capability->isWorkerInitializationComplete()) {
        if (!context.capability->isWorkerReady()) {
            emit updateLifecycleFailed(
                context.capability->workerInitializationError().isEmpty()
                    ? QStringLiteral("host.capability.initialization_failed")
                    : context.capability->workerInitializationError());
            HostCapabilityRuntime::retire(std::move(context.capability));
            return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
        }
        if (!attachInitializedWorkerContext(std::move(context))) {
            return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
        }
        return InstalledPackageWorkerLauncher::AttachResult::Attached;
    }

    HostCapabilityRuntime *const pendingCapability = context.capability.get();
    pendingWorkerAttach_ = std::make_unique<WorkerAttachContext>(
        std::move(context));
    connect(
        pendingCapability,
        &HostCapabilityRuntime::workerInitializationFinished,
        this,
        [this, pendingCapability](const bool, const QString &) {
            if (pendingWorkerAttach_ != nullptr
                && pendingWorkerAttach_->capability.get()
                    == pendingCapability) {
                completePendingWorkerContext();
            }
        });
    return InstalledPackageWorkerLauncher::AttachResult::Attached;
}

bool HostApplication::attachInitializedWorkerContext(
    WorkerAttachContext context)
{
    bool surfaceAttached = false;
    const auto rollback = [this, &context, &surfaceAttached] {
        if (surfaceAttached && mainWindow_ != nullptr) {
            mainWindow_->detachWorkerSurface();
        }
        if (context.stopProcess) {
            context.stopProcess();
        }
        HostCapabilityRuntime::retire(std::move(context.capability));
        return false;
    };
    const WorkerLaunchRequest &request = context.launchRequest;
    BrowserTabModel *const tabModel = mainWindow_ != nullptr
        ? mainWindow_->tabModel() : nullptr;
    const QString tabId = tabModel != nullptr ? tabModel->activeId() : QString{};
    TabController *const owningTab = mainWindow_ != nullptr
        ? mainWindow_->tabController(tabId) : nullptr;
    if (mainWindow_ == nullptr || workerSessionController_ == nullptr
        || context.session == nullptr || context.surface == nullptr
        || context.processLifetime == nullptr || !context.stopProcess
        || context.capability == nullptr || !context.capability->isWorkerReady()
        || workerProcessLifetime_ != nullptr || tabId.isEmpty()
        || tabId != request.tabId || owningTab == nullptr
        || context.capability->authority().sessionGeneration
            != workerSessionController_->generation() + 1) {
        return rollback();
    }

    WorkerSurface *const surface = context.surface.get();
    if (!mainWindow_->attachWorkerSurface(std::move(context.surface))) {
        return rollback();
    }
    surfaceAttached = true;
    if (mainWindow_->tabModel()->activeId() != tabId
        || mainWindow_->tabController(tabId) != owningTab
        || !workerSessionController_->attach(
            std::move(context.session), context.capability.get())) {
        return rollback();
    }
    capabilityRuntime_ = std::move(context.capability);
    workerProcessLifetime_ = std::move(context.processLifetime);
    stopWorkerProcess_ = std::move(context.stopProcess);
    attachedWorkerKey_ = request.attempt;
    synchronizeGestureAuthority();
    Q_ASSERT(mainWindow_->workerSurface() == surface);
    return true;
}

void HostApplication::completePendingWorkerContext()
{
    if (pendingWorkerAttach_ == nullptr
        || pendingWorkerAttach_->capability == nullptr
        || !pendingWorkerAttach_->capability
                ->isWorkerInitializationComplete()) {
        return;
    }
    WorkerAttachContext context = std::move(*pendingWorkerAttach_);
    pendingWorkerAttach_.reset();
    const WorkerLaunchRequest request = context.launchRequest;
    const quint32 processId = context.processId;
    if (!context.capability->isWorkerReady()) {
        const QString errorCode =
            context.capability->workerInitializationError().isEmpty()
            ? QStringLiteral("host.capability.initialization_failed")
            : context.capability->workerInitializationError();
        if (context.stopProcess) {
            context.stopProcess();
        }
        HostCapabilityRuntime::retire(std::move(context.capability));
        emit updateLifecycleFailed(errorCode);
        return;
    }
    if (!attachInitializedWorkerContext(std::move(context))) {
        emit updateLifecycleFailed(
            QStringLiteral("host.capability.initialization_failed"));
        return;
    }
    emit packageWorkerReady(
        request.lease.appId, request.lease.version,
        request.lease.packageDirectory, request.attempt.activation.value,
        request.attempt.attempt.value, processId);
}

void HostApplication::clearPendingWorkerContext() noexcept
{
    if (pendingWorkerAttach_ == nullptr) {
        return;
    }
    if (pendingWorkerAttach_->stopProcess) {
        pendingWorkerAttach_->stopProcess();
    }
    HostCapabilityRuntime::retire(
        std::move(pendingWorkerAttach_->capability));
    pendingWorkerAttach_.reset();
}

void HostApplication::detachWorkerContext(const QString &reason)
{
    clearPendingWorkerContext();
    if (workerProcessLifetime_ == nullptr) return;
    if (workerSessionController_ != nullptr
        && workerSessionController_->state() == HostWorkerSessionState::Running) {
        (void)workerSessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker_context.detached") : reason);
    }
    if (stopWorkerProcess_) stopWorkerProcess_();
    if (workerSessionController_ != nullptr && capabilityRuntime_ != nullptr) {
        workerSessionController_->unbindCapabilityRuntime(
            capabilityRuntime_->authority());
    }
    HostCapabilityRuntime::retire(std::exchange(capabilityRuntime_, {}));
    if (mainWindow_ != nullptr) mainWindow_->detachWorkerSurface();
    stopWorkerProcess_ = {};
    workerProcessLifetime_.reset();
    attachedWorkerKey_.reset();
}

void HostApplication::synchronizeGestureAuthority()
{
    if (gestureRouter_ == nullptr) return;
    HostCapabilityRuntime *activeCapability = capabilityRuntime_.get();
    HostWorkerSessionController *activeSession =
        workerSessionController_.get();
    QString activeTabId;
    if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr) {
        activeTabId = mainWindow_->tabModel()->activeId();
        if (TabController *const activeTab =
                mainWindow_->tabController(activeTabId);
            activeTab != nullptr && activeTab->appRuntimeController() != nullptr) {
            if (activeTab->appRuntimeController()->capabilityRuntime() != nullptr
                && activeTab->surfaceKind() == HostSurfaceKind::Worker
                && activeTab->currentSurface() == activeTab->workerSurface()) {
                activeCapability =
                    activeTab->appRuntimeController()->capabilityRuntime();
                activeSession =
                    activeTab->appRuntimeController()->sessionController();
            } else {
                // A per-tab runtime may remain alive while its tab shows Web,
                // New Tab, or a trusted error.  Never reuse a compatibility
                // capability from another surface in that state.
                activeCapability = nullptr;
                activeSession = nullptr;
            }
        }
    }
    if (activeCapability == nullptr || mainWindow_ == nullptr
        || mainWindow_->tabModel() == nullptr
        || QGuiApplication::applicationState() != Qt::ApplicationActive
        || activeTabId != activeCapability->authority().tabId) {
        gestureRouter_->hostDeactivated();
        return;
    }
    const TabCapabilityAuthority &authority = activeCapability->authority();
    if (activeSession == nullptr
        || activeSession->state() != HostWorkerSessionState::Running
        || activeSession->generation() != authority.sessionGeneration) {
        gestureRouter_->hostDeactivated();
        return;
    }
    if (!gestureRouter_->activateBinding(authority)) {
        gestureRouter_->hostDeactivated();
    }
}

bool HostApplication::hasWorkerContext() const noexcept
{
    if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr) {
        const QString activeId = mainWindow_->tabModel()->activeId();
        if (TabController *const active = mainWindow_->tabController(activeId);
            active != nullptr && active->appRuntimeController() != nullptr) {
            return active->appRuntimeController()->hasWorkerContext();
        }
    }
    return workerProcessLifetime_ != nullptr;
}

MainWindow *HostApplication::mainWindow() const noexcept
{
    return mainWindow_.get();
}

HostWorkerSessionController *HostApplication::workerSessionController() const noexcept
{
    if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr) {
        if (TabController *const activeTab = mainWindow_->tabController(
                mainWindow_->tabModel()->activeId());
            activeTab != nullptr && activeTab->appRuntimeController() != nullptr) {
            return activeTab->appRuntimeController()->sessionController();
        }
    }
    return workerSessionController_.get();
}
