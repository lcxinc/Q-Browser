#pragma once

#include "RouteRegistry.h"

#include <QMainWindow>
#include <QUrl>

class QLabel;
class NavigationBar;
class QStackedWidget;
class WebSurface;
class WorkerSurface;

enum class HostSurfaceKind
{
    Worker,
    Web,
    TrustedError,
};

Q_DECLARE_METATYPE(HostSurfaceKind)

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RouteRegistry routeRegistry,
               const QUrl &mockOrigin,
               WorkerSurface *workerSurface = nullptr,
               QWidget *parent = nullptr);

    [[nodiscard]] bool navigate(QStringView input);
    [[nodiscard]] bool goBack();
    [[nodiscard]] bool goForward();

    [[nodiscard]] HostSurfaceKind activeSurface() const noexcept;
    [[nodiscard]] int activeSurfaceCount() const;
    [[nodiscard]] QString currentAppUrl() const;
    [[nodiscard]] int historyCount() const noexcept;
    [[nodiscard]] int historyIndex() const noexcept;
    [[nodiscard]] QString trustedErrorText() const;
    [[nodiscard]] NavigationBar *navigationBar() const noexcept;
    [[nodiscard]] QStackedWidget *surfaceStack() const noexcept;
    [[nodiscard]] WebSurface *webSurface() const noexcept;

signals:
    void currentUrlChanged(const QString &url);
    void workerRouteRequested(const QString &packageId,
                              const QString &entryPoint,
                              const QVariantMap &parameters,
                              const QUrl &appUrl);

private:
    [[nodiscard]] bool activate(const QString &canonicalUrl);
    void showTrustedError(const QString &message);
    void setCurrentAppUrl(const QString &url);
    void updateNavigationState();

    static constexpr int maximumHistoryEntries = 256;

    RouteRegistry routes_;
    NavigationBar *navigationBar_ = nullptr;
    QStackedWidget *surfaceStack_ = nullptr;
    WorkerSurface *workerSurface_ = nullptr;
    WebSurface *webSurface_ = nullptr;
    QWidget *trustedErrorSurface_ = nullptr;
    QLabel *trustedErrorLabel_ = nullptr;
    QStringList history_;
    int historyIndex_ = -1;
    QString currentAppUrl_;
    HostSurfaceKind activeSurface_ = HostSurfaceKind::TrustedError;
    bool navigationInProgress_ = false;
};
