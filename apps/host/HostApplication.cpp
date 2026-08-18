#include "HostApplication.h"

#include "MainWindow.h"

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

    RouteRegistry routes;
    const RouteRecord worker{QStringLiteral("/dashboard"),
                             Engine::QmlWorker,
                             QStringLiteral("com.qbrowser.pilot"),
                             QStringLiteral("qml/Main.qml")};
    const RouteRecord web{QStringLiteral("/web/help"),
                          Engine::WebEngine,
                          QStringLiteral("com.qbrowser.web"),
                          mockOrigin_.resolved(QUrl(QStringLiteral("help")))
                              .toString(QUrl::FullyEncoded)};
    if (routes.add(worker) != RouteAddResult::Added
        || routes.add(web) != RouteAddResult::Added) {
        return false;
    }

    auto window = std::make_unique<MainWindow>(std::move(routes), mockOrigin_);
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
