#pragma once

#include "IpcSession.h"
#include "SandboxLauncher.h"
#include "SandboxTrustBoundary.h"

#include <QTemporaryDir>

#include <functional>
#include <optional>

using SessionReceiveFunction = std::function<SessionReceiveResult(int)>;
using ElapsedMillisecondsFunction = std::function<qint64()>;

SessionReceiveResult receiveUntilWithClock(
    const SessionReceiveFunction &receive,
    const ElapsedMillisecondsFunction &elapsedMilliseconds,
    ProtocolType expectedType,
    int timeoutMs);

SessionReceiveResult receiveUntil(IpcSession &session,
                                  ProtocolType expectedType,
                                  int timeoutMs = 15000);

class WorkerTestEnvironment final
{
public:
    explicit WorkerTestEnvironment(QByteArray mainQml = {}, QString packageSource = {});
    ~WorkerTestEnvironment();

    WorkerTestEnvironment(const WorkerTestEnvironment &) = delete;
    WorkerTestEnvironment &operator=(const WorkerTestEnvironment &) = delete;

    bool isValid() const noexcept;
    QString error() const;
    QString appId() const;
    QString packageRoot() const;
    QString sandboxTempRoot() const;
    QString runtimeRoot() const;
    QString workerExecutable() const;
    [[nodiscard]] bool cleanup();
    [[nodiscard]] QString cleanupError() const;

    struct Launch final {
        IpcSession hostSession;
        SandboxProcess process;

        Launch(IpcSession host, SandboxProcess child);
        Launch(Launch &&) noexcept = default;
        Launch &operator=(Launch &&) noexcept = default;
        Launch(const Launch &) = delete;
        Launch &operator=(const Launch &) = delete;
    };

    std::optional<Launch> launch(const QString &workerNonce,
                                 const QString &hostNonce,
                                 int heartbeatMs = 50,
                                 const QString &apiOrigin = QStringLiteral("http://127.0.0.1:4173/"));

private:
    bool prepare();

    QTemporaryDir root_;
    QString appId_;
    QString packageRoot_;
    QString tempRoot_;
    QString runtimeRoot_;
    QString packageDirectory_;
    QString workerTemp_;
    QString workerExecutable_;
    QString error_;
    std::optional<SandboxTrustBoundary> boundary_;
    QByteArray mainQml_;
    QString packageSource_;
    QString cleanupError_;
    bool cleaned_ = false;
};
