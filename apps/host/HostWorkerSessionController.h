#pragma once

#include "ProtocolMessage.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <optional>

class HostWorkerSessionIo;
class IpcSession;
class MainWindow;
class QThread;

enum class HostWorkerSessionState { Detached, Running, ShuttingDown, Failed };
Q_DECLARE_METATYPE(HostWorkerSessionState)

class HostWorkerSessionController final : public QObject
{
    Q_OBJECT
public:
    explicit HostWorkerSessionController(MainWindow *window, QObject *parent = nullptr);
    ~HostWorkerSessionController() override;

    [[nodiscard]] bool attach(std::unique_ptr<IpcSession> session);
    [[nodiscard]] bool shutdown(const QString &reason);
    [[nodiscard]] HostWorkerSessionState state() const noexcept;
    [[nodiscard]] QString lastErrorCode() const;
    [[nodiscard]] qsizetype pendingRouteLoadCount() const noexcept;
    [[nodiscard]] bool hasIoThread() const noexcept;

signals:
    void failed(const QString &errorCode);
    void routeLoadAcknowledged(const QString &route);

private:
    struct OutboundCommand final {
        quint64 id = 0;
        std::optional<ProtocolMessage> message;
        QString route;
        bool trackedRouteLoad = false;
        bool resumePollingAfter = false;
    };

    void handleHostWorkerRoute(const QString &packageId,
                               const QString &entryPoint,
                               const QVariantMap &parameters,
                               const QUrl &appUrl);
    void handleNavigationRequest(quint64 generation,
                                 const QString &requestId,
                                 const QString &route);
    void handleRouteLoadResponse(quint64 generation,
                                 const QString &requestId,
                                 const QJsonObject &payload);
    void handleCommandFinished(quint64 generation,
                               quint64 commandId,
                               bool success,
                               const QString &errorCode);
    bool enqueueMessage(const ProtocolMessage &message,
                        bool resumePollingAfter = false);
    bool enqueueRouteLoad(const QString &route);
    void pumpOutbound();
    void resumeIoPolling();
    void failClosed(const QString &errorCode);
    bool startSession(std::unique_ptr<IpcSession> session);
    void requestIoStop();
    void handleIoThreadFinished(HostWorkerSessionIo *oldIo,
                                QThread *oldThread,
                                quint64 generation);
    void stopIoThreadForDestruction();

    static constexpr qsizetype maximumQueuedCommands = 64;

    MainWindow *window_ = nullptr;
    HostWorkerSessionIo *io_ = nullptr;
    HostWorkerSessionIo *ioIdentity_ = nullptr;
    QThread *ioThread_ = nullptr;
    std::unique_ptr<IpcSession> pendingSession_;
    QQueue<OutboundCommand> outbound_;
    std::optional<OutboundCommand> activeCommand_;
    QHash<QString, QString> pendingRouteLoads_;
    HostWorkerSessionState state_ = HostWorkerSessionState::Detached;
    HostWorkerSessionState cleanupFinalState_ = HostWorkerSessionState::Detached;
    QString lastErrorCode_;
    QString appIdentity_;
    quint64 generation_ = 0;
    quint64 nextCommandId_ = 0;
    quint64 nextRouteLoadId_ = 0;
    bool suppressHostRoute_ = false;
};
