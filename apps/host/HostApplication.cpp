#include "HostApplication.h"

#include "EventRecorder.h"
#include "HostWorkerSessionController.h"
#include "InstalledPackageWorkerLauncher.h"
#include "MainWindow.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "SandboxTrustBoundary.h"
#include "UpdateLifecycleCoordinator.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSurface.h"

#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <utility>

namespace
{
constexpr qsizetype MaximumPendingLifecycleOperations = 512;

class HostLifecycleRuntime final : public QObject
{
public:
    HostLifecycleRuntime(std::unique_ptr<PackageStore> store,
                         std::unique_ptr<PackageInstaller> installer,
                         std::unique_ptr<EventRecorder> recorder,
                         std::unique_ptr<UpdateLifecycleCoordinator> coordinator)
        : store_(std::move(store))
        , installer_(std::move(installer))
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
    std::unique_ptr<PackageStore> store_;
    std::unique_ptr<PackageInstaller> installer_;
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
}

bool HostApplication::enqueueLifecycle(
    std::function<void(UpdateLifecycleCoordinator &)> operation)
{
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

    QPointer<HostApplication> guard(this);
    installedPackageLauncher_ = std::make_unique<InstalledPackageWorkerLauncher>(
        std::move(*boundary.value), runtimeConfig_->workerExecutable(),
        runtimeConfig_->sandboxTempRoot(),
        runtimeConfig_->mockOrigin(),
        [guard](std::unique_ptr<IpcSession> session, WorkerSurface *surface,
                std::shared_ptr<SandboxProcess> process,
                const WorkerAttemptKey key) {
            if (!guard || process == nullptr) return false;
            HostWorkerAttachContext context;
            context.session = std::move(session);
            context.surface = surface;
            context.processLifetime = std::static_pointer_cast<void>(process);
            context.stopProcess = [process] {
                process->terminate(ERROR_PROCESS_ABORTED);
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
            if (!guard) return;
            emit guard->updateLifecycleFailed(
                stableError.isEmpty() ? QStringLiteral("host.launch.failed")
                                      : stableError);
            (void)guard->enqueueLifecycle(
                [key](UpdateLifecycleCoordinator &coordinator) {
                    (void)coordinator.workerExited(
                        key, WorkerExitReason::StartupFailure);
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

    auto store = std::make_unique<PackageStore>(runtimeConfig_->packageStoreRoot());
    InstallPolicy installPolicy;
    installPolicy.runtimeVersion = QStringLiteral("1.2.0");
    installPolicy.allowedImports = {QStringLiteral("QtQuick"),
                                    QStringLiteral("Company.Design")};
    installPolicy.preflight = [](const Manifest &, const QString &) { return true; };
    auto installer = std::make_unique<PackageInstaller>(
        *store, runtimeConfig_->trustedPublicKeyPem(), std::move(installPolicy));
    EventRecorderConfig recorderConfig;
    recorderConfig.directoryPath = runtimeConfig_->telemetryDirectory();
    auto recorder = std::make_unique<EventRecorder>(std::move(recorderConfig));

    QPointer<InstalledPackageWorkerLauncher> launcher(installedPackageLauncher_.get());
    auto coordinator = std::make_unique<UpdateLifecycleCoordinator>(
        runtimeConfig_->appId(), *store, *installer,
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
        std::move(store), std::move(installer), std::move(recorder),
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
    if (mainWindow_) {
        mainWindow_->show();
        return true;
    }
    auto routes = createPilotRouteRegistry(mockOrigin_);
    if (!routes.has_value()) return false;
    auto window = std::make_unique<MainWindow>(std::move(*routes), mockOrigin_);
    if (!window->webSurface()->isConfigurationValid()) return false;
    window->resize(1100, 720);
    window->show();
    mainWindow_ = std::move(window);
    workerSessionController_ = std::make_unique<HostWorkerSessionController>(
        mainWindow_.get());
    if (!initializePackageRuntime()) return false;

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

    if (runtimeConfig_.has_value()
        && runtimeConfig_->mode() == HostRuntimeMode::Package) {
        const bool queued = runtimeConfig_->installPackage().has_value()
            ? requestPackageInstall(*runtimeConfig_->installPackage())
            : requestOfflineStart();
        if (!queued) {
            emit updateLifecycleFailed(QStringLiteral("host.runtime.start_queue_failed"));
            return false;
        }
    }
    return true;
}

bool HostApplication::attachWorkerSession(std::unique_ptr<IpcSession> session)
{
    return workerSessionController_ != nullptr
        && workerSessionController_->attach(std::move(session));
}

bool HostApplication::attachWorkerContext(HostWorkerAttachContext context)
{
    if (mainWindow_ == nullptr || workerSessionController_ == nullptr
        || context.session == nullptr || context.surface == nullptr
        || context.processLifetime == nullptr || !context.stopProcess
        || workerProcessLifetime_ != nullptr
        || !mainWindow_->attachWorkerSurface(context.surface)) return false;
    if (!workerSessionController_->attach(std::move(context.session))) {
        mainWindow_->detachWorkerSurface();
        return false;
    }
    workerProcessLifetime_ = std::move(context.processLifetime);
    stopWorkerProcess_ = std::move(context.stopProcess);
    attachedWorkerKey_ = context.supervisionKey;
    if (attachedWorkerKey_.has_value()) {
        const WorkerAttemptKey key = *attachedWorkerKey_;
        QPointer<HostApplication> guard(this);
        if (!enqueueLifecycle([guard, key](UpdateLifecycleCoordinator &coordinator) {
                const UpdateLifecycleAction action =
                    coordinator.authenticatedHandshake(key);
                if ((action == UpdateLifecycleAction::FailedClosed
                     || action == UpdateLifecycleAction::IgnoredStaleAttempt)
                    && guard) {
                    QMetaObject::invokeMethod(guard, [guard] {
                        if (guard) guard->detachWorkerContext(QStringLiteral(
                            "host.worker_context.supervision_rejected"));
                    }, Qt::QueuedConnection);
                }
            })) {
            detachWorkerContext(
                QStringLiteral("host.worker_context.supervision_queue_full"));
            return false;
        }
    }
    return true;
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
