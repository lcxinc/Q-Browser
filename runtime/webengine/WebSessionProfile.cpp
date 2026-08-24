#include "WebSessionProfile.h"

#include "PilotRequestInterceptor.h"
#include "WebSurface.h"

#include <QCoreApplication>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>

#include <utility>

WebSessionProfile::WebSessionProfile(QUrl mockOrigin, QObject *parent)
    : QObject(parent)
    , interceptor_(std::make_unique<PilotRequestInterceptor>(std::move(mockOrigin)))
{
    configurationValid_ = interceptor_->isConfigurationValid()
        && WebSurface::isChromiumSandboxConfigurationSafe(
            QCoreApplication::arguments(),
            qgetenv("QTWEBENGINE_DISABLE_SANDBOX"),
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
    if (!configurationValid_) {
        interceptor_.reset();
        acceptingPages_ = false;
        return;
    }

    profile_ = std::make_unique<QWebEngineProfile>();
    profile_->setHttpCacheType(QWebEngineProfile::NoCache);
    profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile_->setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    profile_->setUrlRequestInterceptor(interceptor_.get());
    connect(profile_.get(), &QWebEngineProfile::downloadRequested, this,
            [this](QWebEngineDownloadRequest *request) {
                if (request == nullptr) return;
                QWebEnginePage *const page = request->page();
                const QUrl url = request->url();
                request->cancel();
                emit downloadDenied(page, url);
            });
}

WebSessionProfile::~WebSessionProfile()
{
    if (!registeredPages_.isEmpty()) {
        qCritical("WebSessionProfile destroyed with %lld registered page(s)",
                  static_cast<long long>(registeredPages_.size()));
    }
    detachProfile();
}

bool WebSessionProfile::isConfigurationValid() const noexcept
{
    return configurationValid_ && !shutdown_ && profile_ != nullptr
        && interceptor_ != nullptr;
}

QWebEngineProfile *WebSessionProfile::profile() const noexcept
{
    return profile_.get();
}

PilotRequestInterceptor *WebSessionProfile::requestInterceptor() const noexcept
{
    return interceptor_.get();
}

qsizetype WebSessionProfile::registeredPageCount() const noexcept
{
    return registeredPages_.size();
}

bool WebSessionProfile::registerPage(QWebEnginePage *page)
{
    if (!acceptingPages_ || shutdown_ || page == nullptr || profile_ == nullptr
        || page->profile() != profile_.get() || registeredPages_.contains(page)
        || registeredPages_.size() >= maximumPageCount) {
        return false;
    }
    registeredPages_.insert(page);
    connect(page, &QObject::destroyed, this, [this, page] {
        if (registeredPages_.remove(page)) {
            emit registeredPageCountChanged(registeredPages_.size());
        }
    });
    emit registeredPageCountChanged(registeredPages_.size());
    return true;
}

bool WebSessionProfile::unregisterPage(QWebEnginePage *page)
{
    if (page == nullptr || !registeredPages_.remove(page)) return false;
    emit registeredPageCountChanged(registeredPages_.size());
    return true;
}

bool WebSessionProfile::shutdown()
{
    acceptingPages_ = false;
    if (!registeredPages_.isEmpty()) return false;
    if (shutdown_) return true;
    shutdown_ = true;
    configurationValid_ = false;
    detachProfile();
    return true;
}

void WebSessionProfile::detachProfile()
{
    if (profile_ != nullptr) profile_->setUrlRequestInterceptor(nullptr);
    profile_.reset();
    interceptor_.reset();
}
