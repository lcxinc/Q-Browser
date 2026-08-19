#pragma once

#include "IpcSession.h"

#include <QObject>
#include <memory>

class QTimer;
class QThread;

class HostWorkerSessionIo final : public QObject
{
    Q_OBJECT

public:
    HostWorkerSessionIo(std::unique_ptr<IpcSession> session,
                        quint64 generation,
                        QThread *ownerThread);

    void start();
    void sendMessage(quint64 generation,
                     quint64 commandId,
                     const ProtocolMessage &message,
                     bool trackedRouteLoad,
                     const QString &route);
    void resumePolling(quint64 generation);
    void beginShutdown(quint64 generation, const QString &reason);
    void abort(quint64 generation);
    [[nodiscard]] bool transferToOwnerThread();

signals:
    void commandFinished(quint64 generation,
                         quint64 commandId,
                         bool success,
                         const QString &errorCode);
    void navigationRequested(quint64 generation,
                             const QString &requestId,
                             const QString &route);
    void routeLoadResponse(quint64 generation,
                           const QString &requestId,
                           const QJsonObject &payload);
    void sessionFailed(quint64 generation, const QString &errorCode);
    void shutdownFinished(quint64 generation);
    void heartbeatObserved(quint64 generation);

private:
    void pollSession();
    void fail(const QString &errorCode);

    static constexpr int sendTimeoutMs = 5000;
    static constexpr int routeLoadTimeoutMs = 5000;
    static constexpr int maximumMessagesPerTurn = 8;

    std::unique_ptr<IpcSession> session_;
    QTimer *pollTimer_ = nullptr;
    quint64 generation_ = 0;
    QThread *ownerThread_ = nullptr;
    bool awaitingGui_ = false;
    bool stopping_ = false;
    bool terminal_ = false;
};
