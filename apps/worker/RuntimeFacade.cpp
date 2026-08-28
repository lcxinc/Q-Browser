#include "RuntimeFacade.h"

#include "NormalizedPath.h"
#include "ProtocolMessage.h"

#include <QUrl>
#include <QUuid>

namespace {

constexpr qsizetype maximumRawPageTitleCodeUnits = 4096;
constexpr qsizetype maximumPageTitleCodeUnits = 256;

bool isBidiControl(const char16_t value)
{
    return value == 0x061c || (value >= 0x200e && value <= 0x200f)
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

std::optional<QString> canonicalPageTitle(const QString &untrusted)
{
    if (untrusted.isEmpty() || untrusted.size() > maximumRawPageTitleCodeUnits) {
        return std::nullopt;
    }
    QString canonical;
    canonical.reserve(maximumPageTitleCodeUnits);
    bool prefixComplete = false;
    for (qsizetype index = 0; index < untrusted.size(); ++index) {
        const QChar character = untrusted.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= untrusted.size()
                || !untrusted.at(index + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            if (!prefixComplete
                && canonical.size() + 2 <= maximumPageTitleCodeUnits) {
                canonical.append(character);
                canonical.append(untrusted.at(index + 1));
                prefixComplete = canonical.size() == maximumPageTitleCodeUnits;
            } else if (!prefixComplete) {
                prefixComplete = true;
            }
            ++index;
            continue;
        }
        if (character.isLowSurrogate() || character == u'<' || character == u'>') {
            return std::nullopt;
        }
        if (character.category() == QChar::Other_Control
            || isBidiControl(character.unicode())) {
            continue;
        }
        if (!prefixComplete) {
            canonical.append(character);
            prefixComplete = canonical.size() == maximumPageTitleCodeUnits;
        }
    }
    return canonical.isEmpty() ? std::nullopt
                               : std::optional<QString>(std::move(canonical));
}

} // namespace

RuntimeFacade::RuntimeFacade(QObject *parent) : QObject(parent) {}

QString RuntimeFacade::appIdentity() const { return appIdentity_; }
QString RuntimeFacade::apiOrigin() const { return apiOrigin_; }
QString RuntimeFacade::route() const { return route_; }
bool RuntimeFacade::active() const { return active_; }

void RuntimeFacade::assignAppIdentity(const QString &identity)
{
    if (appIdentity_ == identity) return;
    appIdentity_ = identity;
    emit appIdentityChanged();
}

void RuntimeFacade::assignApiOrigin(const QString &origin)
{
    if (apiOrigin_ == origin) return;
    apiOrigin_ = origin;
    emit apiOriginChanged();
}

void RuntimeFacade::loadRoute(const QString &route)
{
    if (route_ == route) return;
    route_ = route;
    emit routeChanged();
}

void RuntimeFacade::assignActive(const bool active)
{
    if (active_ == active) return;
    active_ = active;
    emit activeChanged();
}

QString RuntimeFacade::invoke(const QString &capability,
                              const QString &operation,
                              const QJsonObject &payload)
{
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128).toLower();
    emit capabilityRequested(requestId, capability, operation, payload);
    return requestId;
}

QString RuntimeFacade::navigate(const QString &route)
{
    if (pendingNavigationRequests_.size() >= 1 || route.isEmpty() || route.size() > 2048
        || route.contains(u'#')) {
        return {};
    }
    const qsizetype queryStart = route.indexOf(u'?');
    const QString path = queryStart < 0 ? route : route.first(queryStart);
    if (!NormalizedPath::parse(path).isValid()) {
        return {};
    }
    const QUrl candidate(route, QUrl::StrictMode);
    if (!candidate.isValid() || !candidate.isRelative() || candidate.path().isEmpty()
        || !candidate.scheme().isEmpty() || !candidate.authority().isEmpty()) {
        return {};
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128).toLower();
    pendingNavigationRequests_.insert(requestId);
    emit navigationRequested(requestId, route);
    return requestId;
}

bool RuntimeFacade::setPageMetadata(const QString &title, const QString &status)
{
    const std::optional<QString> canonicalTitle = canonicalPageTitle(title);
    if (!canonicalTitle.has_value()
        || !ProtocolMessage::pageMetadata(*canonicalTitle, status).has_value()) {
        return false;
    }
    pendingPageMetadata_ = PendingPageMetadata{*canonicalTitle, status};
    emit pageMetadataChanged(*canonicalTitle, status);
    return true;
}

std::optional<RuntimeFacade::PendingPageMetadata>
RuntimeFacade::takePendingPageMetadata()
{
    std::optional<PendingPageMetadata> pending = std::move(pendingPageMetadata_);
    pendingPageMetadata_.reset();
    return pending;
}

void RuntimeFacade::complete(const QString &requestId, const QJsonObject &response)
{
    if (pendingNavigationRequests_.remove(requestId)) {
        emit navigationFinished(requestId, response);
        return;
    }
    emit capabilityFinished(requestId, response);
}
