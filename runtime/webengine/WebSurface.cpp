#include "WebSurface.h"

#include "PilotRequestInterceptor.h"

#include <QApplication>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QVBoxLayout>
#include <QWebEngineCertificateError>
#include <QWebEngineDownloadRequest>
#include <QWebEngineFileSystemAccessRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineRegisterProtocolHandlerRequest>
#include <QWebEngineSettings>
#include <QWebEngineView>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <functional>
#include <utility>

static void initializeHostResources()
{
    Q_INIT_RESOURCE(host);
}

namespace {

bool processHasExited(const qint64 processId)
{
    if (processId <= 0) return true;
#ifdef Q_OS_WIN
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE,
                                       static_cast<DWORD>(processId));
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
#else
    return false;
#endif
}

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
                 DeniedCallback popupDenied,
                 FileSelectionDeniedCallback fileSelectionDenied,
                 QObject *parent)
        : QWebEnginePage(profile, parent)
        , interceptor_(interceptor)
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
        if (!interceptor_->isAllowed(url)) {
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
    DeniedCallback popupDenied_;
    FileSelectionDeniedCallback fileSelectionDenied_;
};

} // namespace

WebSurface::WebSurface(const QUrl &mockOrigin, QWidget *parent)
    : QWidget(parent)
{
    initializeHostResources();
    interceptor_ = std::make_unique<PilotRequestInterceptor>(mockOrigin);
    configurationValid_ = interceptor_->isConfigurationValid()
        && isChromiumSandboxConfigurationSafe(
            QCoreApplication::arguments(),
            qgetenv("QTWEBENGINE_DISABLE_SANDBOX"),
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
    if (!configurationValid_) {
        return;
    }

    profile_ = std::make_unique<QWebEngineProfile>();
    profile_->setHttpCacheType(QWebEngineProfile::NoCache);
    profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile_->setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    profile_->setUrlRequestInterceptor(interceptor_.get());

    page_ = std::make_unique<PilotWebPage>(
        profile_.get(), interceptor_.get(),
        [this] { emit popupDenied(); },
        [this](const bool directorySelection) {
            emit fileSelectionDenied(directorySelection);
        },
        nullptr);
    QWebEngineSettings *settings = page_->settings();
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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_.get());

    connect(profile_.get(), &QWebEngineProfile::downloadRequested, this,
            [this](QWebEngineDownloadRequest *request) {
                const QUrl url = request->url();
                request->cancel();
                emit downloadDenied(url);
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
            [this](const QWebEnginePage::RenderProcessTerminationStatus status, int) {
                if (status != QWebEnginePage::NormalTerminationStatus) {
                    loadTrustedError();
                }
            });
    connect(page_.get(), &QWebEnginePage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest &) { emit popupDenied(); });
    connect(page_.get(), &QWebEnginePage::loadFinished, this, [this](const bool success) {
        if (!success && page_->url() != trustedErrorUrl() && !loadingTrustedError_) {
            loadTrustedError();
            return;
        }
        const bool trustedErrorLoaded = page_->url() == trustedErrorUrl();
        if (trustedErrorLoaded) {
            loadingTrustedError_ = false;
        }
        emit navigationFinished(page_->url(), success);
    });
}

WebSurface::~WebSurface()
{
    if (!shutdown_) {
        if (view_) view_->setPage(nullptr);
        if (profile_) profile_->setUrlRequestInterceptor(nullptr);
    }
}

bool WebSurface::shutdown()
{
    if (shutdown_) return shutdownSucceeded_;
    shutdown_ = true;
    configurationValid_ = false;
    if (page_) {
        page_->triggerAction(QWebEnginePage::Stop);
    }
    if (view_) {
        view_->hide();
        view_->setPage(nullptr);
        view_.reset();
    }
    qint64 rendererProcessId = 0;
    if (page_) {
        rendererProcessId = page_->renderProcessPid();
        page_->setVisible(false);
        page_->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
        QElapsedTimer rendererShutdown;
        rendererShutdown.start();
        while (!processHasExited(rendererProcessId)
               && rendererShutdown.elapsed() < 5'000) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        shutdownSucceeded_ = processHasExited(rendererProcessId);
        page_.reset();
    }
    if (profile_) {
        profile_->setUrlRequestInterceptor(nullptr);
        profile_.reset();
    }
    interceptor_.reset();
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
    if (!disableSandboxEnvironment.trimmed().isEmpty()) {
        return false;
    }
    for (const QString &argument : arguments) {
        if (disablesSandbox(argument)) {
            return false;
        }
    }
    const QString flags = QString::fromUtf8(chromiumFlagsEnvironment);
    const QStringList tokens = QProcess::splitCommand(flags);
    for (const QString &token : tokens) {
        if (disablesSandbox(token)) {
            return false;
        }
    }
    return true;
}

bool WebSurface::isConfigurationValid() const noexcept
{
    return configurationValid_;
}

bool WebSurface::navigate(const QUrl &url)
{
    if (!configurationValid_ || page_ == nullptr || view_ == nullptr) {
        return false;
    }
    if (!interceptor_->isAllowed(url)
        || url == trustedErrorUrl()) {
        loadTrustedError();
        return false;
    }
    loadingTrustedError_ = false;
    view_->setUrl(url);
    return true;
}

QUrl WebSurface::currentUrl() const
{
    return page_ ? page_->url() : QUrl();
}

QWebEngineProfile *WebSurface::profile() const noexcept { return profile_.get(); }
QWebEnginePage *WebSurface::page() const noexcept { return page_.get(); }
QWebEngineView *WebSurface::view() const noexcept { return view_.get(); }
PilotRequestInterceptor *WebSurface::requestInterceptor() const noexcept
{
    return interceptor_.get();
}

void WebSurface::loadTrustedError()
{
    if (view_ == nullptr) {
        return;
    }
    loadingTrustedError_ = true;
    view_->setUrl(trustedErrorUrl());
}
