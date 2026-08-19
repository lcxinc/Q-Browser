#include "HostApplication.h"

#include "MainWindow.h"

#include "PilotRoutes.h"
#include "RouteRegistry.h"
#include "WebSurface.h"

#include <utility>

HostApplication::HostApplication(QUrl mockOrigin, QObject *parent)
    : QObject(parent), mockOrigin_(std::move(mockOrigin))
{
}

HostApplication::~HostApplication() = default;

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
    return true;
}

MainWindow *HostApplication::mainWindow() const noexcept
{
    return mainWindow_.get();
}
