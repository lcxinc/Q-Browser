#include "MainWindow.h"

#include "AppUrl.h"
#include "NavigationBar.h"
#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWebEnginePage>

#include <utility>

namespace {

QString canonicalAppUrl(const AppUrl &url)
{
    QString canonical = QStringLiteral("app://pilot") + url.path();
    if (!url.query().isEmpty()) {
        canonical += u'?' + url.query();
    }
    return canonical;
}

QVariantMap variantParameters(const QHash<QString, QString> &parameters)
{
    QVariantMap result;
    for (auto iterator = parameters.constBegin(); iterator != parameters.constEnd(); ++iterator) {
        result.insert(iterator.key(), iterator.value());
    }
    return result;
}

} // namespace

MainWindow::MainWindow(RouteRegistry routeRegistry,
                       const QUrl &mockOrigin,
                       WorkerSurface *workerSurface,
                       QWidget *parent)
    : QMainWindow(parent)
    , routes_(std::move(routeRegistry))
    , workerSurface_(workerSurface)
{
    setObjectName(QStringLiteral("qbrowser-main-window"));
    setWindowTitle(QStringLiteral("Q-Browser"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    navigationBar_ = new NavigationBar(central);
    surfaceStack_ = new QStackedWidget(central);
    surfaceStack_->setObjectName(QStringLiteral("surface-stack"));
    webSurface_ = new WebSurface(mockOrigin, surfaceStack_);
    webSurface_->setObjectName(QStringLiteral("web-surface"));

    trustedErrorSurface_ = new QWidget(surfaceStack_);
    trustedErrorSurface_->setObjectName(QStringLiteral("trusted-error-surface"));
    auto *errorLayout = new QVBoxLayout(trustedErrorSurface_);
    trustedErrorLabel_ = new QLabel(QStringLiteral("No route selected."), trustedErrorSurface_);
    trustedErrorLabel_->setObjectName(QStringLiteral("trusted-error-message"));
    trustedErrorLabel_->setAlignment(Qt::AlignCenter);
    trustedErrorLabel_->setWordWrap(true);
    errorLayout->addWidget(trustedErrorLabel_);

    if (workerSurface_ != nullptr) {
        workerSurface_->setObjectName(QStringLiteral("worker-surface"));
        surfaceStack_->addWidget(workerSurface_);
    }
    surfaceStack_->addWidget(webSurface_);
    surfaceStack_->addWidget(trustedErrorSurface_);
    surfaceStack_->setCurrentWidget(trustedErrorSurface_);

    layout->addWidget(navigationBar_);
    layout->addWidget(surfaceStack_, 1);
    setCentralWidget(central);

    connect(navigationBar_, &NavigationBar::navigateRequested, this,
            [this](const QString &address) { (void)navigate(address); });
    connect(navigationBar_, &NavigationBar::backRequested, this,
            [this] { (void)goBack(); });
    connect(navigationBar_, &NavigationBar::forwardRequested, this,
            [this] { (void)goForward(); });
    updateNavigationState();
}

bool MainWindow::navigate(const QStringView input)
{
    const AppUrl parsed = AppUrl::parse(input, QStringLiteral("pilot"));
    if (!parsed.isValid()) {
        showTrustedError(QStringLiteral("The address is not a valid Q-Browser route."));
        navigationBar_->setAddressText(currentAppUrl_);
        return false;
    }
    const QString canonical = canonicalAppUrl(parsed);
    if (historyIndex_ + 1 < history_.size()) {
        history_.erase(history_.begin() + historyIndex_ + 1, history_.end());
    }
    history_.append(canonical);
    if (history_.size() > maximumHistoryEntries) {
        history_.removeFirst();
    }
    historyIndex_ = history_.size() - 1;
    setCurrentAppUrl(canonical);
    const bool activated = activate(canonical);
    updateNavigationState();
    return activated;
}

bool MainWindow::goBack()
{
    if (historyIndex_ <= 0) {
        return false;
    }
    --historyIndex_;
    const QString url = history_.at(historyIndex_);
    setCurrentAppUrl(url);
    (void)activate(url);
    updateNavigationState();
    return true;
}

bool MainWindow::goForward()
{
    if (historyIndex_ < 0 || historyIndex_ + 1 >= history_.size()) {
        return false;
    }
    ++historyIndex_;
    const QString url = history_.at(historyIndex_);
    setCurrentAppUrl(url);
    (void)activate(url);
    updateNavigationState();
    return true;
}

HostSurfaceKind MainWindow::activeSurface() const noexcept { return activeSurface_; }

int MainWindow::activeSurfaceCount() const
{
    int visible = 0;
    for (int index = 0; index < surfaceStack_->count(); ++index) {
        QWidget *surface = surfaceStack_->widget(index);
        if (!surface->isHidden() && surface->isVisibleTo(surfaceStack_)) {
            ++visible;
        }
    }
    return visible;
}

QString MainWindow::currentAppUrl() const { return currentAppUrl_; }
int MainWindow::historyCount() const noexcept { return history_.size(); }
int MainWindow::historyIndex() const noexcept { return historyIndex_; }
QString MainWindow::trustedErrorText() const { return trustedErrorLabel_->text(); }
NavigationBar *MainWindow::navigationBar() const noexcept { return navigationBar_; }
QStackedWidget *MainWindow::surfaceStack() const noexcept { return surfaceStack_; }
WebSurface *MainWindow::webSurface() const noexcept { return webSurface_; }

bool MainWindow::activate(const QString &canonicalUrl)
{
    const AppUrl parsed = AppUrl::parse(canonicalUrl, QStringLiteral("pilot"));
    if (!parsed.isValid()) {
        showTrustedError(QStringLiteral("The address is not a valid Q-Browser route."));
        return false;
    }
    const RouteMatch match = routes_.match(parsed.path());
    if (!match.isValid()) {
        showTrustedError(QStringLiteral("Route not found."));
        return false;
    }

    switch (match.record.engine) {
    case Engine::QmlWorker:
        if (workerSurface_ == nullptr || !workerSurface_->isValid()) {
            showTrustedError(QStringLiteral("The package worker is unavailable."));
            return false;
        }
        surfaceStack_->setCurrentWidget(workerSurface_);
        if (webSurface_->page() != nullptr) {
            webSurface_->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
        }
        activeSurface_ = HostSurfaceKind::Worker;
        emit workerRouteRequested(match.record.packageId,
                                  match.record.entryPoint,
                                  variantParameters(match.parameters),
                                  QUrl(canonicalUrl));
        return true;
    case Engine::WebEngine: {
        surfaceStack_->setCurrentWidget(webSurface_);
        activeSurface_ = HostSurfaceKind::Web;
        if (webSurface_->page() != nullptr) {
            webSurface_->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
        }
        const QUrl target(match.record.entryPoint, QUrl::StrictMode);
        return webSurface_->navigate(target);
    }
    case Engine::TrustedQml:
        showTrustedError(QStringLiteral("The trusted page is unavailable."));
        return false;
    case Engine::Invalid:
        break;
    }
    showTrustedError(QStringLiteral("Route not found."));
    return false;
}

void MainWindow::showTrustedError(const QString &message)
{
    trustedErrorLabel_->setText(message);
    surfaceStack_->setCurrentWidget(trustedErrorSurface_);
    activeSurface_ = HostSurfaceKind::TrustedError;
    if (webSurface_->page() != nullptr) {
        webSurface_->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
    }
}

void MainWindow::setCurrentAppUrl(const QString &url)
{
    currentAppUrl_ = url;
    navigationBar_->setAddressText(url);
    emit currentUrlChanged(url);
}

void MainWindow::updateNavigationState()
{
    navigationBar_->setNavigationAvailability(historyIndex_ > 0,
                                               historyIndex_ >= 0
                                                   && historyIndex_ + 1 < history_.size());
}
