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

    HostLifecycleRuntime(std::shared_ptr<RuntimePackageAuthority> authority,
                         std::unique_ptr<EventRecorder> recorder,
                         std::unique_ptr<UpdateLifecycleCoordinator> coordinator,
                         std::unique_ptr<AppRuntimeCoordinator> appCoordinator,
                         FailurePublisher publishFailure)
        : authority_(std::move(authority))
        , recorder_(std::move(recorder))
        , coordinator_(std::move(coordinator))
        , appCoordinator_(std::move(appCoordinator))
        , publishFailure_(std::move(publishFailure))
        , shutdownHandoff_(
              std::make_shared<HostLifecycleShutdownHandoff>())
    {
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
            if (coordinator_ == nullptr) {
                if (shutdownHandoffRegistered_.load(
                        std::memory_order_acquire)) {
                    shutdownHandoff_->complete(std::nullopt);
                }
                return true;
            }
            if (appCoordinator_ != nullptr) {
                (void)appCoordinator_->beginShutdown();
            }
            UpdateLifecycleShutdownResult result =
                coordinator_->beginHostShutdown();
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
                    const AppRuntimeResult result =
                        coordinator.admitAuthenticatedWorker(request);
                    const bool accepted =
                        result.code == AppRuntimeResultCode::Applied;
                    const bool ignoredStale =
                        result.code == AppRuntimeResultCode::IgnoredStale;
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
                        || result.code != AppRuntimeResultCode::Applied) {
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, result] {
                                if (guard) guard->handleAppRuntimeResult(result);
                            },
                            Qt::QueuedConnection);
                    }
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
    connect(raw, &AppTabRuntimeController::ready, this,
            [this](const WorkerLaunchRequest &request, const quint32 processId) {
                emit packageWorkerReadyForTab(
                    request.tabId, request.runtimeIncarnation,
                    request.lease.appId, request.lease.version,
                    request.lease.packageDirectory,
                    request.attempt.activation.value,
                    request.attempt.attempt.value, processId,
                    request.lease.leaseAuthorityEpoch);
                if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr
                    && mainWindow_->tabModel()->activeId() == request.tabId) {
                    emit packageWorkerReady(
                        request.lease.appId, request.lease.version,
                        request.lease.packageDirectory,
                        request.attempt.activation.value,
                        request.attempt.attempt.value, processId);
                }
            },
            Qt::DirectConnection);
    connect(raw, &AppTabRuntimeController::workerExited, this,
            [this](const WorkerLaunchRequest &request, const bool expected) {
                emit packageWorkerExitedForTab(
                    request.tabId, request.runtimeIncarnation,
                    request.attempt.activation.value,
                    request.attempt.attempt.value,
                    request.lease.leaseAuthorityEpoch, expected);
                if (!expected) {
                    if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr
                        && mainWindow_->tabModel()->activeId() == request.tabId) {
                        emit packageWorkerExited(
                            request.attempt.activation.value,
                            request.attempt.attempt.value);
                    }
                }
                const WorkerExitReason reason = expected
                    ? WorkerExitReason::Clean : WorkerExitReason::Crashed;
                const FullAttemptKey key{
                    {request.tabId, request.runtimeIncarnation}, request.attempt,
                    request.lease.leaseAuthorityEpoch};
                QPointer<HostApplication> guard(this);
                const bool queued = enqueueAppRuntime(
                    [guard, key, reason](AppRuntimeCoordinator &coordinator) {
                        const AppRuntimeResult result =
                            coordinator.workerExited(key, reason, -1);
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
                const bool queued = enqueueAppRuntime(
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
                if (!queued) {
                    failClosedAppRuntime(
                        QStringLiteral("host.runtime.critical_event_dropped"));
                }
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
            [this](const WorkerLaunchRequest &request, const QString &,
                   const quint64) {
                if (mainWindow_ == nullptr || mainWindow_->tabModel() == nullptr
                    || mainWindow_->tabModel()->indexOfId(request.tabId) < 0) {
                    return;
                }
                (void)mainWindow_->tabModel()->setLoadState(
                    request.tabId, false, 100);
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
            [this](const WorkerLaunchRequest &request, const QString &capability,
                   const QString &operation, const QVariantMap &payload,
                   const quint64 sessionGeneration) {
                emit workerCapabilityRequestObservedForTab(
                    request.tabId, request.runtimeIncarnation,
                    sessionGeneration, capability, operation, payload,
                    request.lease.leaseAuthorityEpoch);
                if (mainWindow_ != nullptr && mainWindow_->tabModel() != nullptr
                    && mainWindow_->tabModel()->activeId() == request.tabId) {
                    emit workerCapabilityRequestObserved(capability, operation,
                                                         payload);
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
    const quint64 runtimeIncarnation = reload
        ? advanceRuntimeIncarnationForTab(tabId)
        : runtimeIncarnationForTab(tabId);
    if (runtimeIncarnation == 0) {
        tab->showTrustedErrorForNavigation(
            navigationIncarnation,
            QStringLiteral("The package runtime is unavailable."));
        return;
    }
    pendingAppNavigationIncarnations_[tabId] = navigationIncarnation;
    const TabLaunchAuthority authority{tabId, runtimeIncarnation};
    QPointer<HostApplication> guard(this);
    const bool queued = enqueueAppRuntime(
        [guard, authority, route, tabId, navigationIncarnation](
            AppRuntimeCoordinator &coordinator) {
            const AppRuntimeResult result = coordinator.requestTabLaunch(
                authority, route, TabLaunchIntent::ActivateCurrent, -1);
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

void HostApplication::failClosedAppRuntime(const QString &stableError)
{
    const QString error = stableError.isEmpty()
        ? QStringLiteral("host.runtime.critical_event_dropped")
        : stableError;
    if (appRuntimeFailedClosed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    acceptingLifecycle_.store(false, std::memory_order_release);
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
        [guard, error] {
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
            guard->handleAppRuntimeFailure(error);
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
        const bool actionTargetsCurrentRuntime =
            action.runtimeIncarnation != 0
            && expectedRuntime == action.runtimeIncarnation
            && (action.kind == AppRuntimeActionKind::Launch
                || currentRuntime == 0
                || currentRuntime == action.runtimeIncarnation);
        if (!actionTargetsCurrentRuntime && action.kind != AppRuntimeActionKind::AwaitAuthorityDrain) {
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
                handleAppRuntimeFailure(
                    QStringLiteral("host.runtime.app_launch_rejected"));
                break;
            }
            const quint64 navigationIncarnation =
                pendingAppNavigationIncarnations_.value(
                    action.tabId, tab->incarnation());
            if (navigationIncarnation == 0
                || navigationIncarnation != tab->incarnation()
                || !tab->prepareAppLaunch(action.launch->lease.appId,
                                           navigationIncarnation)
                || !runtime->requestLaunch(*action.launch)) {
                handleAppRuntimeFailure(
                    QStringLiteral("host.runtime.app_launch_rejected"));
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
    return installedPackageLauncher_ != nullptr
        && installedPackageLauncher_->retryFatalCleanupForTesting();
}

HostGestureRouter *HostApplication::gestureRouterForTesting() const noexcept
{
    return gestureRouter_.get();
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
    if (packagePath.isEmpty()) return false;
    if (!appTabRuntimeControllers_.isEmpty()) {
        QPointer<HostApplication> guard(this);
        return enqueueAppRuntime(
            [guard, packagePath,
             sourceAuthority = std::move(sourceAuthority)](
                AppRuntimeCoordinator &coordinator) mutable {
                if (sourceAuthority && !sourceAuthority->revalidate()) {
                    if (guard) {
                        const QString error = QStringLiteral(
                            "host.runtime.install_source_authority_changed");
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, error] {
                                if (guard) emit guard->updateLifecycleFailed(error);
                            },
                            Qt::QueuedConnection);
                    }
                    return;
                }
                const AppRuntimeResult result = coordinator.installAndActivate(
                    packagePath, hostMonotonicNowMs());
                if (guard && result.code != AppRuntimeResultCode::Applied) {
                    const QString error = result.stableError.isEmpty()
                        ? QStringLiteral("host.runtime.install_rejected")
                        : result.stableError;
                    QMetaObject::invokeMethod(
                        guard,
                        [guard, error, result] {
                            if (guard) {
                                emit guard->updateLifecycleFailed(
                                    error, result.nativeError);
                            }
                        },
                        Qt::QueuedConnection);
                }
            });
    }
    QPointer<HostApplication> guard(this);
    return enqueueLifecycle(
        [guard, packagePath,
         sourceAuthority = std::move(sourceAuthority)](
            UpdateLifecycleCoordinator &coordinator) {
            if (sourceAuthority && !sourceAuthority->revalidate()) {
                if (guard) {
                    const QString error = QStringLiteral(
                        "host.runtime.install_source_authority_changed");
                    QMetaObject::invokeMethod(guard, [guard, error] {
                        if (guard) emit guard->updateLifecycleFailed(error);
                    }, Qt::QueuedConnection);
                }
                return;
            }
             const UpdateLifecycleResult result = coordinator.installAndLaunch(
                packagePath);
            if (!result.succeeded() && guard) {
                const QString error = result.stableError;
                const quint32 nativeError = result.nativeError;
                QMetaObject::invokeMethod(guard, [guard, error, nativeError] {
                    if (guard) {
                        emit guard->updateLifecycleFailed(error, nativeError);
                    }
                }, Qt::QueuedConnection);
            } else if (result.succeeded() && guard) {
                // Keep the new per-tab coordinator's verified descriptor in
                // sync with the compatibility lifecycle adapter.  The
                // adapter remains responsible for legacy startup; tab
                // launches are issued only after this state is committed.
                (void)guard->enqueueAppRuntime(
                    [guard](AppRuntimeCoordinator &appCoordinator) {
                        const AppRuntimeResult appResult =
                            appCoordinator.startOffline(-1);
                        if (appResult.code != AppRuntimeResultCode::Applied) {
                            const QString error = appResult.stableError;
                            QMetaObject::invokeMethod(
                                guard,
                                [guard, error] {
                                    if (guard) {
                                        guard->handleAppRuntimeFailure(
                                            error.isEmpty()
                                                ? QStringLiteral(
                                                      "host.runtime.app_state_unavailable")
                                                : error);
                                    }
                                },
                                Qt::QueuedConnection);
                        }
                    });
            }
        });
}

bool HostApplication::requestOfflineStart()
{
    if (!appTabRuntimeControllers_.isEmpty()) {
        QPointer<HostApplication> guard(this);
        return enqueueAppRuntime(
            [guard](AppRuntimeCoordinator &coordinator) {
                const AppRuntimeResult result = coordinator.startOffline(
                    hostMonotonicNowMs());
                if (guard && result.code != AppRuntimeResultCode::Applied) {
                    const QString error = result.stableError.isEmpty()
                        ? QStringLiteral("host.runtime.offline_start_rejected")
                        : result.stableError;
                    QMetaObject::invokeMethod(
                        guard,
                        [guard, error, result] {
                            if (guard) {
                                emit guard->updateLifecycleFailed(
                                    error, result.nativeError);
                            }
                        },
                        Qt::QueuedConnection);
                }
            });
    }
    QPointer<HostApplication> guard(this);
    return enqueueLifecycle([guard](UpdateLifecycleCoordinator &coordinator) {
        const UpdateLifecycleResult result = coordinator.startOffline();
        if (!result.succeeded() && guard) {
            const QString error = result.stableError;
            const quint32 nativeError = result.nativeError;
            QMetaObject::invokeMethod(guard, [guard, error, nativeError] {
                if (guard) {
                        emit guard->updateLifecycleFailed(error, nativeError);
                    }
                }, Qt::QueuedConnection);
        } else if (result.succeeded() && guard) {
            (void)guard->enqueueAppRuntime(
                [guard](AppRuntimeCoordinator &appCoordinator) {
                    const AppRuntimeResult appResult =
                        appCoordinator.startOffline(-1);
                    if (appResult.code != AppRuntimeResultCode::Applied) {
                        const QString error = appResult.stableError;
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, error] {
                                if (guard) {
                                    guard->handleAppRuntimeFailure(
                                        error.isEmpty()
                                            ? QStringLiteral(
                                                  "host.runtime.app_state_unavailable")
                                            : error);
                                }
                            },
                            Qt::QueuedConnection);
                    }
                });
        }
    });
}

bool HostApplication::initializePackageRuntime()
{
    if (!runtimeConfig_.has_value()
        || runtimeConfig_->mode() == HostRuntimeMode::TrustedShell) return true;
    const SandboxApprovedRoots roots{
        runtimeConfig_->packageStoreRoot(), runtimeConfig_->sandboxTempRoot(),
        runtimeConfig_->immutableRuntimeRoots()};
    auto boundary = SandboxTrustBoundary::create(roots);
    if (!boundary.value.has_value()) {
        emit updateLifecycleFailed(QStringLiteral("host.runtime.trust_boundary_rejected"));
        return false;
    }

    InstallPolicy installPolicy;
    installPolicy.expectedAppId = runtimeConfig_->appId();
    installPolicy.runtimeVersion = QStringLiteral("1.2.0");
    installPolicy.allowedImports = {QStringLiteral("QtQuick"),
                                    QStringLiteral("QtQuick.Layouts"),
                                    QStringLiteral("Company.Design")};
    installPolicy.preflight = [](const Manifest &, const QString &) { return true; };
    auto authority = std::make_shared<RuntimePackageAuthority>(
        runtimeConfig_->packageStoreRoot(),
        runtimeConfig_->trustedPublicKeyPem(), std::move(installPolicy));
    BrowserTabModel *const tabModel = mainWindow_ != nullptr
        ? mainWindow_->tabModel() : nullptr;
    const QString packageTabId = tabModel != nullptr
        ? tabModel->activeId() : QString{};
    if (packageTabId.isEmpty() || nextCapabilityRuntimeIncarnation_ == 0) {
        emit updateLifecycleFailed(
            QStringLiteral("host.runtime.launch_authority_unavailable"));
        return false;
    }
    if (mainWindow_ == nullptr
        || !mainWindow_->reserveLegacyWorkerOwner(packageTabId)) {
        emit updateLifecycleFailed(
            QStringLiteral("host.runtime.legacy_owner_unavailable"));
        return false;
    }
    const quint64 runtimeIncarnation = nextCapabilityRuntimeIncarnation_++;

    QPointer<HostApplication> guard(this);
    installedPackageLauncher_ = std::make_unique<InstalledPackageWorkerLauncher>(
        std::move(*boundary.value), runtimeConfig_->workerExecutable(),
        runtimeConfig_->sandboxTempRoot(), runtimeConfig_->mockOrigin(),
        [authority](
            const WorkerLaunchRequest &request,
            std::shared_ptr<const ImmutablePackageGuard> retainedGuard) {
            return authority->revalidateWorkerLaunch(
                request, std::move(retainedGuard));
        },
        [guard](const WorkerLaunchRequest &request,
                InstalledPackageWorkerLauncher::AdmissionCompletion complete) {
            if (!guard || !complete) return false;
            return guard->enqueueLifecycle(
                [request, complete = std::move(complete)](
                    UpdateLifecycleCoordinator &coordinator) mutable {
#ifdef Q_BROWSER_HOST_TESTING
                    const auto hooks = qbrowser_host_testing::
                        installedPackageWorkerLauncherTestHooks();
                    if (hooks.beforeAdmissionDecision) {
                        hooks.beforeAdmissionDecision(request, coordinator);
                    }
#endif
                    const UpdateLifecycleAction action =
                        coordinator.admitAuthenticatedWorker(request);
                    const bool accepted = action == UpdateLifecycleAction::None;
                    const bool ignoredStale = action
                        == UpdateLifecycleAction::IgnoredStaleAttempt;
                    complete({
                        accepted,
                        accepted || ignoredStale
                            ? QString{}
                            : QStringLiteral("host.launch.admission_rejected"),
                        ignoredStale,
                        accepted
                            ? std::optional<WorkerLaunchRequest>(request)
                            : std::nullopt});
                });
        },
        [guard](InstalledPackageWorkerLauncher::CommittedAttachTransaction
                    transaction) {
#ifdef Q_BROWSER_HOST_TESTING
            const auto hooks = qbrowser_host_testing::
                installedPackageWorkerLauncherTestHooks();
            if (hooks.beforeCommittedAttachRealization) {
                hooks.beforeCommittedAttachRealization(transaction.request());
            }
            if (hooks.throwAttachRealization) {
                throw std::runtime_error("injected attach realization failure");
            }
#endif
            if (!guard)
                return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
            return guard->attachWorkerContext(std::move(transaction));
        },
        [guard] {
            if (guard) guard->detachWorkerContext(
                QStringLiteral("host.worker_context.supervised_relaunch"));
        },
        [guard](const WorkerAttemptKey key, const bool expected) {
            if (!guard || expected) return;
#ifdef Q_BROWSER_HOST_TESTING
            const auto hooks = qbrowser_host_testing::
                installedPackageWorkerLauncherTestHooks();
            if (hooks.duringExitCallbackBeforeLifecycleEnqueue) {
                hooks.duringExitCallbackBeforeLifecycleEnqueue();
            }
#endif
            if (!guard) return;
            const bool queued = guard->enqueueLifecycle(
                [key](UpdateLifecycleCoordinator &coordinator) {
                    (void)coordinator.workerExited(key, WorkerExitReason::Crashed);
                });
            if (!queued) {
                guard->failClosedAppRuntime(
                    QStringLiteral("host.runtime.critical_event_dropped"));
            }
        },
        [guard](const WorkerAttemptKey key,
                const QString &stableError,
                const quint32 nativeError) {
            QPointer<HostApplication> localGuard = guard;
            if (!localGuard) return;
            const QString error = stableError.isEmpty()
                ? QStringLiteral("host.launch.failed") : stableError;
            emit localGuard->updateLifecycleFailed(error, nativeError);
            if (!localGuard) return;
#ifdef Q_BROWSER_HOST_TESTING
            const auto hooks = qbrowser_host_testing::
                installedPackageWorkerLauncherTestHooks();
            if (hooks.afterFailureSignalBeforeLifecycleEnqueue) {
                hooks.afterFailureSignalBeforeLifecycleEnqueue();
            }
#endif
            if (!localGuard) return;
            const bool cleanupFailure = error == QStringLiteral(
                    "host.launch.temp_cleanup_failed")
                || error == QStringLiteral("host.launch.process_wait_failed")
                || error == QStringLiteral("host.launch.process_cleanup_failed")
                || error.startsWith(QStringLiteral("sandbox.process."))
                || error.startsWith(QStringLiteral("sandbox.job."))
                || error.startsWith(QStringLiteral("sandbox.acl."))
                || error.startsWith(QStringLiteral("package.immutable_"));
            const bool admissionFailure = error.startsWith(
                QStringLiteral("host.launch.admission_"));
            const bool queued = localGuard->enqueueLifecycle(
                [key, cleanupFailure, admissionFailure](
                    UpdateLifecycleCoordinator &coordinator) {
                    if (cleanupFailure) {
                        (void)coordinator.workerCleanupFailed(key);
                    } else if (admissionFailure) {
                        (void)coordinator.workerAdmissionFailed(key);
                    } else {
                        (void)coordinator.workerExited(
                            key, WorkerExitReason::StartupFailure);
                    }
                });
            if (!queued && cleanupFailure) {
                localGuard->failClosedAppRuntime(error);
            }
        }, this);
    if (!installedPackageLauncher_->isAccepting()) {
        installedPackageLauncher_.reset();
        emit updateLifecycleFailed(QStringLiteral("host.runtime.launcher_invalid"));
        return false;
    }
    connect(installedPackageLauncher_.get(),
            &InstalledPackageWorkerLauncher::ready,
            this,
            [this](const QString &appId, const QString &version,
                   const QString &packageDirectory,
                   const quint64 activation, const quint64 attempt,
                   const quint32 processId) {
                if (pendingWorkerAttach_ != nullptr
                    && pendingWorkerAttach_->launchRequest.attempt
                        == WorkerAttemptKey{WorkerActivationId{activation},
                                            WorkerAttemptId{attempt}}) {
                    return;
                }
                emit packageWorkerReady(
                    appId, version, packageDirectory, activation, attempt,
                    processId);
            });
    connect(installedPackageLauncher_.get(),
            &InstalledPackageWorkerLauncher::unexpectedExit,
            this, &HostApplication::packageWorkerExited);

    EventRecorderConfig recorderConfig;
    recorderConfig.directoryPath = runtimeConfig_->telemetryDirectory();
    auto recorder = std::make_unique<EventRecorder>(std::move(recorderConfig));

    QPointer<InstalledPackageWorkerLauncher> launcher(installedPackageLauncher_.get());
    auto coordinator = std::make_unique<UpdateLifecycleCoordinator>(
        runtimeConfig_->appId(), authority->store(), authority->installer(),
        WorkerSupervisionPolicy{runtimeConfig_->healthWindowMs(),
                                runtimeConfig_->heartbeatTimeoutMs()},
        [launcher](const UpdateLaunchRequest &request) {
            if (!launcher) return false;
            return QMetaObject::invokeMethod(launcher, [launcher, request] {
                if (launcher) (void)launcher->requestLaunch(
                    request.workerRequest);
            }, Qt::QueuedConnection);
        }, LifecycleClock::system(), recorder.get(), packageTabId,
        runtimeIncarnation);
    coordinator->setBeforeRelaunchCallback([launcher] {
        if (launcher) {
            (void)QMetaObject::invokeMethod(launcher, [launcher] {
                if (launcher) launcher->stopCurrent();
            }, Qt::QueuedConnection);
        }
    });

    auto appCoordinator = std::make_unique<AppRuntimeCoordinator>(
        runtimeConfig_->appId(), authority->store(), authority->installer(),
        WorkerSupervisionPolicy{runtimeConfig_->healthWindowMs(),
                                runtimeConfig_->heartbeatTimeoutMs()},
        LifecycleClock::system(), recorder.get(),
        AppRuntimeCoordinator::DrainConsumer{}, 10'000);
    packageAuthority_ = authority;

    auto *const runtime = new HostLifecycleRuntime(
        std::move(authority), std::move(recorder),
        std::move(coordinator),
        std::move(appCoordinator),
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
    if (mainWindow_) {
        if (!mainWindow_->hasValidWebSession()) {
            recordHostDiagnosticPhase("start-existing-window-retired");
            return false;
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
    auto window = std::make_unique<MainWindow>(std::move(*routes), mockOrigin_);
    recordHostDiagnosticPhase("start-main-window-created");
    if (!window->hasValidWebSession()) {
        return false;
    }
    const bool packageMode = runtimeConfig_.has_value()
        && runtimeConfig_->mode() == HostRuntimeMode::Package;
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
    window->show();
    recordHostDiagnosticPhase("start-main-window-shown");
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
    workerSessionController_ = std::make_unique<HostWorkerSessionController>(
        mainWindow_.get());
    recordHostDiagnosticPhase("start-session-controller-ready");
    if (!initializePackageRuntime()) return false;
    recordHostDiagnosticPhase("start-package-runtime-ready");

    connect(workerSessionController_.get(), &HostWorkerSessionController::failed,
            this, [this] {
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

    if (runtimeConfig_.has_value()
        && runtimeConfig_->mode() == HostRuntimeMode::Package) {
        bool queued = false;
        if (runtimeConfig_->installPackage().has_value()) {
            queued = requestPackageInstall(
                *runtimeConfig_->installPackage(),
                runtimeConfig_->takeInstallPackageAuthority());
        } else {
            queued = requestOfflineStart();
        }
        if (!queued) {
            emit updateLifecycleFailed(QStringLiteral("host.runtime.start_queue_failed"));
            return false;
        }
        recordHostDiagnosticPhase("start-package-operation-queued");
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
