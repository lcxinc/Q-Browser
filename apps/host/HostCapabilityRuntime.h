#pragma once

#include "CapabilityBroker.h"
#include "Manifest.h"
#include "TabCapabilityAuthority.h"
#include "UserGestureGrantStore.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class CapabilityWorkerLane;
class ClipboardBroker;
class FileBroker;
class FileDialogCoordinator;
class FileDialogOperationToken;
struct FileDialogSelection;
class QtClipboardBackend;
class QtFileDialogBackend;
class HostGestureRouter;
class QThread;
struct PendingHostFileRequest;

struct CapabilityDeliveryState final
{
    CapabilityDeliveryState(
        TabCapabilityAuthority immutableAuthority,
        quint64 immutableGeneration,
        QString immutableRequestId,
        BrokerResult immutableResult,
        std::shared_ptr<AuthorityAdmissionToken::UseGuard> retainedUse)
        : authority(std::move(immutableAuthority)),
          generation(immutableGeneration),
          requestId(std::move(immutableRequestId)),
          result(std::move(immutableResult)),
          use(std::move(retainedUse))
    {
    }

    const TabCapabilityAuthority authority;
    const quint64 generation = 0;
    const QString requestId;
    const BrokerResult result;
    const std::shared_ptr<AuthorityAdmissionToken::UseGuard> use;
};

using CapabilityCompletionSubmitter = std::function<bool(
    const std::shared_ptr<CapabilityDeliveryState> &delivery)>;

#ifdef Q_BROWSER_HOST_TESTING
struct HostWorkerGestureEvidence final
{
    quint32 now = 0;
    quint32 trustedWorkerInput = 0;
    quint32 lastSystemInput = 0;
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

class HostCapabilityRuntime final
    : public QObject,
      public std::enable_shared_from_this<HostCapabilityRuntime>
{
    Q_OBJECT
public:
    ~HostCapabilityRuntime() override;

    HostCapabilityRuntime(const HostCapabilityRuntime &) = delete;
    HostCapabilityRuntime &operator=(const HostCapabilityRuntime &) = delete;

#ifdef Q_BROWSER_HOST_TESTING
    [[nodiscard]] static std::shared_ptr<HostCapabilityRuntime> create(
        const QString &appIdentity,
        const ManifestPermissions &permissions,
        const QUrl &mockOrigin,
        const QString &storageDirectory,
        quintptr hostWindowId,
        quintptr workerWindowId,
        quint32 workerProcessId,
        QString *errorCode = nullptr,
        FileDialogCoordinator *fileDialogCoordinator = nullptr);
#endif
    [[nodiscard]] static std::shared_ptr<HostCapabilityRuntime> create(
        const TabCapabilityAuthority &authority,
        std::shared_ptr<AuthorityAdmissionToken> admissionToken,
        HostGestureRouter *gestureRouter,
        const ManifestPermissions &permissions,
        const QUrl &mockOrigin,
        const QString &storageDirectory,
        quintptr hostWindowId,
        QString *errorCode,
        FileDialogCoordinator *fileDialogCoordinator);
    static void retire(std::shared_ptr<HostCapabilityRuntime> runtime) noexcept;

    [[nodiscard]] const TabCapabilityAuthority &authority() const noexcept;
    [[nodiscard]] bool bindCompletionSubmitter(
        const TabCapabilityAuthority &authority,
        CapabilityCompletionSubmitter submitter);
    void unbindCompletionSubmitter(
        const TabCapabilityAuthority &authority) noexcept;
    [[nodiscard]] std::shared_ptr<AuthorityAdmissionToken::UseGuard>
    acquireResponseUse(const TabCapabilityAuthority &authority,
                       quint64 generation);

    void dispatch(quint64 generation,
                  const QString &requestId,
                  const QString &capability,
                  const QString &operation,
                  const QJsonObject &payload);
    void invalidate() noexcept;

signals:
#ifdef Q_BROWSER_HOST_TESTING
    void completed(quint64 generation,
                   const QString &requestId,
                   const BrokerResult &result);
#endif
    void authorityCompleted(const TabCapabilityAuthority &authority,
                            quint64 generation,
                            const QString &requestId,
                            const BrokerResult &result);

private:
    HostCapabilityRuntime(TabCapabilityAuthority authority,
                          std::shared_ptr<AuthorityAdmissionToken> admissionToken,
                          HostGestureRouter *gestureRouter,
#ifdef Q_BROWSER_HOST_TESTING
                          bool authorityEnforced,
#endif
                          EffectivePolicy policy,
                          quintptr hostWindowId,
                          FileDialogCoordinator *fileDialogCoordinator);
    [[nodiscard]] bool initialize(const QString &storageDirectory,
                                  QString *errorCode);
    [[nodiscard]] bool queueCompletion(
        std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
        quint64 generation,
        const QString &requestId,
        const BrokerResult &result);
    [[nodiscard]] bool publishCompletion(
        std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
        const TabCapabilityAuthority &authority,
        quint64 generation,
        const QString &requestId,
        const BrokerResult &result);
    void dispatchFile(
        std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
        quint64 generation,
        const QString &requestId,
        const QString &operation,
        const QJsonObject &payload);
    void completeFile(
        const std::shared_ptr<PendingHostFileRequest> &pending,
        const FileDialogOperationToken &token,
        FileDialogSelection selection);
    [[nodiscard]] bool isActiveFileOwner(
        const std::shared_ptr<PendingHostFileRequest> &pending) const noexcept;
    [[nodiscard]] bool generationMatches(quint64 generation) const noexcept;

    const TabCapabilityAuthority authority_;
    const std::shared_ptr<AuthorityAdmissionToken> admissionToken_;
    QPointer<HostGestureRouter> gestureRouter_;
#ifdef Q_BROWSER_HOST_TESTING
    const bool authorityEnforced_ = false;
#endif
    EffectivePolicy policy_;
    quintptr hostWindowId_ = 0;
    FileDialogCoordinator *fileDialogCoordinator_ = nullptr;
    QThread *workerThread_ = nullptr;
    QPointer<CapabilityWorkerLane> workerLane_;
    std::unique_ptr<QtClipboardBackend> clipboardBackend_;
    std::unique_ptr<QtFileDialogBackend> fileBackend_;
    std::shared_ptr<UserGestureGrantStore> gestureGrants_;
    std::unique_ptr<ClipboardBroker> clipboard_;
    std::unique_ptr<FileBroker> file_;
    std::unique_ptr<CapabilityBroker> guiBroker_;
    std::shared_ptr<PendingHostFileRequest> pendingFile_;
    CapabilityCompletionSubmitter completionSubmitter_;
    bool gestureBindingRegistered_ = false;
    bool accepting_ = true;
};

Q_DECLARE_METATYPE(BrokerResult)
