#include "WebSurface.h"

#include "PilotRequestInterceptor.h"
#include "WebSessionProfile.h"

#include <QCoreApplication>
#include <QProcess>
#include <QVBoxLayout>
#include <QWebEngineCertificateError>
#include <QWebEngineFileSystemAccessRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineRegisterProtocolHandlerRequest>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <functional>
#include <utility>

static void initializeHostResources()
{
    Q_INIT_RESOURCE(host);
}

namespace {

bool disablesSandbox(const QString &argument)
{
    QString normalized = argument.trimmed();
    while (!normalized.isEmpty()
           && (normalized.front() == u'"' || normalized.front() == u'\'')) {
        normalized.removeFirst();
    }
    while (!normalized.isEmpty()
           && (normalized.back() == u'"' || normalized.back() == u'\'')) {
        normalized.chop(1);
    }
    const QString token = normalized.section(u'=', 0, 0);
    return token == QStringLiteral("--no-sandbox")
        || token == QStringLiteral("--single-process")
        || (token.startsWith(QStringLiteral("--disable-"))
            && token.contains(QStringLiteral("sandbox")));
}

class PilotWebPage final : public QWebEnginePage
{
public:
    using DeniedCallback = std::function<void()>;
    using FileSelectionDeniedCallback = std::function<void(bool)>;

    PilotWebPage(QWebEngineProfile *profile,
                 const PilotRequestInterceptor *interceptor,
                 QUrl registeredMainFrameEntry,
                 DeniedCallback popupDenied,
                 FileSelectionDeniedCallback fileSelectionDenied,
                 QObject *parent)
        : QWebEnginePage(profile, parent)
        , interceptor_(interceptor)
        , registeredMainFrameEntry_(std::move(registeredMainFrameEntry))
        , popupDenied_(std::move(popupDenied))
        , fileSelectionDenied_(std::move(fileSelectionDenied))
    {
    }

protected:
    QWebEnginePage *createWindow(WebWindowType) override
    {
        popupDenied_();
        return nullptr;
    }

    bool acceptNavigationRequest(const QUrl &url,
                                 NavigationType type,
                                 const bool isMainFrame) override
    {
        const bool allowed = isMainFrame
            ? url == registeredMainFrameEntry_ || url == WebSurface::trustedErrorUrl()
            : interceptor_->isAllowed(url);
        return allowed
            && QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }

    QStringList chooseFiles(FileSelectionMode mode,
                            const QStringList &,
                            const QStringList &) override
    {
        fileSelectionDenied_(mode == FileSelectUploadFolder);
        return {};
    }

private:
    const PilotRequestInterceptor *interceptor_ = nullptr;
    QUrl registeredMainFrameEntry_;
    DeniedCallback popupDenied_;
    FileSelectionDeniedCallback fileSelectionDenied_;
};

} // namespace

WebSurface::WebSurface(WebSessionProfile &session,
                       QUrl registeredMainFrameEntry,
                       QWidget *parent)
    : QWidget(parent)
    , session_(&session)
    , registeredMainFrameEntry_(std::move(registeredMainFrameEntry))
{
    initializeHostResources();
    PilotRequestInterceptor *const interceptor = session_->requestInterceptor();
    if (!session_->isConfigurationValid() || interceptor == nullptr
        || !registeredMainFrameEntry_.isValid()
        || registeredMainFrameEntry_ == trustedErrorUrl()
        || !interceptor->isAllowed(registeredMainFrameEntry_)) {
        return;
    }

    auto page = std::make_unique<PilotWebPage>(
        session_->profile(), interceptor, registeredMainFrameEntry_,
        [this] { emit popupDenied(); },
        [this](const bool directorySelection) {
            emit fileSelectionDenied(directorySelection);
        },
        nullptr);
    if (!session_->registerPage(page.get())) return;
    page_ = std::move(page);

    QWebEngineSettings *const settings = page_->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    settings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
    settings->setAttribute(QWebEngineSettings::NavigateOnDropEnabled, false);
    settings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);

    view_ = std::make_unique<QWebEngineView>(this);
    view_->setPage(page_.get());
    auto *const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_.get());

    connect(session_, &WebSessionProfile::downloadDenied, this,
            [this](QWebEnginePage *downloadPage, const QUrl &url) {
                if (downloadPage == page_.get()) emit downloadDenied(url);
            });
    connect(page_.get(), &QWebEnginePage::permissionRequested, this,
            [this](const QWebEnginePermission &request) {
                const QUrl origin = request.origin();
                request.deny();
                emit permissionDenied(origin);
            });
    connect(page_.get(), &QWebEnginePage::fileSystemAccessRequested, this,
            [this](QWebEngineFileSystemAccessRequest request) {
                const QUrl origin = request.origin();
                request.reject();
                emit permissionDenied(origin);
            });
    connect(page_.get(), &QWebEnginePage::registerProtocolHandlerRequested, this,
            [this](QWebEngineRegisterProtocolHandlerRequest request) {
                const QUrl origin = request.origin();
                request.reject();
                emit permissionDenied(origin);
            });
    connect(page_.get(), &QWebEnginePage::fullScreenRequested, this,
            [this](QWebEngineFullScreenRequest request) {
                const QUrl origin = request.origin();
                request.reject();
                emit permissionDenied(origin);
            });
    connect(page_.get(), &QWebEnginePage::certificateError, this,
            [](QWebEngineCertificateError error) { error.rejectCertificate(); });
    connect(page_.get(), &QWebEnginePage::renderProcessTerminated, this,
            [this](const QWebEnginePage::RenderProcessTerminationStatus status,
                   const int exitCode) {
                if (status == QWebEnginePage::NormalTerminationStatus) return;
                emit rendererFailed(status, exitCode);
                loadTrustedError();
            });
    connect(page_.get(), &QWebEnginePage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest &) { emit popupDenied(); });
    connect(page_.get(), &QWebEnginePage::loadingChanged, this,
            &WebSurface::observeLoadingChange);
    connect(page_.get(), &QWebEnginePage::loadProgress, this,
            [this](const int progress) {
                const quint64 incarnation = activeLoadIncarnation_;
                if (incarnation == 0 || incarnation != navigationIncarnation_) return;
                setLoadProgress(progress);
            });
    connect(page_.get(), &QWebEnginePage::titleChanged, this,
            [this](const QString &physicalTitle) {
                const quint64 incarnation = activeLoadIncarnation_;
                if (incarnation == 0 || incarnation != navigationIncarnation_) return;
                updateTitle(physicalTitle);
            });

    configurationValid_ = true;
}

WebSurface::~WebSurface()
{
    (void)shutdown();
}

bool WebSurface::shutdown()
{
    if (shutdown_) return shutdownSucceeded_;
    shutdown_ = true;
    configurationValid_ = false;
    ++navigationIncarnation_;
    disconnect(recommendedStateConnection_);
    recommendedStateConnection_ = {};
    if (page_ != nullptr) page_->triggerAction(QWebEnginePage::Stop);
    if (view_ != nullptr) {
        view_->hide();
        view_->setPage(nullptr);
        view_.reset();
    }
    if (page_ != nullptr) {
        page_->setVisible(false);
        shutdownSucceeded_ = session_ != nullptr
            && session_->unregisterPage(page_.get());
        page_.reset();
    }
    return shutdownSucceeded_;
}

QUrl WebSurface::trustedErrorUrl()
{
    return QUrl(QStringLiteral("qrc:/web/error.html"));
}

bool WebSurface::isChromiumSandboxConfigurationSafe(
    const QStringList &arguments,
    const QByteArray &disableSandboxEnvironment,
    const QByteArray &chromiumFlagsEnvironment)
{
    if (!disableSandboxEnvironment.trimmed().isEmpty()) return false;
    for (const QString &argument : arguments) {
        if (disablesSandbox(argument)) return false;
    }
    const QString flags = QString::fromUtf8(chromiumFlagsEnvironment);
    const QStringList tokens = QProcess::splitCommand(flags);
    for (const QString &token : tokens) {
        if (disablesSandbox(token)) return false;
    }
    return true;
}

bool WebSurface::isConfigurationValid() const noexcept
{
    return configurationValid_;
}

bool WebSurface::navigate(const QUrl &url)
{
    if (!configurationValid_ || page_ == nullptr || view_ == nullptr) return false;
    if (url != registeredMainFrameEntry_) {
        loadTrustedError();
        return false;
    }
    beginNavigation(url);
    view_->setUrl(url);
    return true;
}

bool WebSurface::reload()
{
    if (!configurationValid_ || page_ == nullptr || view_ == nullptr) return false;
    const bool reloadCurrentEntry = page_->url() == registeredMainFrameEntry_;
    beginNavigation(registeredMainFrameEntry_);
    if (reloadCurrentEntry) {
        view_->reload();
    } else {
        view_->setUrl(registeredMainFrameEntry_);
    }
    return true;
}

void WebSurface::stop()
{
    if (!configurationValid_ || page_ == nullptr) return;
    stopRequestedIncarnation_ = navigationIncarnation_;
    page_->triggerAction(QWebEnginePage::Stop);
}

void WebSurface::setTabActive(const bool active)
{
    if (!configurationValid_ || page_ == nullptr || view_ == nullptr) return;
    tabActive_ = active;
    disconnect(recommendedStateConnection_);
    recommendedStateConnection_ = {};
    if (active) {
        page_->setLifecycleState(QWebEnginePage::LifecycleState::Active);
        view_->show();
        page_->setVisible(true);
        return;
    }

    view_->hide();
    page_->setVisible(false);
    freezeWhenRecommended();
}

QUrl WebSurface::currentUrl() const
{
    return page_ != nullptr ? page_->url() : QUrl();
}

QUrl WebSurface::registeredMainFrameEntry() const
{
    return registeredMainFrameEntry_;
}

QString WebSurface::title() const { return title_; }
int WebSurface::loadProgress() const noexcept { return loadProgress_; }
bool WebSurface::isLoading() const noexcept { return loading_; }

QWebEngineProfile *WebSurface::profile() const noexcept
{
    return session_ != nullptr ? session_->profile() : nullptr;
}

QWebEnginePage *WebSurface::page() const noexcept { return page_.get(); }
QWebEngineView *WebSurface::view() const noexcept { return view_.get(); }

PilotRequestInterceptor *WebSurface::requestInterceptor() const noexcept
{
    return session_ != nullptr ? session_->requestInterceptor() : nullptr;
}

void WebSurface::beginNavigation(const QUrl &url)
{
    ++navigationIncarnation_;
    activeLoadIncarnation_ = navigationIncarnation_;
    expectedNavigationUrl_ = url;
    stopRequestedIncarnation_ = 0;
    awaitingLoadStart_ = true;
}

void WebSurface::observeLoadingChange(const QWebEngineLoadingInfo &information)
{
    if (shutdown_ || page_ == nullptr) return;
    quint64 incarnation = activeLoadIncarnation_;
    if (information.status() == QWebEngineLoadingInfo::LoadStartedStatus) {
        if (information.url() != expectedNavigationUrl_) return;
        activeLoadIncarnation_ = navigationIncarnation_;
        incarnation = activeLoadIncarnation_;
        awaitingLoadStart_ = false;
    } else if (information.url() != expectedNavigationUrl_) {
        return;
    } else if (awaitingLoadStart_
               && information.status() == QWebEngineLoadingInfo::LoadStoppedStatus
               && stopRequestedIncarnation_ != navigationIncarnation_) {
        return;
    } else {
        awaitingLoadStart_ = false;
    }
    if (incarnation == 0) return;
    QMetaObject::invokeMethod(
        this,
        [this, information, incarnation] {
            handleLoadingChange(information, incarnation);
        },
        Qt::QueuedConnection);
}

void WebSurface::handleLoadingChange(const QWebEngineLoadingInfo &information,
                                     const quint64 incarnation)
{
    if (shutdown_ || page_ == nullptr || incarnation != navigationIncarnation_)
        return;
    switch (information.status()) {
    case QWebEngineLoadingInfo::LoadStartedStatus:
        setLoadProgress(0);
        setLoading(true);
        break;
    case QWebEngineLoadingInfo::LoadStoppedStatus:
        setLoading(false);
        stopRequestedIncarnation_ = 0;
        emit navigationFinished(information.url(), false);
        break;
    case QWebEngineLoadingInfo::LoadSucceededStatus:
        setLoadProgress(100);
        setLoading(false);
        stopRequestedIncarnation_ = 0;
        updateTitle(page_->title());
        emit navigationFinished(information.url(), true);
        break;
    case QWebEngineLoadingInfo::LoadFailedStatus:
        setLoading(false);
        if (stopRequestedIncarnation_ == incarnation) {
            stopRequestedIncarnation_ = 0;
            emit navigationFinished(information.url(), false);
        } else if (information.url() == trustedErrorUrl()) {
            emit navigationFinished(information.url(), false);
        } else {
            loadTrustedError();
        }
        break;
    }
}

void WebSurface::setLoading(const bool loading)
{
    if (loading_ == loading) return;
    loading_ = loading;
    emit loadingChanged(loading_);
}

void WebSurface::setLoadProgress(const int progress)
{
    const int bounded = std::clamp(progress, 0, 100);
    if (loadProgress_ == bounded) return;
    loadProgress_ = bounded;
    emit loadProgressChanged(loadProgress_);
}

void WebSurface::updateTitle(const QString &physicalTitle)
{
    const QString updated = trustedTitle(physicalTitle);
    if (title_ == updated) return;
    title_ = updated;
    emit titleChanged(title_);
}

QString WebSurface::trustedTitle(const QString &physicalTitle) const
{
    constexpr qsizetype maximumTitleLength = 256;
    const QString candidate = physicalTitle.trimmed();
    const QString host = requestInterceptor() != nullptr
        ? requestInterceptor()->mockOrigin().host() : QString{};
    const QUrl possibleUrl(candidate, QUrl::StrictMode);
    if (candidate.isEmpty() || candidate.contains(QStringLiteral("://"))
        || (!host.isEmpty() && candidate.contains(host, Qt::CaseInsensitive))
        || (possibleUrl.isValid() && !possibleUrl.scheme().isEmpty())) {
        return QStringLiteral("Restricted web");
    }
    return candidate.left(maximumTitleLength);
}

void WebSurface::loadTrustedError()
{
    if (shutdown_ || page_ == nullptr || view_ == nullptr) return;
    beginNavigation(trustedErrorUrl());
    view_->setUrl(trustedErrorUrl());
}

void WebSurface::freezeWhenRecommended()
{
    if (tabActive_ || page_ == nullptr) return;
    if (page_->recommendedState() != QWebEnginePage::LifecycleState::Active) {
        page_->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
        return;
    }
    recommendedStateConnection_ = connect(
        page_.get(), &QWebEnginePage::recommendedStateChanged, this,
        [this](const QWebEnginePage::LifecycleState state) {
            if (tabActive_ || page_ == nullptr
                || state == QWebEnginePage::LifecycleState::Active) {
                return;
            }
            disconnect(recommendedStateConnection_);
            recommendedStateConnection_ = {};
            page_->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
        });
}
