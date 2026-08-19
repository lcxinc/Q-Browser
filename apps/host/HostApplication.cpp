#include "HostApplication.h"

#include "MainWindow.h"
#include "HostWorkerSessionController.h"
#include "IpcSession.h"
#include "UpdateLifecycleCoordinator.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSurface.h"

#include <utility>
#include <QDateTime>
#include <QTimer>
#include <QThread>

HostApplication::HostApplication(QUrl mockOrigin, QObject *parent)
    : QObject(parent), mockOrigin_(std::move(mockOrigin))
{
}

HostApplication::~HostApplication()
{
    if (updateLifecycleCoordinator_ != nullptr && updateLifecycleDispatch_ != nullptr
        && updateLifecycleThread_ != nullptr && updateLifecycleThread_->isRunning()) {
        UpdateLifecycleCoordinator *const coordinator =
            updateLifecycleCoordinator_.get();
        (void)QMetaObject::invokeMethod(
            updateLifecycleDispatch_,
            [coordinator] { coordinator->beginHostShutdown(); },
            Qt::BlockingQueuedConnection);
    }
    detachWorkerContext(QStringLiteral("host.application.stopping"));
    if (updateLifecycleThread_ != nullptr) {
        updateLifecycleThread_->quit();
        if (!updateLifecycleThread_->wait(5000)) {
            qFatal("Unable to stop update lifecycle thread");
        }
        updateLifecycleDispatch_ = nullptr;
    }
}

bool HostApplication::setUpdateLifecycleCoordinator(
    std::unique_ptr<UpdateLifecycleCoordinator> coordinator)
{
    if (coordinator == nullptr || updateLifecycleCoordinator_ != nullptr
        || mainWindow_ != nullptr) {
        return false;
    }
    coordinator->setBeforeRelaunchCallback([this] {
        const auto detach = [this] {
            detachWorkerContext(QStringLiteral(
                "host.worker_context.supervised_relaunch"));
        };
        if (QThread::currentThread() == thread()) {
            detach();
        } else {
            (void)QMetaObject::invokeMethod(
                this, detach, Qt::BlockingQueuedConnection);
        }
    });
    updateLifecycleCoordinator_ = std::move(coordinator);
    return true;
}

bool HostApplication::enqueueLifecycle(
    std::function<void(UpdateLifecycleCoordinator &)> operation)
{
    if (!operation || updateLifecycleCoordinator_ == nullptr
        || updateLifecycleDispatch_ == nullptr) {
        return false;
    }
    qsizetype pending = pendingLifecycleOperations_.load(std::memory_order_relaxed);
    do {
        if (pending >= maximumPendingLifecycleOperations) return false;
    } while (!pendingLifecycleOperations_.compare_exchange_weak(
        pending, pending + 1, std::memory_order_acq_rel));
    UpdateLifecycleCoordinator *const coordinator =
        updateLifecycleCoordinator_.get();
    const bool queued = QMetaObject::invokeMethod(
        updateLifecycleDispatch_,
        [this, coordinator, operation = std::move(operation)]() mutable {
            operation(*coordinator);
            pendingLifecycleOperations_.fetch_sub(1, std::memory_order_release);
        },
        Qt::QueuedConnection);
    if (!queued) {
        pendingLifecycleOperations_.fetch_sub(1, std::memory_order_release);
    }
    return queued;
}

bool HostApplication::requestPackageInstall(const QString &packagePath)
{
    if (packagePath.isEmpty()) return false;
    return enqueueLifecycle([this, packagePath](UpdateLifecycleCoordinator &coordinator) {
        const UpdateLifecycleResult result = coordinator.installAndLaunch(
            packagePath, QDateTime::currentMSecsSinceEpoch());
        if (!result.succeeded()) {
            const QString error = result.stableError;
            QMetaObject::invokeMethod(this, [this, error] {
                emit updateLifecycleFailed(error);
            }, Qt::QueuedConnection);
        }
    });
}

bool HostApplication::requestOfflineStart()
{
    return enqueueLifecycle([this](UpdateLifecycleCoordinator &coordinator) {
        const UpdateLifecycleResult result = coordinator.startOffline(
            QDateTime::currentMSecsSinceEpoch());
        if (!result.succeeded()) {
            const QString error = result.stableError;
            QMetaObject::invokeMethod(this, [this, error] {
                emit updateLifecycleFailed(error);
            }, Qt::QueuedConnection);
        }
    });
}

bool HostApplication::start()
{
    if (mainWindow_) {
        mainWindow_->show();
        return true;
    }

    auto routes = createPilotRouteRegistry(mockOrigin_);
    if (!routes.has_value()) {
        return false;
    }

    auto window = std::make_unique<MainWindow>(std::move(*routes), mockOrigin_);
    if (!window->webSurface()->isConfigurationValid()) {
        return false;
    }
    window->resize(1100, 720);
    window->show();
    mainWindow_ = std::move(window);
    workerSessionController_ = std::make_unique<HostWorkerSessionController>(
        mainWindow_.get());
    if (updateLifecycleCoordinator_ != nullptr) {
        updateLifecycleThread_ = std::make_unique<QThread>();
        updateLifecycleThread_->setObjectName(
            QStringLiteral("host-update-lifecycle"));
        updateLifecycleDispatch_ = new QObject;
        updateLifecycleDispatch_->moveToThread(updateLifecycleThread_.get());
        connect(updateLifecycleThread_.get(), &QThread::finished,
                updateLifecycleDispatch_, &QObject::deleteLater);
        updateLifecycleThread_->start();
    }
    connect(workerSessionController_.get(), &HostWorkerSessionController::failed,
            this, [this] {
                const std::optional<WorkerAttemptKey> failedKey = attachedWorkerKey_;
                detachWorkerContext(QStringLiteral("host.worker_session.failed"));
                if (failedKey.has_value() && updateLifecycleCoordinator_ != nullptr) {
                    const qint64 observedAt = QDateTime::currentMSecsSinceEpoch();
                    (void)enqueueLifecycle([failedKey, observedAt](
                        UpdateLifecycleCoordinator &coordinator) {
                        (void)coordinator.workerExited(
                            *failedKey, WorkerExitReason::Crashed,
                            observedAt);
                    });
                }
            });
    connect(workerSessionController_.get(),
            &HostWorkerSessionController::heartbeatObserved,
            this, [this] {
                if (attachedWorkerKey_.has_value()
                    && updateLifecycleCoordinator_ != nullptr) {
                    const WorkerAttemptKey key = *attachedWorkerKey_;
                    const qint64 observedAt = QDateTime::currentMSecsSinceEpoch();
                    (void)enqueueLifecycle([key, observedAt](
                            UpdateLifecycleCoordinator &coordinator) {
                            (void)coordinator.heartbeat(
                                key, observedAt);
                        });
                }
            });
    updateHealthTimer_ = std::make_unique<QTimer>();
    updateHealthTimer_->setInterval(250);
    connect(updateHealthTimer_.get(), &QTimer::timeout, this, [this] {
        if (attachedWorkerKey_.has_value()
            && updateLifecycleCoordinator_ != nullptr) {
            const WorkerAttemptKey key = *attachedWorkerKey_;
            const qint64 observedAt = QDateTime::currentMSecsSinceEpoch();
            (void)enqueueLifecycle([key, observedAt](
                    UpdateLifecycleCoordinator &coordinator) {
                    (void)coordinator.checkHealth(
                        key, observedAt);
                });
        }
    });
    updateHealthTimer_->start();
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
        || !mainWindow_->attachWorkerSurface(context.surface)) {
        return false;
    }
    if (!workerSessionController_->attach(std::move(context.session))) {
        mainWindow_->detachWorkerSurface();
        return false;
    }
    workerProcessLifetime_ = std::move(context.processLifetime);
    stopWorkerProcess_ = std::move(context.stopProcess);
    attachedWorkerKey_ = context.supervisionKey;
    if (attachedWorkerKey_.has_value() && updateLifecycleCoordinator_ != nullptr) {
        const WorkerAttemptKey key = *attachedWorkerKey_;
        const qint64 observedAt = QDateTime::currentMSecsSinceEpoch();
        if (!enqueueLifecycle([this, key, observedAt](
                UpdateLifecycleCoordinator &coordinator) {
                const UpdateLifecycleAction action =
                    coordinator.authenticatedHandshake(
                        key, observedAt);
                if (action == UpdateLifecycleAction::FailedClosed
                    || action == UpdateLifecycleAction::IgnoredStaleAttempt) {
                    QMetaObject::invokeMethod(this, [this] {
                        detachWorkerContext(QStringLiteral(
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
        && workerSessionController_->state() == HostWorkerSessionState::Running)
        (void)workerSessionController_->shutdown(
            reason.isEmpty() ? QStringLiteral("host.worker_context.detached") : reason);
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
