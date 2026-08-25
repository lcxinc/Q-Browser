#include "WebSurface.h"

#include "PilotRequestInterceptor.h"
#include "WebSessionProfile.h"

#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
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

constexpr qsizetype maximumRawTitleLength = 4096;
constexpr qsizetype maximumTrustedTitleLength = 256;
constexpr int maximumPercentDecodePasses = 3;

bool hasUnsafeTitleCodeUnits(const QString &title)
{
    for (qsizetype index = 0; index < title.size(); ++index) {
        const QChar character = title.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= title.size()
                || !title.at(index + 1).isLowSurrogate()) {
                return true;
            }
            ++index;
            continue;
        }
        if (character.isLowSurrogate()
            || character.category() == QChar::Other_Control) {
            return true;
        }
        const char16_t value = character.unicode();
        if (value == 0x061c || (value >= 0x200e && value <= 0x200f)
            || (value >= 0x202a && value <= 0x202e)
            || (value >= 0x2066 && value <= 0x2069)) {
            return true;
        }
    }
    return false;
}

bool looksLikePhysicalAddress(const QString &title, const QString &physicalHost)
{
    const QString candidate = title.trimmed();
    if (candidate.isEmpty() || candidate.contains(QStringLiteral("://"))) {
        return true;
    }
    if (!physicalHost.isEmpty()
        && candidate.contains(physicalHost, Qt::CaseInsensitive)) {
        return true;
    }

    static const QRegularExpression scheme(
        QStringLiteral(R"(^[A-Za-z][A-Za-z0-9+.-]*:)")
    );
    static const QRegularExpression localhost(
        QStringLiteral(R"((?:^|[^A-Za-z0-9-])localhost(?:[^A-Za-z0-9-]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ipv4Loopback(
        QStringLiteral(R"((?:^|[^0-9])127(?:\.[0-9]{1,3}){3}(?:[^0-9]|$))"));
    if (scheme.match(candidate).hasMatch()
        || localhost.match(candidate).hasMatch()
        || ipv4Loopback.match(candidate).hasMatch()
        || candidate.contains(QStringLiteral("::1"), Qt::CaseInsensitive)
        || candidate.contains(QStringLiteral("0:0:0:0:0:0:0:1"),
                              Qt::CaseInsensitive)) {
        return true;
    }
    const QUrl possibleUrl(candidate, QUrl::StrictMode);
    return possibleUrl.isValid() && !possibleUrl.scheme().isEmpty();
}

class PilotWebPage final : public QWebEnginePage
{
public:
    using DeniedCallback = std::function<void()>;
    using NavigationDeniedCallback = std::function<void(const QUrl &)>;
    using FileSelectionDeniedCallback = std::function<void(bool)>;

    PilotWebPage(QWebEngineProfile *profile,
                 const PilotRequestInterceptor *interceptor,
                 QUrl registeredMainFrameEntry,
                 DeniedCallback popupDenied,
                 NavigationDeniedCallback navigationDenied,
                 FileSelectionDeniedCallback fileSelectionDenied,
                 QObject *parent)
        : QWebEnginePage(profile, parent)
        , interceptor_(interceptor)
        , registeredMainFrameEntry_(std::move(registeredMainFrameEntry))
        , popupDenied_(std::move(popupDenied))
        , navigationDenied_(std::move(navigationDenied))
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
        if (!allowed) {
            if (isMainFrame) navigationDenied_(url);
            return false;
        }
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
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
    NavigationDeniedCallback navigationDenied_;
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
    sessionRetirementConnection_ = connect(
        session_, &WebSessionProfile::retirementRequested, this,
        [this] { (void)shutdown(); }, Qt::DirectConnection);
    PilotRequestInterceptor *const interceptor = session_->interceptorHandle();
    if (!session_->isConfigurationValid() || interceptor == nullptr
        || !registeredMainFrameEntry_.isValid()
        || registeredMainFrameEntry_ == trustedErrorUrl()
        || !interceptor->isAllowed(registeredMainFrameEntry_)) {
        return;
    }
    physicalOriginHost_ = interceptor->mockOrigin().host();

    auto page = std::make_unique<PilotWebPage>(
        session_->profileHandle(), interceptor, registeredMainFrameEntry_,
        [this] { emit popupDenied(); },
        [this](const QUrl &url) { handleDeniedMainFrameNavigation(url); },
        [this](const bool directorySelection) {
            emit fileSelectionDenied(directorySelection);
        },
        nullptr);
    if (!session_->registerPageInternal(page.get())) return;
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
    connect(page_.get(), &QWebEnginePage::titleChanged, this,
            [this](const QString &) {
                const quint64 incarnation = activeLoadIncarnation_;
                QMetaObject::invokeMethod(
                    this,
                    [this, incarnation] {
                        if (shutdown_ || page_ == nullptr || incarnation == 0
                            || incarnation != navigationIncarnation_
                            || !titleUpdatesAllowed_ || activeLoadStarted_
                            || page_->isLoading()) {
                            return;
                        }
                        updateTitle(page_->title());
                    },
                    Qt::QueuedConnection);
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
    awaitingLoadStart_ = false;
    awaitingDifferentDocument_ = false;
    activeLoadStarted_ = false;
    titleUpdatesAllowed_ = false;
    terminalPendingIncarnation_ = 0;
    releaseLoadProgress();
    disconnect(recommendedStateConnection_);
    recommendedStateConnection_ = {};
    disconnect(sessionRetirementConnection_);
    sessionRetirementConnection_ = {};
    WebSessionProfile *const retiringSession = session_;
    if (page_ != nullptr) page_->triggerAction(QWebEnginePage::Stop);
    if (view_ != nullptr) {
        view_->hide();
        view_->setPage(nullptr);
        view_.reset();
    }
    if (page_ != nullptr) {
        page_->setVisible(false);
        shutdownSucceeded_ = retiringSession != nullptr
            && retiringSession->retirePageInternal(page_);
    }
    session_ = nullptr;
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

QString WebSurface::title() const { return title_; }
int WebSurface::loadProgress() const noexcept { return loadProgress_; }
bool WebSurface::isLoading() const noexcept { return loading_; }

#ifdef Q_BROWSER_WEBENGINE_TESTING
QUrl WebSurface::currentUrl() const
{
    return page_ != nullptr ? page_->url() : QUrl();
}

QUrl WebSurface::registeredMainFrameEntry() const
{
    return registeredMainFrameEntry_;
}

QWebEngineProfile *WebSurface::profile() const noexcept
{
    return session_ != nullptr ? session_->profileHandle() : nullptr;
}

QWebEnginePage *WebSurface::page() const noexcept { return page_.get(); }
QWebEngineView *WebSurface::view() const noexcept { return view_.get(); }

PilotRequestInterceptor *WebSurface::requestInterceptor() const noexcept
{
    return session_ != nullptr ? session_->interceptorHandle() : nullptr;
}

quint64 WebSurface::navigationIncarnationForTesting() const noexcept
{
    return navigationIncarnation_;
}

QString WebSurface::trustedTitleForTesting(const QString &physicalTitle) const
{
    return trustedTitle(physicalTitle);
}
#endif

void WebSurface::beginNavigation(const QUrl &url)
{
    releaseLoadProgress();
    ++navigationIncarnation_;
    activeLoadIncarnation_ = navigationIncarnation_;
    awaitingDifferentDocument_ = page_ != nullptr && page_->url() != url;
    expectedNavigationUrl_ = url;
    stopRequestedIncarnation_ = 0;
    terminalPendingIncarnation_ = 0;
    awaitingLoadStart_ = true;
    activeLoadStarted_ = false;
    titleUpdatesAllowed_ = false;
    setLoading(false);
    setLoadProgress(0);
}

void WebSurface::observeLoadingChange(const QWebEngineLoadingInfo &information)
{
    if (shutdown_ || page_ == nullptr) return;
    quint64 incarnation = 0;
    if (information.status() == QWebEngineLoadingInfo::LoadStartedStatus) {
        if (information.url() != registeredMainFrameEntry_
            && information.url() != trustedErrorUrl()) {
            return;
        }
        if (awaitingLoadStart_) {
            if (information.url() != expectedNavigationUrl_) return;
        } else {
            ++navigationIncarnation_;
            activeLoadIncarnation_ = navigationIncarnation_;
            expectedNavigationUrl_ = information.url();
            stopRequestedIncarnation_ = 0;
            terminalPendingIncarnation_ = 0;
            titleUpdatesAllowed_ = false;
            setLoading(false);
            setLoadProgress(0);
        }
        activeLoadIncarnation_ = navigationIncarnation_;
        incarnation = activeLoadIncarnation_;
        awaitingLoadStart_ = false;
        awaitingDifferentDocument_ = false;
        activeLoadStarted_ = true;
        terminalPendingIncarnation_ = 0;
        armLoadProgress(incarnation);
    } else {
        if (information.url() != expectedNavigationUrl_ || page_->isLoading()) {
            return;
        }
        if (awaitingLoadStart_) {
            // Chromium can omit a distinct started notification when a live
            // document is replaced by a different local document.  The URL
            // transition makes that terminal notification unambiguous; a
            // same-URL replacement must still wait for its own start so an
            // older incarnation cannot complete it.
            if (!awaitingDifferentDocument_
                || page_->url() != expectedNavigationUrl_) {
                return;
            }
            awaitingLoadStart_ = false;
            awaitingDifferentDocument_ = false;
        } else if (!activeLoadStarted_) {
            return;
        }
        incarnation = activeLoadIncarnation_;
        activeLoadStarted_ = false;
        releaseLoadProgress();
        terminalPendingIncarnation_ = incarnation;
    }
    if (incarnation == 0) return;
    QMetaObject::invokeMethod(
        this,
        [this, information, incarnation] {
            handleLoadingChange(information, incarnation);
        },
        Qt::QueuedConnection);
}

void WebSurface::armLoadProgress(const quint64 incarnation)
{
    releaseLoadProgress();
    if (page_ == nullptr || incarnation == 0) return;
    QWebEnginePage *const progressPage = page_.get();
    loadProgressConnection_ = connect(
        progressPage, &QWebEnginePage::loadProgress, this,
        [this, progressPage, incarnation](const int progress) {
            if (shutdown_ || page_.get() != progressPage
                || incarnation != navigationIncarnation_
                || incarnation != activeLoadIncarnation_
                || awaitingLoadStart_ || !activeLoadStarted_
                || !progressPage->isLoading()) {
                return;
            }
            setLoadProgress(progress);
        },
        Qt::QueuedConnection);
}

void WebSurface::releaseLoadProgress()
{
    disconnect(loadProgressConnection_);
    loadProgressConnection_ = {};
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
        if (terminalPendingIncarnation_ != incarnation) return;
        terminalPendingIncarnation_ = 0;
        setLoading(false);
        stopRequestedIncarnation_ = 0;
        titleUpdatesAllowed_ = true;
        updateTitle(page_->title());
        emit navigationFinished(information.url(), false);
        break;
    case QWebEngineLoadingInfo::LoadSucceededStatus:
        if (terminalPendingIncarnation_ != incarnation) return;
        terminalPendingIncarnation_ = 0;
        setLoadProgress(100);
        setLoading(false);
        stopRequestedIncarnation_ = 0;
        titleUpdatesAllowed_ = true;
        updateTitle(page_->title());
        emit navigationFinished(information.url(), true);
        break;
    case QWebEngineLoadingInfo::LoadFailedStatus:
        if (terminalPendingIncarnation_ != incarnation) return;
        terminalPendingIncarnation_ = 0;
        setLoading(false);
        if (stopRequestedIncarnation_ == incarnation) {
            stopRequestedIncarnation_ = 0;
            titleUpdatesAllowed_ = true;
            updateTitle(page_->title());
            emit navigationFinished(information.url(), false);
        } else if (information.url() == trustedErrorUrl()) {
            titleUpdatesAllowed_ = true;
            updateTitle(page_->title());
            emit navigationFinished(information.url(), false);
        } else {
            loadTrustedError();
        }
        break;
    }
}

void WebSurface::handleDeniedMainFrameNavigation(const QUrl &)
{
    if (shutdown_ || page_ == nullptr || view_ == nullptr
        || (!awaitingLoadStart_ && !activeLoadStarted_)) {
        return;
    }
    const quint64 incarnation = navigationIncarnation_;
    QMetaObject::invokeMethod(
        this,
        [this, incarnation] {
            if (shutdown_ || incarnation != navigationIncarnation_) return;
            loadTrustedError();
        },
        Qt::QueuedConnection);
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
    const QString fallback = QStringLiteral("Restricted web");
    if (physicalTitle.size() > maximumRawTitleLength) return fallback;
    const QString candidate = physicalTitle.trimmed();
    if (candidate.isEmpty() || hasUnsafeTitleCodeUnits(candidate)) return fallback;

    QString inspected = candidate;
    for (int pass = 0; pass <= maximumPercentDecodePasses; ++pass) {
        if (hasUnsafeTitleCodeUnits(inspected)
            || looksLikePhysicalAddress(inspected, physicalOriginHost_)) {
            return fallback;
        }
        if (pass == maximumPercentDecodePasses) break;
        const QString decoded = QUrl::fromPercentEncoding(inspected.toUtf8());
        if (decoded == inspected) break;
        inspected = decoded;
    }

    QString trusted = candidate.left(maximumTrustedTitleLength);
    if (!trusted.isEmpty() && trusted.back().isHighSurrogate()) trusted.chop(1);
    return trusted.isEmpty() ? fallback : trusted;
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
