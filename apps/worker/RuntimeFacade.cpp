#include "RuntimeFacade.h"

#include "NormalizedPath.h"

#include <QUrl>
#include <QUuid>

RuntimeFacade::RuntimeFacade(QObject *parent) : QObject(parent) {}

QString RuntimeFacade::appIdentity() const { return appIdentity_; }
QString RuntimeFacade::apiOrigin() const { return apiOrigin_; }
QString RuntimeFacade::route() const { return route_; }

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

void RuntimeFacade::complete(const QString &requestId, const QJsonObject &response)
{
    if (pendingNavigationRequests_.remove(requestId)) {
        emit navigationFinished(requestId, response);
        return;
    }
    emit capabilityFinished(requestId, response);
}
