#include "WorkerNetworkAccess.h"

#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

class WorkerDeniedNetworkReply final : public QNetworkReply
{
public:
    explicit WorkerDeniedNetworkReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOpenMode(QIODevice::ReadOnly);
        setError(QNetworkReply::ContentAccessDenied,
                 QStringLiteral("worker network access is broker-only"));
        QMetaObject::invokeMethod(this, [this] {
            emit errorOccurred(error());
            emit finished();
        }, Qt::QueuedConnection);
    }

    void abort() override {}

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

} // namespace

WorkerDeniedNetworkAccessManager::WorkerDeniedNetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
}

QNetworkReply *WorkerDeniedNetworkAccessManager::createRequest(
    Operation,
    const QNetworkRequest &request,
    QIODevice *)
{
    return new WorkerDeniedNetworkReply(request, this);
}

QNetworkAccessManager *WorkerNetworkAccessManagerFactory::create(QObject *parent)
{
    return new WorkerDeniedNetworkAccessManager(parent);
}
