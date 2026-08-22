#pragma once

#include "CapabilityBroker.h"
#include "Manifest.h"
#include "UserGestureGrantStore.h"

#include <QJsonObject>
#include <QObject>
#include <QUrl>

#include <memory>
#include <optional>

class CapabilityWorkerLane;
struct CapabilityDeliveryState;
class ClipboardBroker;
class FileBroker;
class QtClipboardBackend;
class QtFileDialogBackend;
class QThread;
class QTimer;

#ifdef Q_BROWSER_HOST_TESTING
struct HostWorkerGestureEvidence final
{
    quint32 now = 0;
    quint32 lastInput = 0;
    quint32 lastGrantedInput = 0;
    quint32 workerProcessId = 0;
    quint32 focusProcessId = 0;
    bool foregroundMatchesHostRoot = false;
    bool focusBelongsToWorkerWindow = false;
};

namespace qbrowser_host_testing
{
[[nodiscard]] bool isTrustedWorkerGesture(
    const HostWorkerGestureEvidence &evidence) noexcept;
}
#endif

class HostCapabilityRuntime final : public QObject
{
    Q_OBJECT
public:
    ~HostCapabilityRuntime() override;

    HostCapabilityRuntime(const HostCapabilityRuntime &) = delete;
    HostCapabilityRuntime &operator=(const HostCapabilityRuntime &) = delete;

    [[nodiscard]] static std::shared_ptr<HostCapabilityRuntime> create(
        const QString &appIdentity,
        const ManifestPermissions &permissions,
        const QUrl &mockOrigin,
        const QString &storageDirectory,
        quintptr hostWindowId,
        quintptr workerWindowId,
        quint32 workerProcessId,
        QString *errorCode = nullptr);
    static void retire(std::shared_ptr<HostCapabilityRuntime> runtime) noexcept;

    void dispatch(quint64 generation,
                  const QString &requestId,
                  const QString &capability,
                  const QString &operation,
                  const QJsonObject &payload);
    void invalidate() noexcept;

signals:
    void completed(quint64 generation,
                   const QString &requestId,
                   const BrokerResult &result);

private:
    HostCapabilityRuntime(QString appIdentity,
                          EffectivePolicy policy,
                          quintptr hostWindowId,
                          quintptr workerWindowId,
                          quint32 workerProcessId);
    [[nodiscard]] bool initialize(const QString &storageDirectory,
                                  QString *errorCode);

    QString appIdentity_;
    EffectivePolicy policy_;
    quintptr hostWindowId_ = 0;
    quintptr workerWindowId_ = 0;
    quint32 workerProcessId_ = 0;
    quint32 lastGrantedInputTick_ = 0;
    QThread *workerThread_ = nullptr;
    CapabilityWorkerLane *workerLane_ = nullptr;
    std::shared_ptr<CapabilityDeliveryState> deliveryState_;
    QTimer *deliveryTimer_ = nullptr;
    std::unique_ptr<QtClipboardBackend> clipboardBackend_;
    std::unique_ptr<QtFileDialogBackend> fileBackend_;
    std::unique_ptr<UserGestureGrantStore> gestureGrants_;
    std::optional<UserGestureSession> gestureSession_;
    std::unique_ptr<ClipboardBroker> clipboard_;
    std::unique_ptr<FileBroker> file_;
    std::unique_ptr<CapabilityBroker> guiBroker_;
    bool accepting_ = true;
};

Q_DECLARE_METATYPE(BrokerResult)
