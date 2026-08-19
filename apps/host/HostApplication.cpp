#include "HostApplication.h"

#include "MainWindow.h"
#include "HostWorkerSessionController.h"
#include "IpcSession.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSurface.h"

#include <utility>

HostApplication::HostApplication(QUrl mockOrigin, QObject *parent)
    : QObject(parent), mockOrigin_(std::move(mockOrigin))
{
}

HostApplication::~HostApplication()
{
    detachWorkerContext(QStringLiteral("host.application.stopping"));
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
    connect(workerSessionController_.get(), &HostWorkerSessionController::failed,
            this, [this] {
                detachWorkerContext(QStringLiteral("host.worker_session.failed"));
            });
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
