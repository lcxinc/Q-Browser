#pragma once

#include "IpcSession.h"
#include "PendingCapabilityQueue.h"
#include "RuntimeFacade.h"

#include <QObject>
#include <QTimer>

#include <memory>

class WorkerWindow;

class WorkerApplication final : public QObject
{
    Q_OBJECT

public:
    explicit WorkerApplication(QObject *parent = nullptr);
    ~WorkerApplication() override;

    static constexpr int invalidLaunchExitCode() noexcept { return 64; }
    bool start(const QStringList &arguments);

private slots:
    void pollIpc();
    void sendHeartbeat();
    void sendCapabilityRequest(const QString &requestId,
                               const QString &capability,
                               const QString &operation,
                               const QJsonObject &payload);
    void sendNavigationRequest(const QString &requestId, const QString &route);

private:
    enum class State {
        Authenticating,
        Loading,
        Ready,
        Exiting,
    };

    struct LaunchArguments final {
        HANDLE readHandle = nullptr;
        HANDLE writeHandle = nullptr;
        QString nonce;
        QString packageDirectory;
        QString entryPoint;
        QString apiOrigin;
        int heartbeatMs = 0;
    };

    static std::optional<LaunchArguments> parseArguments(const QStringList &arguments);
    bool finishAuthentication();
    bool flushPendingCapabilities();
    void handleMessage(const ProtocolMessage &message);
    void failClosed(int exitCode = 70);

    std::unique_ptr<IpcSession> session_;
    std::unique_ptr<WorkerWindow> window_;
    RuntimeFacade runtimeFacade_;
    QTimer pollTimer_;
    QTimer heartbeatTimer_;
    LaunchArguments launch_;
    PendingCapabilityQueue pendingCapabilities_;
    State state_ = State::Authenticating;
};
