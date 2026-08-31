#pragma once

#include "IpcSession.h"
#include "TabCapabilityAuthority.h"

#include <QObject>
#include <QVariantMap>
#include <memory>
#include <optional>

class QTimer;
class QThread;
struct HostWorkerSessionBindingState;

class HostWorkerSessionIo final : public QObject
{
    Q_OBJECT

public:
    HostWorkerSessionIo(std::unique_ptr<IpcSession> session,
                        quint64 generation,
                        QThread *ownerThread,
                        std::optional<TabCapabilityAuthority>
                            capabilityAuthority = std::nullopt);
    ~HostWorkerSessionIo() override;

    void start();
    void sendMessage(quint64 generation,
                     quint64 commandId,
                     const ProtocolMessage &message,
                     bool trackedRouteLoad,
                     const QString &route,
                     std::optional<TabCapabilityAuthority>
                         capabilityAuthority = std::nullopt,
                     std::shared_ptr<AuthorityAdmissionToken::UseGuard>
                         capabilityUse = nullptr);
    void resumePolling(quint64 generation);
    void beginShutdown(quint64 generation, const QString &reason);
    void abort(quint64 generation);
    [[nodiscard]] bool transferToOwnerThread();

signals:
    void commandFinished(quint64 generation,
                         quint64 commandId,
                         bool success,
                         bool published,
                         const QString &errorCode);
    void navigationRequested(quint64 generation,
                             const QString &requestId,
                             const QString &route);
    void pageMetadataReceived(quint64 generation,
                              const QString &title,
                              const QString &status);
    void routeLoadResponse(quint64 generation,
                           const QString &requestId,
                           const QJsonObject &payload);
    void sessionFailed(quint64 generation, const QString &errorCode);
    void shutdownFinished(quint64 generation);
    void heartbeatObserved(quint64 generation);
    void capabilityRequested(const TabCapabilityAuthority &authority,
                             quint64 generation,
                             const QString &requestId,
                             const QString &capability,
                             const QString &operation,
                             const QJsonObject &payload);

private:
    void pollSession();
    void finishShutdown();
    void fail(const QString &errorCode);
    void invalidateLiveBinding() noexcept;
    void armCommandDeadline(quint64 commandId,
                            PipeWriteCancellation cancellation);
    [[nodiscard]] bool completeCommandSubmission(quint64 commandId);

    static constexpr int sendTimeoutMs = 5000;
    static constexpr int shutdownTimeoutMs = 5000;
    static constexpr int routeLoadTimeoutMs = 5000;
    static constexpr int maximumMessagesPerTurn = 8;

    std::unique_ptr<IpcSession> session_;
    QTimer *pollTimer_ = nullptr;
    QTimer *sendDeadlineTimer_ = nullptr;
    QTimer *shutdownDeadlineTimer_ = nullptr;
    quint64 generation_ = 0;
    QThread *ownerThread_ = nullptr;
    const std::optional<TabCapabilityAuthority> capabilityAuthority_;
    std::shared_ptr<HostWorkerSessionBindingState> liveBinding_;
    PipeWriteCancellation commandCancellation_;
    PipeWriteCancellation shutdownCancellation_;
    quint64 pendingCommandId_ = 0;
    bool awaitingGui_ = false;
    bool stopping_ = false;
    bool terminal_ = false;
};
