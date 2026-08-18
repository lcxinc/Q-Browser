#include "PilotRequestInterceptor.h"

#include "WebSurface.h"

#include <QHostAddress>
#include <QWebEngineUrlRequestInfo>

#include <utility>

namespace {

bool isValidMockOrigin(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("http")
        || !url.userInfo().isEmpty() || url.port() <= 0
        || !url.query().isEmpty() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
        return false;
    }

    QHostAddress address;
    return address.setAddress(url.host()) && address.isLoopback();
}

bool hasSameOrigin(const QUrl &url, const QUrl &origin)
{
    return url.isValid() && url.scheme() == origin.scheme()
        && url.host() == origin.host() && url.port() == origin.port()
        && url.userInfo().isEmpty();
}

} // namespace

PilotRequestInterceptor::PilotRequestInterceptor(QUrl mockOrigin, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , mockOrigin_(std::move(mockOrigin))
    , configurationValid_(isValidMockOrigin(mockOrigin_))
{
}

bool PilotRequestInterceptor::isConfigurationValid() const noexcept
{
    return configurationValid_;
}

bool PilotRequestInterceptor::isAllowed(const QUrl &url) const
{
    if (url == WebSurface::trustedErrorUrl()) {
        return true;
    }
    return configurationValid_ && hasSameOrigin(url, mockOrigin_);
}

QUrl PilotRequestInterceptor::mockOrigin() const
{
    return mockOrigin_;
}

void PilotRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    if (isAllowed(info.requestUrl())) {
        return;
    }
    const QUrl blockedUrl = info.requestUrl();
    info.block(true);
    emit requestBlocked(blockedUrl);
}
