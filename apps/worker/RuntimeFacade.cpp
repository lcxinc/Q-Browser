#include "RuntimeFacade.h"

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

void RuntimeFacade::complete(const QString &requestId, const QJsonObject &response)
{
    emit capabilityFinished(requestId, response);
}
