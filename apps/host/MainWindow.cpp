#include "MainWindow.h"

#include "AppUrl.h"
#include "NavigationBar.h"
#include "WebSessionProfile.h"
#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QLabel>
#include <QCoreApplication>
#include <QScopedValueRollback>
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
    , webSessionProfile_(std::make_unique<WebSessionProfile>(mockOrigin))
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
    webSurface_ = new WebSurface(*webSessionProfile_,
                                 mockOrigin.resolved(QUrl(QStringLiteral("help"))),
                                 surfaceStack_);
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
    webSurface_->setTabActive(false);

    layout->addWidget(navigationBar_);
    layout->addWidget(surfaceStack_, 1);
    setCentralWidget(central);

    connect(navigationBar_, &NavigationBar::navigateRequested, this,
            [this](const QString &address) { (void)navigate(address); });
    connect(navigationBar_, &NavigationBar::backRequested, this,
            [this] { (void)goBack(); });
    connect(navigationBar_, &NavigationBar::forwardRequested, this,
            [this] { (void)goForward(); });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { (void)shutdown(); });
    updateNavigationState();
}

MainWindow::~MainWindow()
{
    (void)shutdown();
}

bool MainWindow::shutdown()
{
    if (shutdown_) return shutdownSucceeded_;
    shutdown_ = true;
    hide();
    if (webSurface_ != nullptr) {
        webSurface_->stop();
        if (surfaceStack_ != nullptr) surfaceStack_->removeWidget(webSurface_);
        shutdownSucceeded_ = webSurface_->shutdown();
        delete webSurface_;
        webSurface_ = nullptr;
    }
    if (webSessionProfile_ != nullptr) {
        if (webSessionProfile_->registeredPageCount() != 0) {
            shutdownSucceeded_ = false;
        }
        const bool profileShutdown = webSessionProfile_->shutdown();
        shutdownSucceeded_ = profileShutdown && shutdownSucceeded_;
        if (profileShutdown) webSessionProfile_.reset();
    }
    return shutdownSucceeded_;
}

bool MainWindow::navigate(const QStringView input)
{
    if (navigationInProgress_) {
        return false;
    }
    QScopedValueRollback transaction(navigationInProgress_, true);

    const AppUrl parsed = AppUrl::parse(input, QStringLiteral("pilot"));
    if (!parsed.isValid()) {
        showTrustedError(QStringLiteral("The address is not a valid Q-Browser route."));
        navigationBar_->setAddressText(currentAppUrl_);
        return false;
    }
    const QString canonical = canonicalAppUrl(parsed);
    const bool isCurrentEntry = historyIndex_ >= 0
        && historyIndex_ < history_.size()
        && history_.at(historyIndex_) == canonical;
    if (!isCurrentEntry) {
        if (historyIndex_ + 1 < history_.size()) {
            history_.erase(history_.begin() + historyIndex_ + 1, history_.end());
        }
        history_.append(canonical);
        if (history_.size() > maximumHistoryEntries) {
            history_.removeFirst();
        }
        historyIndex_ = history_.size() - 1;
    }
    const bool urlChanged = currentAppUrl_ != canonical;
    setCurrentAppUrl(canonical);
    updateNavigationState();
    const bool activated = activate(canonical);
    if (urlChanged) {
        emit currentUrlChanged(canonical);
    }
    return activated;
}

bool MainWindow::navigateFromWorker(const QString &packageId, const QString &route)
{
    if (activeSurface_ != HostSurfaceKind::Worker || packageId.isEmpty()
        || packageId != activeWorkerPackageId_ || !route.startsWith(u'/')
        || route.startsWith(QStringLiteral("//"))) {
        return false;
    }
    const AppUrl parsed = AppUrl::parse(QStringLiteral("app://pilot") + route,
                                        QStringLiteral("pilot"));
    if (!parsed.isValid()) {
        return false;
    }
    const RouteMatch match = routes_.match(parsed.path());
    if (!match.isValid() || match.record.engine != Engine::QmlWorker
        || match.record.packageId != packageId) {
        return false;
    }
    return navigate(canonicalAppUrl(parsed));
}

bool MainWindow::goBack()
{
    if (navigationInProgress_ || historyIndex_ <= 0) {
        return false;
    }
    QScopedValueRollback transaction(navigationInProgress_, true);

    --historyIndex_;
    const QString url = history_.at(historyIndex_);
    setCurrentAppUrl(url);
    updateNavigationState();
    (void)activate(url);
    emit currentUrlChanged(url);
    return true;
}

bool MainWindow::goForward()
{
    if (navigationInProgress_
        || historyIndex_ < 0
        || historyIndex_ + 1 >= history_.size()) {
        return false;
    }
    QScopedValueRollback transaction(navigationInProgress_, true);

    ++historyIndex_;
    const QString url = history_.at(historyIndex_);
    setCurrentAppUrl(url);
    updateNavigationState();
    (void)activate(url);
    emit currentUrlChanged(url);
    return true;
}

bool MainWindow::attachWorkerSurface(std::unique_ptr<WorkerSurface> surface)
{
    if (workerSurface_ != nullptr || surface == nullptr || !surface->isValid()) {
        return false;
    }
    workerSurface_ = surface.release();
    workerSurface_->setParent(surfaceStack_);
    workerSurface_->setObjectName(QStringLiteral("worker-surface"));
    surfaceStack_->insertWidget(0, workerSurface_);
    return true;
}

void MainWindow::detachWorkerSurface()
{
    if (workerSurface_ == nullptr) return;
    if (surfaceStack_->currentWidget() == workerSurface_)
        showTrustedError(QStringLiteral("The package worker is unavailable."));
    surfaceStack_->removeWidget(workerSurface_);
    delete workerSurface_;
    workerSurface_ = nullptr;
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
WebSessionProfile *MainWindow::webSessionProfile() const noexcept
{
    return webSessionProfile_.get();
}
WebSurface *MainWindow::webSurface() const noexcept { return webSurface_; }
WorkerSurface *MainWindow::workerSurface() const noexcept { return workerSurface_; }

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
        webSurface_->setTabActive(false);
        surfaceStack_->setCurrentWidget(workerSurface_);
        activeSurface_ = HostSurfaceKind::Worker;
        activeWorkerPackageId_ = match.record.packageId;
        emit workerRouteRequested(match.record.packageId,
                                  match.record.entryPoint,
                                  variantParameters(match.parameters),
                                  QUrl(canonicalUrl));
        return true;
    case Engine::WebEngine: {
        webSurface_->setTabActive(true);
        surfaceStack_->setCurrentWidget(webSurface_);
        activeSurface_ = HostSurfaceKind::Web;
        activeWorkerPackageId_.clear();
        const QUrl target(match.record.entryPoint, QUrl::StrictMode);
        if (!webSurface_->navigate(target)) {
            showTrustedError(QStringLiteral("The web content is unavailable."));
            return false;
        }
        return true;
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
    if (webSurface_ != nullptr) webSurface_->setTabActive(false);
    surfaceStack_->setCurrentWidget(trustedErrorSurface_);
    activeSurface_ = HostSurfaceKind::TrustedError;
    activeWorkerPackageId_.clear();
}

void MainWindow::setCurrentAppUrl(const QString &url)
{
    currentAppUrl_ = url;
    navigationBar_->setAddressText(url);
}

void MainWindow::updateNavigationState()
{
    navigationBar_->setNavigationAvailability(historyIndex_ > 0,
                                               historyIndex_ >= 0
                                                   && historyIndex_ + 1 < history_.size());
}
