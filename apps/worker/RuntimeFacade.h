#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>

#include <optional>

class WorkerApplication;

class RuntimeFacade final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appIdentity READ appIdentity NOTIFY appIdentityChanged FINAL)
    Q_PROPERTY(QString apiOrigin READ apiOrigin NOTIFY apiOriginChanged FINAL)
    Q_PROPERTY(QString route READ route NOTIFY routeChanged FINAL)

public:
    explicit RuntimeFacade(QObject *parent = nullptr);

    QString appIdentity() const;
    QString apiOrigin() const;
    QString route() const;
    void assignAppIdentity(const QString &identity);
    void assignApiOrigin(const QString &origin);
    void loadRoute(const QString &route);

    Q_INVOKABLE QString invoke(const QString &capability,
                               const QString &operation,
                               const QJsonObject &payload = {});
    Q_INVOKABLE QString navigate(const QString &route);
    Q_INVOKABLE bool setPageMetadata(const QString &title,
                                     const QString &status = {});
    void complete(const QString &requestId, const QJsonObject &response);

signals:
    void appIdentityChanged();
    void apiOriginChanged();
    void routeChanged();
    void capabilityRequested(const QString &requestId,
                             const QString &capability,
                             const QString &operation,
                             const QJsonObject &payload);
    void capabilityFinished(const QString &requestId, const QJsonObject &response);
    void navigationRequested(const QString &requestId, const QString &route);
    void navigationFinished(const QString &requestId, const QJsonObject &response);
    void pageMetadataChanged(const QString &title, const QString &status);

private:
    struct PendingPageMetadata final {
        QString title;
        QString status;
    };

    std::optional<PendingPageMetadata> takePendingPageMetadata();

    friend class WorkerApplication;

    QString appIdentity_;
    QString apiOrigin_;
    QString route_ = QStringLiteral("/");
    QSet<QString> pendingNavigationRequests_;
    std::optional<PendingPageMetadata> pendingPageMetadata_;
};
