#include "WebSessionProfile.h"

#include "PilotRequestInterceptor.h"
#include "WebSurface.h"

#include <QCoreApplication>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include <utility>

namespace {

class SessionRequestFilter final : public QWebEngineUrlRequestInterceptor
{
public:
    explicit SessionRequestFilter(PilotRequestInterceptor *interceptor)
        : interceptor_(interceptor)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &information) override
    {
        if (interceptor_ == nullptr) {
            information.block(true);
            return;
        }
        const QUrl url = information.requestUrl();
        if (url == WebSurface::trustedErrorUrl()
            && information.resourceType()
                != QWebEngineUrlRequestInfo::ResourceTypeMainFrame) {
            information.block(true);
            (void)QMetaObject::invokeMethod(
                interceptor_, "requestBlocked", Qt::DirectConnection,
                Q_ARG(QUrl, url));
            return;
        }
        interceptor_->interceptRequest(information);
    }

private:
    PilotRequestInterceptor *interceptor_ = nullptr;
};

} // namespace

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
    requestFilter_ = std::make_unique<SessionRequestFilter>(interceptor_.get());
    profile_->setUrlRequestInterceptor(requestFilter_.get());
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
    acceptingPages_ = false;
    emit retirementRequested();
    if (!registeredPages_.isEmpty()) {
        qFatal("WebSessionProfile retirement left %lld registered page(s)",
               static_cast<long long>(registeredPages_.size()));
    }
    detachProfile();
}

bool WebSessionProfile::isConfigurationValid() const noexcept
{
    return configurationValid_ && !shutdown_ && profile_ != nullptr
        && interceptor_ != nullptr;
}

QWebEngineProfile *WebSessionProfile::profileHandle() const noexcept
{
    return profile_.get();
}

PilotRequestInterceptor *WebSessionProfile::interceptorHandle() const noexcept
{
    return interceptor_.get();
}

#ifdef Q_BROWSER_WEBENGINE_TESTING
QWebEngineProfile *WebSessionProfile::profile() const noexcept
{
    return profileHandle();
}

PilotRequestInterceptor *WebSessionProfile::requestInterceptor() const noexcept
{
    return interceptorHandle();
}
#endif

qsizetype WebSessionProfile::registeredPageCount() const noexcept
{
    return registeredPages_.size();
}

bool WebSessionProfile::registerPageInternal(QWebEnginePage *page)
{
    if (!acceptingPages_ || shutdown_ || page == nullptr || profile_ == nullptr
        || page->profile() != profile_.get() || registeredPages_.contains(page)
        || registeredPages_.size() >= maximumPageCount) {
        return false;
    }
    const QMetaObject::Connection destructionConnection = connect(
        page, &QObject::destroyed, this, [this, page] {
            const auto iterator = registeredPages_.find(page);
            if (iterator == registeredPages_.end()) return;
            registeredPages_.erase(iterator);
            emit registeredPageCountChanged(registeredPages_.size());
        });
    registeredPages_.insert(page, destructionConnection);
    emit registeredPageCountChanged(registeredPages_.size());
    return true;
}

bool WebSessionProfile::retirePageInternal(
    std::unique_ptr<QWebEnginePage> &page)
{
    if (page == nullptr || retiringPage_ != nullptr) return false;
    QWebEnginePage *const pageAddress = page.get();
    const auto iterator = registeredPages_.find(pageAddress);
    if (iterator == registeredPages_.end()) return false;
    const QMetaObject::Connection destructionConnection = iterator.value();
    disconnect(destructionConnection);
    retiringPage_ = pageAddress;
    page.reset();
    const qsizetype removed = registeredPages_.remove(pageAddress);
    retiringPage_ = nullptr;
    if (removed != 1) {
        qFatal("WebSessionProfile lost a retiring page registration");
    }
    emit registeredPageCountChanged(registeredPages_.size());
    return true;
}

#ifdef Q_BROWSER_WEBENGINE_TESTING
bool WebSessionProfile::registerPage(QWebEnginePage *page)
{
    return registerPageInternal(page);
}

bool WebSessionProfile::unregisterPage(QWebEnginePage *page)
{
    if (page == nullptr || page == retiringPage_) return false;
    const auto iterator = registeredPages_.find(page);
    if (iterator == registeredPages_.end()) return false;
    const QMetaObject::Connection destructionConnection = iterator.value();
    registeredPages_.erase(iterator);
    disconnect(destructionConnection);
    emit registeredPageCountChanged(registeredPages_.size());
    return true;
}
#endif

bool WebSessionProfile::shutdown()
{
    acceptingPages_ = false;
    if (retiringPage_ != nullptr || !registeredPages_.isEmpty()) return false;
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
    requestFilter_.reset();
    interceptor_.reset();
}
