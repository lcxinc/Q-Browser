#pragma once

#include <QNetworkAccessManager>
#include <QQmlNetworkAccessManagerFactory>

class WorkerDeniedNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit WorkerDeniedNetworkAccessManager(QObject *parent = nullptr);

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData) override;
};

class WorkerNetworkAccessManagerFactory final
    : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override;
};
