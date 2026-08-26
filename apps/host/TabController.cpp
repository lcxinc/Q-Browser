#include "TabController.h"

#include "NewTabPage.h"
#include "WebSessionProfile.h"
#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QLabel>
#include <QPointer>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWebEnginePage>

#include <utility>

TabController::TabController(QString tabId,
                             BrowserTabModel *model,
                             QStackedWidget *surfaceStack,
                             WebSessionProfile *webSessionProfile,
                             QObject *parent)
    : QObject(parent)
    , tabId_(std::move(tabId))
    , model_(model)
    , surfaceStack_(surfaceStack)
    , webSessionProfile_(webSessionProfile)
{
    Q_ASSERT(!tabId_.isEmpty());
    Q_ASSERT(model_ != nullptr);
    Q_ASSERT(surfaceStack_ != nullptr);
    Q_ASSERT(webSessionProfile_ != nullptr);
}

TabController::~TabController()
{
    (void)retire();
}

QString TabController::tabId() const
{
    return tabId_;
}

quint64 TabController::incarnation() const noexcept
{
    return incarnation_;
}

BrowserTabLifecycle TabController::lifecycle() const noexcept
{
    return lifecycle_;
}

HostSurfaceKind TabController::surfaceKind() const noexcept
{
    return surfaceKind_;
}

QWidget *TabController::currentSurface() const noexcept
{
    return currentSurface_;
}

NewTabPage *TabController::hostSurface() const noexcept
{
    return hostSurface_;
}

WebSurface *TabController::webSurface() const noexcept
{
    return webSurface_;
}

WorkerSurface *TabController::workerSurface() const noexcept
{
    return workerSurface_;
}

QString TabController::workerPackageId() const
{
    return workerPackageId_;
}

QString TabController::trustedErrorText() const
{
    return trustedErrorLabel_ != nullptr ? trustedErrorLabel_->text() : QString{};
}

void TabController::setActive(const bool active)
{
    if (retired_) return;
    active_ = active;
    updateVisibility();
    if (lifecycle_ == BrowserTabLifecycle::Dormant
        || lifecycle_ == BrowserTabLifecycle::Starting
        || lifecycle_ == BrowserTabLifecycle::Loading
        || lifecycle_ == BrowserTabLifecycle::TrustedError) {
        return;
    }
    transitionTo(active_ ? BrowserTabLifecycle::Active
                         : BrowserTabLifecycle::Background);
}

quint64 TabController::beginNavigation()
{
    if (!canTransitionResources()) return 0;
    const int index = model_->indexOfId(tabId_);
    Q_ASSERT(index >= 0);
    lifecycle_ = model_->lifecycleAt(index);
    advanceIncarnation();
    return incarnation_;
}

bool TabController::startHost(const quint64 navigationIncarnation)
{
    if (!isCurrentNavigation(navigationIncarnation)) return false;
    transitionTo(BrowserTabLifecycle::Starting);
    (void)model_->setLoadState(tabId_, false, 0);
    (void)model_->setVisualState(tabId_, BrowserVisualState::Normal);
    destroyTrustedErrorSurface();
    destroyWebSurface();
    if (hostSurface_ == nullptr) {
        hostSurface_ = new NewTabPage(surfaceStack_);
        surfaceStack_->addWidget(hostSurface_);
    }
    connectHostSignals();
    currentSurface_ = hostSurface_;
    surfaceKind_ = HostSurfaceKind::Host;
    workerPackageId_.clear();
    updateVisibility();
    transitionTo(active_ ? BrowserTabLifecycle::Active
                         : BrowserTabLifecycle::Background);
    return true;
}

bool TabController::startWeb(const QUrl &physicalEntry,
                             const quint64 navigationIncarnation,
                             const bool reloadExisting)
{
    if (!isCurrentNavigation(navigationIncarnation)
        || !physicalEntry.isValid()) {
        return false;
    }
    if (webSurface_ != nullptr && failedWebSurface_ == webSurface_
        && webEntry_ == physicalEntry) {
        return false;
    }
    transitionTo(BrowserTabLifecycle::Starting);
    (void)model_->setVisualState(tabId_, BrowserVisualState::Normal);
    // Loading is published only after WebEngine reports its own started
    // transition.  Publishing it speculatively would enable Stop before the
    // page has a load to cancel and leave stale chrome state behind.
    (void)model_->setLoadState(tabId_, false, 0);
    destroyTrustedErrorSurface();
    destroyHostSurface();
    workerPackageId_.clear();

    if (webSurface_ != nullptr && webEntry_ != physicalEntry) {
        destroyWebSurface();
    }
    if (webSurface_ == nullptr) {
        auto *const candidate = new WebSurface(
            *webSessionProfile_, physicalEntry, surfaceStack_);
        if (!candidate->isConfigurationValid()) {
            delete candidate;
            enterTrustedError(QStringLiteral("The web content is unavailable."),
                              false, false);
            return false;
        }
        candidate->setObjectName(QStringLiteral("web-surface"));
        webSurface_ = candidate;
        webEntry_ = physicalEntry;
        surfaceStack_->addWidget(webSurface_);
    }

    connectWebSignals();
    currentSurface_ = webSurface_;
    surfaceKind_ = HostSurfaceKind::Web;
    updateVisibility();
    transitionTo(BrowserTabLifecycle::Loading);
    const bool started = reloadExisting ? webSurface_->reload()
                                        : webSurface_->navigate(physicalEntry);
    if (!started) {
        enterTrustedError(QStringLiteral("The web content is unavailable."),
                          true, false);
        return false;
    }
    return true;
}

bool TabController::startLegacyApp(const QString &packageId,
                                   const QString &entryPoint,
                                   const QVariantMap &parameters,
                                   const QUrl &logicalUrl,
                                   const quint64 navigationIncarnation)
{
    if (!isCurrentNavigation(navigationIncarnation) || packageId.isEmpty()
        || entryPoint.isEmpty() || !logicalUrl.isValid()) {
        return false;
    }
    transitionTo(BrowserTabLifecycle::Starting);
    (void)model_->setLoadState(tabId_, false, 0);
    (void)model_->setVisualState(tabId_, BrowserVisualState::Normal);
    destroyTrustedErrorSurface();
    destroyHostSurface();
    destroyWebSurface();
    if (workerSurface_ == nullptr || !workerSurface_->isValid()) {
        enterTrustedError(
            QStringLiteral("The package worker is unavailable."), false, false);
        return false;
    }

    workerPackageId_ = packageId;
    currentSurface_ = workerSurface_;
    surfaceKind_ = HostSurfaceKind::Worker;
    updateVisibility();
    emit workerRouteRequested(packageId, entryPoint, parameters, logicalUrl);
    if (!isCurrentNavigation(navigationIncarnation)
        || workerSurface_ == nullptr || currentSurface_ != workerSurface_
        || surfaceKind_ != HostSurfaceKind::Worker) {
        return false;
    }
    transitionTo(active_ ? BrowserTabLifecycle::Active
                         : BrowserTabLifecycle::Background);
    return true;
}

void TabController::stop()
{
    if (retired_ || webSurface_ == nullptr
        || currentSurface_ != webSurface_) {
        return;
    }
    webSurface_->stop();
}

void TabController::showTrustedError(const QString &plainText,
                                     const bool retireWebSurface)
{
    enterTrustedError(plainText, retireWebSurface, true);
}

void TabController::showTrustedErrorForNavigation(
    const quint64 navigationIncarnation,
    const QString &plainText,
    const bool retireWebSurface)
{
    if (!isCurrentNavigation(navigationIncarnation)) return;
    enterTrustedError(plainText, retireWebSurface, false);
}

bool TabController::attachLegacyWorkerSurface(
    std::unique_ptr<WorkerSurface> surface)
{
    if (!canTransitionResources() || surface == nullptr
        || workerSurface_ != nullptr || !surface->isValid()) {
        return false;
    }
    workerSurface_ = surface.release();
    workerSurface_->setParent(surfaceStack_);
    workerSurface_->setObjectName(QStringLiteral("worker-surface"));
    surfaceStack_->addWidget(workerSurface_);
    workerSurface_->hide();
    return true;
}

void TabController::detachLegacyWorkerSurface()
{
    if (workerSurface_ == nullptr) return;
    const bool wasCurrent = currentSurface_ == workerSurface_;
    surfaceStack_->removeWidget(workerSurface_);
    delete workerSurface_;
    workerSurface_ = nullptr;
    workerPackageId_.clear();
    if (wasCurrent) {
        currentSurface_ = nullptr;
        if (!retired_ && lifecycle_ != BrowserTabLifecycle::Closing) {
            enterTrustedError(
                QStringLiteral("The package worker is unavailable."),
                false, true);
        }
    }
}

bool TabController::beginClosing()
{
    if (retired_ || lifecycle_ == BrowserTabLifecycle::Retired) return false;
    if (lifecycle_ == BrowserTabLifecycle::Closing) return true;
    advanceIncarnation();
    active_ = false;
    transitionTo(BrowserTabLifecycle::Closing);
    updateVisibility();
    return true;
}

bool TabController::retire()
{
    if (retired_) return true;
    if (!beginClosing()) return false;
    destroyTrustedErrorSurface();
    destroyHostSurface();
    destroyWebSurface();
    if (workerSurface_ != nullptr) {
        surfaceStack_->removeWidget(workerSurface_);
        delete workerSurface_;
        workerSurface_ = nullptr;
    }
    currentSurface_ = nullptr;
    workerPackageId_.clear();
    retired_ = true;
    transitionTo(BrowserTabLifecycle::Retired);
    return true;
}

bool TabController::canTransitionResources() const noexcept
{
    return !retired_ && model_ != nullptr && surfaceStack_ != nullptr
        && webSessionProfile_ != nullptr
        && lifecycle_ != BrowserTabLifecycle::Closing
        && lifecycle_ != BrowserTabLifecycle::Retired
        && model_->indexOfId(tabId_) >= 0;
}

bool TabController::isCurrentNavigation(
    const quint64 navigationIncarnation) const noexcept
{
    return navigationIncarnation != 0
        && navigationIncarnation == incarnation_
        && canTransitionResources();
}

void TabController::advanceIncarnation()
{
    ++incarnation_;
}

void TabController::transitionTo(const BrowserTabLifecycle lifecycle)
{
    lifecycle_ = lifecycle;
    if (model_ != nullptr) (void)model_->setLifecycle(tabId_, lifecycle);
}

void TabController::updateVisibility()
{
    if (hostSurface_ != nullptr) hostSurface_->hide();
    if (trustedErrorSurface_ != nullptr) trustedErrorSurface_->hide();
    if (workerSurface_ != nullptr) workerSurface_->hide();
    if (webSurface_ != nullptr) webSurface_->setTabActive(false);
    if (!active_ || currentSurface_ == nullptr || surfaceStack_ == nullptr) {
        return;
    }

    surfaceStack_->setCurrentWidget(currentSurface_);
    if (currentSurface_ == webSurface_) {
        webSurface_->setTabActive(true);
    } else {
        currentSurface_->show();
    }
}

void TabController::connectHostSignals()
{
    if (hostSurface_ == nullptr) return;
    disconnect(hostSurface_, nullptr, this, nullptr);
    const QString stableId = tabId_;
    const quint64 capturedIncarnation = incarnation_;
    connect(hostSurface_, &NewTabPage::addressActivated, this,
            [this, stableId, capturedIncarnation](const QString &address) {
                if (retired_ || stableId != tabId_
                    || capturedIncarnation != incarnation_
                    || hostSurface_ == nullptr) {
                    return;
                }
                emit addressActivated(stableId, capturedIncarnation, address);
            });
}

void TabController::connectWebSignals()
{
    if (webSurface_ == nullptr) return;
    disconnect(webSurface_, nullptr, this, nullptr);
    const QString stableId = tabId_;
    const quint64 capturedIncarnation = incarnation_;
    const QPointer<WebSurface> capturedSurface(webSurface_);
    const auto isCurrent = [this, stableId, capturedIncarnation,
                            capturedSurface] {
        return !retired_ && stableId == tabId_
            && capturedIncarnation == incarnation_
            && capturedSurface && capturedSurface == webSurface_;
    };

    connect(webSurface_, &WebSurface::titleChanged, this,
            [this, isCurrent](const QString &title) {
                if (!isCurrent()) return;
                withResourceMutation([this, isCurrent, title] {
                    if (isCurrent()) (void)model_->setTitle(tabId_, title);
                });
            });
    connect(webSurface_, &WebSurface::loadingChanged, this,
            [this, isCurrent](const bool loading) {
                if (!isCurrent()) return;
                withResourceMutation([this, isCurrent, loading] {
                    if (!isCurrent()) return;
                    WebSurface *const observedSurface = webSurface_;
                    const int progress = observedSurface != nullptr
                        ? observedSurface->loadProgress() : 0;
                    if (!model_->setLoadState(tabId_, loading, progress)
                        || !isCurrent()
                        || webSurface_ != observedSurface) {
                        return;
                    }
                    if (loading) {
                        transitionTo(BrowserTabLifecycle::Loading);
                    } else if (lifecycle_
                               != BrowserTabLifecycle::TrustedError) {
                        transitionTo(active_ ? BrowserTabLifecycle::Active
                                             : BrowserTabLifecycle::Background);
                    }
                });
            });
    connect(webSurface_, &WebSurface::loadProgressChanged, this,
            [this, isCurrent](const int progress) {
                if (!isCurrent()) return;
                withResourceMutation([this, isCurrent, progress] {
                    if (isCurrent() && webSurface_ != nullptr) {
                        (void)model_->setLoadState(
                            tabId_, webSurface_->isLoading(), progress);
                    }
                });
            });
    connect(webSurface_, &WebSurface::navigationFinished, this,
            [this, isCurrent](const QUrl &, const bool) {
                if (!isCurrent()) return;
                withResourceMutation([this, isCurrent] {
                    if (!isCurrent()) return;
                    WebSurface *const observedSurface = webSurface_;
                    const int progress = observedSurface != nullptr
                        ? observedSurface->loadProgress() : 0;
                    if (!model_->setLoadState(tabId_, false, progress)
                        || !isCurrent()
                        || webSurface_ != observedSurface) {
                        return;
                    }
                    if (lifecycle_ != BrowserTabLifecycle::TrustedError) {
                        transitionTo(active_ ? BrowserTabLifecycle::Active
                                             : BrowserTabLifecycle::Background);
                    }
                });
            });
    connect(webSurface_, &WebSurface::rendererFailed, this,
            [this, stableId, capturedIncarnation, capturedSurface](
                QWebEnginePage::RenderProcessTerminationStatus, int) {
                if (retired_ || stableId != tabId_
                    || capturedIncarnation != incarnation_
                    || !capturedSurface || capturedSurface != webSurface_) {
                    return;
                }
                failedWebSurface_ = capturedSurface;
                const bool queued = QMetaObject::invokeMethod(
                    this,
                    [this, stableId, capturedSurface] {
                        if (retired_ || stableId != tabId_
                            || !capturedSurface
                            || capturedSurface != webSurface_
                            || failedWebSurface_ != capturedSurface) {
                            return;
                        }
                        withResourceMutation(
                            [this, stableId, capturedSurface] {
                                if (retired_ || stableId != tabId_
                                    || !capturedSurface
                                    || capturedSurface != webSurface_
                                    || failedWebSurface_ != capturedSurface) {
                                    return;
                                }
                                WebSurface *const failedSurface =
                                    capturedSurface.data();
                                disconnect(failedSurface, nullptr, this, nullptr);
                                if (currentSurface_ == failedSurface) {
                                    currentSurface_ = nullptr;
                                }
                                webSurface_ = nullptr;
                                webEntry_ = {};
                                failedWebSurface_.clear();
                                advanceIncarnation();
                                enterTrustedError(
                                    QStringLiteral(
                                        "The web renderer terminated unexpectedly."),
                                    false, false);
                                failedSurface->setTabActive(false);
                                const bool pageRetired = failedSurface->shutdown();
                                Q_ASSERT(pageRetired);
                                surfaceStack_->removeWidget(failedSurface);
                                delete failedSurface;
                            });
                    },
                    Qt::QueuedConnection);
                Q_ASSERT(queued);
            }, Qt::DirectConnection);
}

void TabController::withResourceMutation(
    const std::function<void()> &mutation)
{
    if (retired_ || !mutation) return;
    if (resourceMutationInProgress_) {
        mutation();
        return;
    }
    resourceMutationInProgress_ = true;
    emit resourceMutationStarted();
    mutation();
    resourceMutationInProgress_ = false;
    emit resourceMutationFinished();
}

void TabController::enterTrustedError(const QString &plainText,
                                      const bool retireWebSurface,
                                      const bool advance)
{
    if (!canTransitionResources()) return;
    withResourceMutation([this, plainText, retireWebSurface, advance] {
        if (!canTransitionResources()) return;
        if (advance) advanceIncarnation();
        if (retireWebSurface) destroyWebSurface();
        if (webSurface_ != nullptr) {
            webSurface_->stop();
            webSurface_->setTabActive(false);
        }
        if (hostSurface_ != nullptr) hostSurface_->hide();
        if (workerSurface_ != nullptr) workerSurface_->hide();
        destroyTrustedErrorSurface();

        trustedErrorSurface_ = new QWidget(surfaceStack_);
        trustedErrorSurface_->setObjectName(
            QStringLiteral("trusted-error-surface"));
        auto *const layout = new QVBoxLayout(trustedErrorSurface_);
        trustedErrorLabel_ = new QLabel(plainText, trustedErrorSurface_);
        trustedErrorLabel_->setObjectName(
            QStringLiteral("trusted-error-message"));
        trustedErrorLabel_->setTextFormat(Qt::PlainText);
        trustedErrorLabel_->setAlignment(Qt::AlignCenter);
        trustedErrorLabel_->setWordWrap(true);
        layout->addWidget(trustedErrorLabel_);
        surfaceStack_->addWidget(trustedErrorSurface_);
        currentSurface_ = trustedErrorSurface_;
        surfaceKind_ = HostSurfaceKind::TrustedError;
        updateVisibility();
        if (!model_->setLoadState(tabId_, false, 0)
            || !canTransitionResources()) {
            return;
        }
        if (!model_->setVisualState(
                tabId_, BrowserVisualState::TrustedError)
            || !canTransitionResources()) {
            return;
        }
        transitionTo(BrowserTabLifecycle::TrustedError);
    });
}

void TabController::destroyHostSurface()
{
    if (hostSurface_ == nullptr) return;
    if (currentSurface_ == hostSurface_) currentSurface_ = nullptr;
    surfaceStack_->removeWidget(hostSurface_);
    delete hostSurface_;
    hostSurface_ = nullptr;
}

void TabController::destroyTrustedErrorSurface()
{
    if (trustedErrorSurface_ == nullptr) return;
    if (currentSurface_ == trustedErrorSurface_) currentSurface_ = nullptr;
    surfaceStack_->removeWidget(trustedErrorSurface_);
    delete trustedErrorSurface_;
    trustedErrorSurface_ = nullptr;
    trustedErrorLabel_ = nullptr;
}

void TabController::destroyWebSurface()
{
    if (webSurface_ == nullptr) return;
    failedWebSurface_.clear();
    if (currentSurface_ == webSurface_) currentSurface_ = nullptr;
    disconnect(webSurface_, nullptr, this, nullptr);
    webSurface_->setTabActive(false);
    surfaceStack_->removeWidget(webSurface_);
    (void)webSurface_->shutdown();
    delete webSurface_;
    webSurface_ = nullptr;
    webEntry_ = {};
}
