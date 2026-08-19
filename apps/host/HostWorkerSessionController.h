#pragma once

#include "IpcSession.h"

#include <QHash>
#include <QObject>

#include <memory>

class MainWindow;
class QTimer;

enum class HostWorkerSessionState
{
    Detached,
    Running,
    Failed,
};

Q_DECLARE_METATYPE(HostWorkerSessionState)

class HostWorkerSessionController final : public QObject
{
    Q_OBJECT

public:
    explicit HostWorkerSessionController(MainWindow *window,
                                         QObject *parent = nullptr);
    ~HostWorkerSessionController() override;

    [[nodiscard]] bool attach(std::unique_ptr<IpcSession> session);
    [[nodiscard]] bool shutdown(const QString &reason);
    [[nodiscard]] HostWorkerSessionState state() const noexcept;
    [[nodiscard]] QString lastErrorCode() const;
    [[nodiscard]] qsizetype pendingRouteLoadCount() const noexcept;

signals:
    void failed(const QString &errorCode);

private slots:
    void pollSession();

private:
    void handleMessage(const ProtocolMessage &message);
    void handleNavigationRequest(const ProtocolMessage &message);
    void handleRouteLoadResponse(const ProtocolMessage &message);
    void failClosed(const QString &errorCode);

    static constexpr int sendTimeoutMs = 5000;
    static constexpr int routeLoadTimeoutMs = 5000;

    MainWindow *window_ = nullptr;
    std::unique_ptr<IpcSession> session_;
    QTimer *pollTimer_ = nullptr;
    QHash<QString, QString> pendingRouteLoads_;
    HostWorkerSessionState state_ = HostWorkerSessionState::Detached;
    QString lastErrorCode_;
    quint64 nextRouteLoadId_ = 0;
};
