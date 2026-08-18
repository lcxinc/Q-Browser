#pragma once

#include <QJsonObject>
#include <QObject>

class RuntimeFacade final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appIdentity READ appIdentity NOTIFY appIdentityChanged FINAL)
    Q_PROPERTY(QString route READ route NOTIFY routeChanged FINAL)

public:
    explicit RuntimeFacade(QObject *parent = nullptr);

    QString appIdentity() const;
    QString route() const;
    void assignAppIdentity(const QString &identity);
    void loadRoute(const QString &route);

    Q_INVOKABLE QString invoke(const QString &capability,
                               const QString &operation,
                               const QJsonObject &payload = {});
    void complete(const QString &requestId, const QJsonObject &response);

signals:
    void appIdentityChanged();
    void routeChanged();
    void capabilityRequested(const QString &requestId,
                             const QString &capability,
                             const QString &operation,
                             const QJsonObject &payload);
    void capabilityFinished(const QString &requestId, const QJsonObject &response);

private:
    QString appIdentity_;
    QString route_ = QStringLiteral("/");
};
