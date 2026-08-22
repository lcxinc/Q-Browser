#include "HostApplication.h"

#include "EventRecorder.h"
#include "HostCapabilityRuntime.h"
#include "HostWorkerSessionController.h"
#include "InstalledPackageWorkerLauncher.h"
#include "InstalledPackageWorkerLauncherTestHooks.h"
#include "MainWindow.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "RuntimePackageAuthority.h"
#include "SandboxTrustBoundary.h"
#include "UpdateLifecycleCoordinator.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSurface.h"

#include <QPointer>
#include <QRegularExpression>
#include <QFile>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <utility>

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

class HostLifecycleRuntime final : public QObject
{
public:
    HostLifecycleRuntime(std::shared_ptr<RuntimePackageAuthority> authority,
                         std::unique_ptr<EventRecorder> recorder,
                         std::unique_ptr<UpdateLifecycleCoordinator> coordinator)
        : authority_(std::move(authority))
        , recorder_(std::move(recorder))
        , coordinator_(std::move(coordinator))
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

    void closeAdmission() noexcept
    {
        accepting_.store(false, std::memory_order_release);
    }

    void shutdown()
    {
        closeAdmission();
        if (coordinator_ != nullptr) coordinator_->beginHostShutdown();
    }

private:
    std::shared_ptr<RuntimePackageAuthority> authority_;
    std::unique_ptr<EventRecorder> recorder_;
    std::unique_ptr<UpdateLifecycleCoordinator> coordinator_;
    std::atomic<qsizetype> pending_{0};
    std::atomic_bool accepting_{true};
};
}

HostApplication::HostApplication(QUrl mockOrigin, QObject *parent)
    : QObject(parent), mockOrigin_(std::move(mockOrigin))
{
}

HostApplication::HostApplication(HostRuntimeConfig runtimeConfig,
                                 QObject *parent)
    : QObject(parent)
    , runtimeConfig_(std::move(runtimeConfig))
    , mockOrigin_(runtimeConfig_->mockOrigin())
{
}

HostApplication::~HostApplication()
{
    acceptingLifecycle_.store(false, std::memory_order_release);
    if (updateHealthTimer_ != nullptr) updateHealthTimer_->stop();
    if (mainWindow_ != nullptr) mainWindow_->hide();
    if (installedPackageLauncher_ != nullptr) installedPackageLauncher_->cancel();
    detachWorkerContext(QStringLiteral("host.application.stopping"));

    QPointer<HostLifecycleRuntime> runtime(
        static_cast<HostLifecycleRuntime *>(updateLifecycleRuntime_.data()));
    if (runtime) runtime->closeAdmission();
    QThread *const lifecycleThread = updateLifecycleThread_;
    if (runtime && lifecycleThread != nullptr) {
        (void)QMetaObject::invokeMethod(
            runtime,
            [runtime, lifecycleThread] {
                if (runtime) runtime->shutdown();
                lifecycleThread->quit();
            }, Qt::QueuedConnection);
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

#ifdef Q_BROWSER_HOST_TESTING
void HostApplication::forceLifecycleQueueFullForTesting(const bool full) noexcept
{
    lifecycleQueueFullForTesting_ = full;
}

bool HostApplication::retryWorkerCleanupForTesting()
{
    return installedPackageLauncher_ != nullptr
        && installedPackageLauncher_->retryFatalCleanupForTesting();
}
#endif

bool HostApplication::requestPackageInstall(const QString &packagePath)
{
    if (packagePath.isEmpty()) return false;
    QPointer<HostApplication> guard(this);
    return enqueueLifecycle(
        [guard, packagePath](UpdateLifecycleCoordinator &coordinator) {
            const UpdateLifecycleResult result = coordinator.installAndLaunch(
                packagePath);
            if (!result.succeeded() && guard) {
                const QString error = result.stableError;
                QMetaObject::invokeMethod(guard, [guard, error] {
                    if (guard) emit guard->updateLifecycleFailed(error);
                }, Qt::QueuedConnection);
            }
        });
}

bool HostApplication::requestOfflineStart()
{
    QPointer<HostApplication> guard(this);
    return enqueueLifecycle([guard](UpdateLifecycleCoordinator &coordinator) {
        const UpdateLifecycleResult result = coordinator.startOffline();
        if (!result.succeeded() && guard) {
            const QString error = result.stableError;
            QMetaObject::invokeMethod(guard, [guard, error] {
                if (guard) emit guard->updateLifecycleFailed(error);
            }, Qt::QueuedConnection);
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

    QPointer<HostApplication> guard(this);
    installedPackageLauncher_ = std::make_unique<InstalledPackageWorkerLauncher>(
        std::move(*boundary.value), runtimeConfig_->workerExecutable(),
        runtimeConfig_->sandboxTempRoot(), runtimeConfig_->mockOrigin(),
        [authority](const QString &appId, const ActivationBinding &binding) {
            return authority->reverifyInstalledVersion(appId, binding);
        },
        [guard](const UpdateLaunchRequest &request,
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
                        coordinator.admitAuthenticatedWorker(
                            request.key, request.expectedActivation);
                    const bool accepted = action == UpdateLifecycleAction::None;
                    const bool ignoredStale = action
                        == UpdateLifecycleAction::IgnoredStaleAttempt;
                    complete({accepted,
                              accepted || ignoredStale
                                  ? QString{}
                                  : QStringLiteral(
                                        "host.launch.admission_rejected"),
                              ignoredStale});
                });
        },
        [guard](std::unique_ptr<IpcSession> session,
                std::unique_ptr<WorkerSurface> surface,
                std::shared_ptr<SandboxProcess> process,
                ManifestPermissions permissions,
                const WorkerAttemptKey key) {
            if (!guard || process == nullptr)
                return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
            HostWorkerAttachContext context;
            context.session = std::move(session);
            context.surface = std::move(surface);
            context.processLifetime = std::static_pointer_cast<void>(process);
            context.permissions = std::move(permissions);
            context.processId = process->processId();
            context.stopProcess = [process] {
                process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
            };
            context.supervisionKey = key;
            return guard->attachWorkerContext(std::move(context));
        },
        [guard] {
            if (guard) guard->detachWorkerContext(
                QStringLiteral("host.worker_context.supervised_relaunch"));
        },
        [guard](const WorkerAttemptKey key, const bool expected) {
            if (!guard || expected) return;
            (void)guard->enqueueLifecycle(
                [key](UpdateLifecycleCoordinator &coordinator) {
                    (void)coordinator.workerExited(key, WorkerExitReason::Crashed);
                });
        },
        [guard](const WorkerAttemptKey key, const QString &stableError) {
            QPointer<HostApplication> localGuard = guard;
            if (!localGuard) return;
            const QString error = stableError.isEmpty()
                ? QStringLiteral("host.launch.failed") : stableError;
            emit localGuard->updateLifecycleFailed(error);
            if (!localGuard) return;
#ifdef Q_BROWSER_HOST_TESTING
            const auto hooks = qbrowser_host_testing::
                installedPackageWorkerLauncherTestHooks();
            if (hooks.afterFailureSignalBeforeLifecycleEnqueue) {
                hooks.afterFailureSignalBeforeLifecycleEnqueue();
            }
#endif
            const bool cleanupFailure = error == QStringLiteral(
                    "host.launch.temp_cleanup_failed")
                || error == QStringLiteral("host.launch.process_wait_failed")
                || error == QStringLiteral("host.launch.process_cleanup_failed")
                || error.startsWith(QStringLiteral("sandbox.process."))
                || error.startsWith(QStringLiteral("sandbox.job."))
                || error.startsWith(QStringLiteral("sandbox.acl."));
            const bool admissionFailure = error.startsWith(
                QStringLiteral("host.launch.admission_"));
            (void)localGuard->enqueueLifecycle(
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
        }, this);
    if (!installedPackageLauncher_->isAccepting()) {
        installedPackageLauncher_.reset();
        emit updateLifecycleFailed(QStringLiteral("host.runtime.launcher_invalid"));
        return false;
    }
    connect(installedPackageLauncher_.get(),
            &InstalledPackageWorkerLauncher::ready,
            this, &HostApplication::packageWorkerReady);
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
                if (launcher) (void)launcher->requestLaunch(request);
            }, Qt::QueuedConnection);
        }, LifecycleClock::system(), recorder.get());
    coordinator->setBeforeRelaunchCallback([launcher] {
        if (launcher) {
            (void)QMetaObject::invokeMethod(launcher, [launcher] {
                if (launcher) launcher->stopCurrent();
            }, Qt::QueuedConnection);
        }
    });

    auto *const runtime = new HostLifecycleRuntime(
        std::move(authority), std::move(recorder),
        std::move(coordinator));
    auto *const lifecycleThread = new QThread;
    lifecycleThread->setObjectName(QStringLiteral("host-update-lifecycle"));
    runtime->moveToThread(lifecycleThread);
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
    if (!window->webSurface()->isConfigurationValid()) return false;
    window->resize(1100, 720);
    window->show();
    recordHostDiagnosticPhase("start-main-window-shown");
    mainWindow_ = std::move(window);
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
                    (void)enqueueLifecycle(
                        [failedKey](UpdateLifecycleCoordinator &coordinator) {
                            (void)coordinator.workerExited(
                                *failedKey, WorkerExitReason::Crashed);
                        });
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
        if (!attachedWorkerKey_.has_value()) return;
        const WorkerAttemptKey key = *attachedWorkerKey_;
        (void)enqueueLifecycle([key](UpdateLifecycleCoordinator &coordinator) {
            (void)coordinator.checkHealth(key);
        });
    });
    updateHealthTimer_->start();
    recordHostDiagnosticPhase("start-health-timer-ready");

    if (runtimeConfig_.has_value()
        && runtimeConfig_->mode() == HostRuntimeMode::Package) {
        const bool queued = runtimeConfig_->installPackage().has_value()
            ? requestPackageInstall(*runtimeConfig_->installPackage())
            : requestOfflineStart();
        if (!queued) {
            emit updateLifecycleFailed(QStringLiteral("host.runtime.start_queue_failed"));
            return false;
        }
        recordHostDiagnosticPhase("start-package-operation-queued");
    }
    recordHostDiagnosticPhase("start-complete");
    return true;
}

bool HostApplication::attachWorkerSession(std::unique_ptr<IpcSession> session)
{
    return workerSessionController_ != nullptr
        && workerSessionController_->attach(std::move(session));
}

InstalledPackageWorkerLauncher::AttachResult
HostApplication::attachWorkerContext(HostWorkerAttachContext context)
{
    if (mainWindow_ == nullptr || workerSessionController_ == nullptr
        || context.session == nullptr || context.surface == nullptr
        || context.processLifetime == nullptr || !context.stopProcess
        || workerProcessLifetime_ != nullptr || !runtimeConfig_.has_value()
        || context.session->appIdentity() != runtimeConfig_->appId())
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    QString capabilityError;
    const quintptr workerWindowId = static_cast<quintptr>(
        context.surface->nativeWindowId());
    auto capabilityRuntime = HostCapabilityRuntime::create(
        runtimeConfig_->appId(), context.permissions, runtimeConfig_->mockOrigin(),
        runtimeConfig_->storageDirectory(), static_cast<quintptr>(mainWindow_->winId()),
        workerWindowId, context.processId, &capabilityError);
    if (capabilityRuntime == nullptr) {
        emit updateLifecycleFailed(
            capabilityError.isEmpty()
                ? QStringLiteral("host.capability.initialization_failed")
                : capabilityError);
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    connect(capabilityRuntime.get(), &HostCapabilityRuntime::completed,
            workerSessionController_.get(),
            &HostWorkerSessionController::completeCapability,
            Qt::QueuedConnection);
    workerSessionController_->setCapabilityRuntime(capabilityRuntime.get());
    WorkerSurface *const surface = context.surface.get();
    if (!mainWindow_->attachWorkerSurface(std::move(context.surface))) {
        workerSessionController_->setCapabilityRuntime(nullptr);
        HostCapabilityRuntime::retire(std::move(capabilityRuntime));
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    if (!workerSessionController_->attach(std::move(context.session))) {
        workerSessionController_->setCapabilityRuntime(nullptr);
        mainWindow_->detachWorkerSurface();
        HostCapabilityRuntime::retire(std::move(capabilityRuntime));
        return InstalledPackageWorkerLauncher::AttachResult::ConsumedFailure;
    }
    capabilityRuntime_ = std::move(capabilityRuntime);
    workerProcessLifetime_ = std::move(context.processLifetime);
    stopWorkerProcess_ = std::move(context.stopProcess);
    attachedWorkerKey_ = context.supervisionKey;
    Q_ASSERT(mainWindow_->workerSurface() == surface);
    return InstalledPackageWorkerLauncher::AttachResult::Attached;
}

void HostApplication::detachWorkerContext(const QString &reason)
{
    if (workerProcessLifetime_ == nullptr) return;
    if (workerSessionController_ != nullptr
        && workerSessionController_->state() == HostWorkerSessionState::Running) {
        (void)workerSessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker_context.detached") : reason);
    }
    if (stopWorkerProcess_) stopWorkerProcess_();
    if (workerSessionController_ != nullptr)
        workerSessionController_->setCapabilityRuntime(nullptr);
    HostCapabilityRuntime::retire(std::exchange(capabilityRuntime_, {}));
    if (mainWindow_ != nullptr) mainWindow_->detachWorkerSurface();
    stopWorkerProcess_ = {};
    workerProcessLifetime_.reset();
    attachedWorkerKey_.reset();
}

bool HostApplication::hasWorkerContext() const noexcept
{
    return workerProcessLifetime_ != nullptr;
}

MainWindow *HostApplication::mainWindow() const noexcept
{
    return mainWindow_.get();
}

HostWorkerSessionController *HostApplication::workerSessionController() const noexcept
{
    return workerSessionController_.get();
}
