#pragma once

#include "ProtocolMessage.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QPointer>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

class HostWorkerSessionIo;
class HostCapabilityRuntime;
class IpcSession;
class MainWindow;
class QThread;
struct BrokerResult;

enum class HostWorkerSessionState { Detached, Running, ShuttingDown, Failed };
Q_DECLARE_METATYPE(HostWorkerSessionState)

class HostWorkerSessionController final : public QObject
{
    Q_OBJECT
public:
    using NavigationCallback =
        std::function<bool(const QString &appId, const QString &route)>;

    explicit HostWorkerSessionController(NavigationCallback navigate,
                                         QObject *parent = nullptr);
    explicit HostWorkerSessionController(MainWindow *window, QObject *parent = nullptr);
    ~HostWorkerSessionController() override;

    [[nodiscard]] bool attach(std::unique_ptr<IpcSession> session);
    [[nodiscard]] bool canAttachImmediately() const noexcept;
    [[nodiscard]] bool requestRouteLoad(const QString &route);
    [[nodiscard]] bool sendVisibilityChanged(bool active);
    [[nodiscard]] bool shutdown(const QString &reason);
    [[nodiscard]] quint64 generation() const noexcept;
    [[nodiscard]] HostWorkerSessionState state() const noexcept;
    [[nodiscard]] QString lastErrorCode() const;
    [[nodiscard]] qsizetype pendingRouteLoadCount() const noexcept;
    [[nodiscard]] qsizetype pendingCapabilityCount() const noexcept;
    [[nodiscard]] bool hasIoThread() const noexcept;
    [[nodiscard]] bool ioThreadRunning() const noexcept;
    void setCapabilityRuntime(HostCapabilityRuntime *runtime) noexcept;
    void completeCapability(quint64 generation,
                            const QString &requestId,
                            const BrokerResult &result);

signals:
    void sessionDetached(quint64 generation);
    void failed(const QString &errorCode, quint64 generation);
    void routeLoadAcknowledged(const QString &route, quint64 generation);
    void heartbeatObserved(quint64 generation);
    void capabilityRequestObserved(const QString &capability,
                                   const QString &operation,
                                   const QVariantMap &payload,
                                   quint64 generation);
    void capabilityResponseQueued(const QString &requestId,
                                  bool ok,
                                  const QString &errorCode,
                                  quint64 generation);
    void capabilityResponseSent(const QString &requestId, quint64 generation);
    void pageMetadataChanged(quint64 generation,
                             const QString &title,
                             const QString &status);

private slots:
    void handlePageMetadata(quint64 generation,
                            const QString &title,
                            const QString &status);
    void handleNavigationRequest(quint64 generation,
                                 const QString &requestId,
                                 const QString &route);

private:
    struct OutboundCommand final {
        quint64 id = 0;
        std::optional<ProtocolMessage> message;
        QString route;
        bool trackedRouteLoad = false;
        bool resumePollingAfter = false;
        QString capabilityRequestId;
    };

    void handleRouteLoadResponse(quint64 generation,
                                 const QString &requestId,
                                 const QJsonObject &payload);
    void handleCapabilityRequest(quint64 generation,
                                 const QString &requestId,
                                 const QString &capability,
                                 const QString &operation,
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
    void handleIoThreadFinished(QPointer<HostWorkerSessionIo> oldIo,
                                QThread *oldThread,
                                quint64 generation);
    void stopIoThreadForDestruction();

    static constexpr qsizetype maximumQueuedCommands = 64;

    NavigationCallback navigate_;
    QPointer<HostWorkerSessionIo> io_;
    QPointer<HostCapabilityRuntime> capabilityRuntime_;
    QThread *ioThread_ = nullptr;
    std::unique_ptr<IpcSession> pendingSession_;
    QQueue<OutboundCommand> outbound_;
    std::optional<OutboundCommand> activeCommand_;
    QString activeCapabilityRequestId_;
    QHash<QString, QString> pendingRouteLoads_;
    HostWorkerSessionState state_ = HostWorkerSessionState::Detached;
    HostWorkerSessionState cleanupFinalState_ = HostWorkerSessionState::Detached;
    QString lastErrorCode_;
    QString appIdentity_;
    quint64 generation_ = 0;
    quint64 nextCommandId_ = 0;
    quint64 nextRouteLoadId_ = 0;
    bool stopRequested_ = false;
};
