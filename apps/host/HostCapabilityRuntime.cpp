#include "HostCapabilityRuntime.h"

#include "ClipboardBroker.h"
#include "FileBroker.h"
#include "FileDialogCoordinator.h"
#include "HostGestureRouter.h"
#include "NetworkBroker.h"
#include "PolicyEngine.h"
#include "StorageBroker.h"
#include "UserGestureGrantStore.h"
#include "WorkerRetirementManager.h"

#include <QMetaObject>
#include <QCoreApplication>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#ifndef Q_BROWSER_HOST_TESTING
template <typename Runtime>
concept ExposesAuthoritylessFactory = requires(
    const QString &appIdentity,
    const ManifestPermissions &permissions,
    const QUrl &mockOrigin,
    const QString &storageDirectory,
    QString *errorCode) {
    Runtime::create(appIdentity, permissions, mockOrigin, storageDirectory,
                    quintptr{}, quintptr{}, quint32{}, errorCode);
};

template <typename Runtime>
concept ExposesAuthoritylessCompletion = requires {
    &Runtime::completed;
};

static_assert(!ExposesAuthoritylessFactory<HostCapabilityRuntime>,
              "production must not expose an authorityless factory");
static_assert(!ExposesAuthoritylessCompletion<HostCapabilityRuntime>,
              "production must not expose an authorityless completion signal");
#endif

namespace {
constexpr quint32 maximumGestureAgeMs = 1'000;
constexpr int issuedGestureLifetimeMs = 250;

struct WorkerGestureEvidence final
{
    quint32 now = 0;
    quint32 trustedWorkerInput = 0;
    quint32 lastGrantedInput = 0;
    quint32 workerProcessId = 0;
    quint32 focusProcessId = 0;
    bool foregroundMatchesHostRoot = false;
    bool focusBelongsToWorkerWindow = false;
};

bool isTrustedWorkerGesture(const WorkerGestureEvidence &evidence) noexcept
{
    if (evidence.workerProcessId == 0 || evidence.trustedWorkerInput == 0
        || evidence.trustedWorkerInput == evidence.lastGrantedInput
        || evidence.focusProcessId != evidence.workerProcessId
        || !evidence.foregroundMatchesHostRoot
        || !evidence.focusBelongsToWorkerWindow) {
        return false;
    }
    return static_cast<quint32>(evidence.now - evidence.trustedWorkerInput)
        <= maximumGestureAgeMs;
}

bool validMockOrigin(const QUrl &origin) noexcept
{
    if (!origin.isValid() || origin.scheme() != QStringLiteral("http")
        || origin.host() != QStringLiteral("127.0.0.1")
        || origin.port(-1) <= 0 || origin.port(-1) > 65'535
        || !origin.userName().isEmpty() || !origin.password().isEmpty()
        || origin.hasQuery() || origin.hasFragment()) {
        return false;
    }
    return origin.path().isEmpty() || origin.path() == QStringLiteral("/");
}

HostPolicy hostPolicyFor(const QUrl &mockOrigin)
{
    HostPolicy host;
    HostNetworkPolicy network;
    NetworkAllowRule rule;
    rule.scheme = NetworkScheme::Http;
    rule.host = mockOrigin.host();
    rule.port = static_cast<quint16>(mockOrigin.port());
    rule.pathPrefix = QStringLiteral("/api/");
    rule.methods = {HttpMethod::Get, HttpMethod::Post, HttpMethod::Patch};
    rule.addressClasses = {NetworkAddressClass::Loopback};
    network.rules = {rule};
    network.maximumRequestBytes = 64 * 1024;
    network.maximumResponseBytes = maximumIpcBinaryResultBytes();
    network.timeoutMs = 5'000;
    host.network = network;
    host.storage = HostStoragePolicy{1024 * 1024};
    host.clipboard = HostClipboardPolicy{false, true};
    host.file = HostFilePolicy{true, maximumIpcBinaryResultBytes()};
    return host;
}
}

struct PendingHostFileRequest final
{
    PendingHostFileRequest(TabCapabilityAuthority immutableAuthority,
                           const quint64 immutableGeneration,
                           QString immutableRequestId,
                           PreparedFileRequest immutablePrepared)
        : authority(std::move(immutableAuthority)),
          generation(immutableGeneration),
          requestId(std::move(immutableRequestId)),
          prepared(std::move(immutablePrepared))
    {
    }

    const TabCapabilityAuthority authority;
    const quint64 generation = 0;
    const QString requestId;
    PreparedFileRequest prepared;
    std::optional<FileDialogOperation> operation;
    std::atomic_bool callbackQuiesced{false};
};

class CapabilityWorkerLane final : public QObject
{
public:
    CapabilityWorkerLane(EffectivePolicy policy,
                         const QString &storageDirectory,
                         QString *errorCode)
    {
        if (policy.network.has_value()) {
            network_ = std::make_unique<NetworkBroker>(*policy.network);
        }
        if (policy.storage.has_value()) {
            storage_ = StorageBroker::create(*policy.storage, storageDirectory,
                                             errorCode);
            if (storage_ == nullptr) return;
        }
        broker_ = std::make_unique<CapabilityBroker>(
            std::move(policy),
            CapabilityServices{network_.get(), storage_.get(), nullptr, nullptr});
        valid_ = true;
    }

    [[nodiscard]] bool isValid() const noexcept { return valid_; }

    [[nodiscard]] BrokerResult dispatch(const QString &capability,
                                        const QString &operation,
                                        const QJsonObject &payload,
                                        const HostRequestContext &context)
    {
        return broker_->dispatch(capability, operation, payload, context);
    }

private:
    std::unique_ptr<NetworkBroker> network_;
    std::unique_ptr<StorageBroker> storage_;
    std::unique_ptr<CapabilityBroker> broker_;
    bool valid_ = false;
};

HostCapabilityRuntime::HostCapabilityRuntime(
    TabCapabilityAuthority authority,
    std::shared_ptr<AuthorityAdmissionToken> admissionToken,
    HostGestureRouter *gestureRouter,
#ifdef Q_BROWSER_HOST_TESTING
    const bool authorityEnforced,
#endif
    EffectivePolicy policy,
    const quintptr hostWindowId,
    FileDialogCoordinator *const fileDialogCoordinator)
    : authority_(std::move(authority)),
      admissionToken_(std::move(admissionToken)),
      gestureRouter_(gestureRouter),
#ifdef Q_BROWSER_HOST_TESTING
      authorityEnforced_(authorityEnforced),
#endif
      policy_(std::move(policy)), hostWindowId_(hostWindowId),
      fileDialogCoordinator_(fileDialogCoordinator)
{
}

HostCapabilityRuntime::~HostCapabilityRuntime()
{
    Q_ASSERT(workerThread_ == nullptr || !workerThread_->isRunning());
    delete std::exchange(workerThread_, nullptr);
}

#ifdef Q_BROWSER_HOST_TESTING
std::shared_ptr<HostCapabilityRuntime> HostCapabilityRuntime::create(
    const QString &appIdentity,
    const ManifestPermissions &permissions,
    const QUrl &mockOrigin,
    const QString &storageDirectory,
    const quintptr hostWindowId,
    const quintptr workerWindowId,
    const quint32 workerProcessId,
    QString *errorCode,
    FileDialogCoordinator *const fileDialogCoordinator)
{
    if (appIdentity.isEmpty() || storageDirectory.isEmpty()
        || !validMockOrigin(mockOrigin)) {
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral("host.capability.invalid_configuration");
        }
        return nullptr;
    }
    EffectivePolicy effective = PolicyEngine::intersect(
        permissions, hostPolicyFor(mockOrigin));
    TabCapabilityAuthority legacyAuthority;
    legacyAuthority.appIdentity = appIdentity;
    legacyAuthority.workerWindowId = workerWindowId;
    legacyAuthority.workerProcessId = workerProcessId;
    auto runtime = std::shared_ptr<HostCapabilityRuntime>(
        new HostCapabilityRuntime(
            std::move(legacyAuthority),
            std::make_shared<AuthorityAdmissionToken>(), nullptr, false,
            std::move(effective), hostWindowId, fileDialogCoordinator));
    if (!runtime->initialize(storageDirectory, errorCode)) return nullptr;
    if (errorCode != nullptr) errorCode->clear();
    return runtime;
}
#endif

std::shared_ptr<HostCapabilityRuntime> HostCapabilityRuntime::create(
    const TabCapabilityAuthority &authority,
    std::shared_ptr<AuthorityAdmissionToken> admissionToken,
    HostGestureRouter *gestureRouter,
    const ManifestPermissions &permissions,
    const QUrl &mockOrigin,
    const QString &storageDirectory,
    const quintptr hostWindowId,
    QString *errorCode,
    FileDialogCoordinator *const fileDialogCoordinator)
{
    if (!authority.isValid() || admissionToken == nullptr
        || gestureRouter == nullptr || hostWindowId == 0
        || storageDirectory.isEmpty() || !validMockOrigin(mockOrigin)) {
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral("host.capability.invalid_configuration");
        }
        return nullptr;
    }
    EffectivePolicy effective = PolicyEngine::intersect(
        permissions, hostPolicyFor(mockOrigin));
    auto runtime = std::shared_ptr<HostCapabilityRuntime>(
        new HostCapabilityRuntime(
            authority, std::move(admissionToken), gestureRouter,
#ifdef Q_BROWSER_HOST_TESTING
            true,
#endif
            std::move(effective), hostWindowId, fileDialogCoordinator));
    if (!runtime->initialize(storageDirectory, errorCode)) return nullptr;
    if (errorCode != nullptr) errorCode->clear();
    return runtime;
}

const TabCapabilityAuthority &HostCapabilityRuntime::authority() const noexcept
{
    return authority_;
}

bool HostCapabilityRuntime::bindCompletionSubmitter(
    const TabCapabilityAuthority &authority,
    CapabilityCompletionSubmitter submitter)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (QThread::currentThread() != thread() || !accepting_ || !submitter
        || authority != authority_ || completionSubmitter_) {
        return false;
    }
    completionSubmitter_ = std::move(submitter);
    return true;
}

void HostCapabilityRuntime::unbindCompletionSubmitter(
    const TabCapabilityAuthority &authority) noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (QThread::currentThread() == thread() && authority == authority_) {
        completionSubmitter_ = {};
    }
}

std::shared_ptr<AuthorityAdmissionToken::UseGuard>
HostCapabilityRuntime::acquireResponseUse(
    const TabCapabilityAuthority &authority,
    const quint64 generation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (QThread::currentThread() != thread() || !accepting_
        || admissionToken_ == nullptr || authority != authority_
        || !generationMatches(generation)) {
        return nullptr;
    }
    auto acquired = admissionToken_->tryAcquireUse();
    if (!acquired.has_value()) return nullptr;
    return std::make_shared<AuthorityAdmissionToken::UseGuard>(
        std::move(*acquired));
}

void HostCapabilityRuntime::retire(
    std::shared_ptr<HostCapabilityRuntime> runtime) noexcept
{
    if (runtime == nullptr) return;
    Q_ASSERT(QThread::currentThread() == runtime->thread());
    runtime->invalidate();
    const std::shared_ptr<PendingHostFileRequest> pendingFile =
        runtime->pendingFile_;
    (void)QObject::disconnect(runtime.get(), nullptr, nullptr, nullptr);
    QThread *const thread = std::exchange(runtime->workerThread_, nullptr);
    if (thread == nullptr) return;
    thread->requestInterruption();
    thread->quit();
    struct RetirementState final
    {
        std::shared_ptr<HostCapabilityRuntime> runtime;
        std::shared_ptr<PendingHostFileRequest> pendingFile;
        QThread *thread = nullptr;
    };
    auto state = std::make_shared<RetirementState>(
        RetirementState{std::move(runtime), pendingFile, thread});
    (void)WorkerRetirementManager::instance().retire(
        [state] {
            if (!state->thread->wait(1'000)) {
                return WorkerRetirementAttemptResult{
                    false,
                    QStringLiteral("host.capability.retirement_pending")};
            }
            if (state->pendingFile != nullptr
                && !state->pendingFile->callbackQuiesced.load(
                    std::memory_order_acquire)) {
                return WorkerRetirementAttemptResult{
                    false,
                    QStringLiteral(
                        "host.capability.file_callback_retirement_pending")};
            }
            return WorkerRetirementAttemptResult{true, {}};
        },
        [state](const bool succeeded, const QString &) {
            if (!succeeded) return;
            QObject *const guiContext = QCoreApplication::instance();
            if (guiContext == nullptr) return;
            (void)QMetaObject::invokeMethod(
                guiContext,
                [state] {
                    state->runtime->workerLane_ = nullptr;
                    state->runtime->pendingFile_.reset();
                    state->pendingFile.reset();
                    delete std::exchange(state->thread, nullptr);
                    state->runtime.reset();
                }, Qt::QueuedConnection);
        });
}

bool HostCapabilityRuntime::initialize(const QString &storageDirectory,
                                       QString *errorCode)
{
    if (policy_.file.has_value() && fileDialogCoordinator_ == nullptr) {
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral(
                "host.capability.file_coordinator_unavailable");
        }
        return false;
    }
    auto *const lane = new CapabilityWorkerLane(policy_, storageDirectory, errorCode);
    auto *const thread = new QThread;
    thread->setObjectName(QStringLiteral("host-capability-worker"));
    if (!lane->isValid()) {
        delete lane;
        delete thread;
        if (errorCode != nullptr && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("host.capability.worker_unavailable");
        }
        return false;
    }
    clipboardBackend_ = std::make_unique<QtClipboardBackend>();
    fileBackend_ = std::make_unique<QtFileDialogBackend>();
    gestureGrants_ = std::make_shared<UserGestureGrantStore>();
#ifdef Q_BROWSER_HOST_TESTING
    if (authorityEnforced_) {
#endif
        auto gestureSession = gestureGrants_->openSession(authority_.appIdentity);
        QString gestureError;
        if (!gestureSession.has_value()
            || gestureRouter_ == nullptr
            || !gestureRouter_->registerBinding(
                authority_, admissionToken_, gestureGrants_,
                std::move(*gestureSession), &gestureError)) {
            if (errorCode != nullptr) {
                *errorCode = gestureError.isEmpty()
                    ? QStringLiteral("host.capability.gesture_session_unavailable")
                    : gestureError;
            }
            (void)admissionToken_->beginRevoke();
            delete lane;
            delete thread;
            return false;
        }
        gestureBindingRegistered_ = true;
#ifdef Q_BROWSER_HOST_TESTING
    }
#endif
    if (policy_.clipboard.has_value()) {
        clipboard_ = std::make_unique<ClipboardBroker>(
            *policy_.clipboard, *clipboardBackend_, *gestureGrants_);
    }
    if (policy_.file.has_value()) {
        file_ = std::make_unique<FileBroker>(*policy_.file, *fileBackend_);
    }
    guiBroker_ = std::make_unique<CapabilityBroker>(
        policy_, CapabilityServices{nullptr, nullptr, clipboard_.get(), nullptr});
    if (!lane->moveToThread(thread)) {
        if (gestureBindingRegistered_ && gestureRouter_ != nullptr) {
            gestureRouter_->unregisterBinding(authority_);
            gestureBindingRegistered_ = false;
        }
        delete lane;
        delete thread;
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral("host.capability.worker_unavailable");
        }
        return false;
    }
    connect(thread, &QThread::finished, lane, &QObject::deleteLater);
    workerLane_ = lane;
    workerThread_ = thread;
    thread->start();
    return true;
}

void HostCapabilityRuntime::dispatch(const quint64 generation,
                                     const QString &requestId,
                                     const QString &capability,
                                     const QString &operation,
                                     const QJsonObject &payload)
{
    const std::shared_ptr<HostCapabilityRuntime> lifetime =
        weak_from_this().lock();
    if (lifetime == nullptr) return;
    if (!accepting_ || admissionToken_ == nullptr
        || !generationMatches(generation)) {
        return;
    }
    auto acquired = admissionToken_->tryAcquireUse();
    if (!acquired.has_value()) return;
    auto use = std::make_shared<AuthorityAdmissionToken::UseGuard>(
        std::move(*acquired));
    if (capability == QStringLiteral("file")) {
        dispatchFile(std::move(use), generation, requestId, operation, payload);
        return;
    }
    const HostRequestContext context{authority_.appIdentity, requestId, nullptr};
    if (capability == QStringLiteral("network")
        || capability == QStringLiteral("storage")) {
        CapabilityWorkerLane *const lane = workerLane_;
        if (lane == nullptr || !QMetaObject::invokeMethod(
                lane,
                [this, lane, use, generation, requestId, capability, operation,
                 payload, context] {
                     const BrokerResult result = lane->dispatch(
                         capability, operation, payload, context);
                    (void)queueCompletion(
                        use, generation, requestId, result);
                }, Qt::QueuedConnection)) {
            (void)queueCompletion(
                use, generation, requestId,
                BrokerResult::failure(
                    QStringLiteral("capability.unavailable"),
                    QStringLiteral("Capability service is unavailable.")));
        }
        return;
    }
    std::optional<UserGestureGrant> grant;
    if (capability == QStringLiteral("clipboard")
        && operation == QStringLiteral("read")
        && gestureRouter_ != nullptr
#ifdef Q_BROWSER_HOST_TESTING
        && authorityEnforced_
#endif
        ) {
        grant = gestureRouter_->issueGrant(
            authority_, requestId, issuedGestureLifetimeMs);
    }
    const HostRequestContext guiContext{
        authority_.appIdentity, requestId,
        grant.has_value() ? &*grant : nullptr};
    const BrokerResult result = guiBroker_->dispatch(
        capability, operation, payload, guiContext);
    (void)queueCompletion(use, generation, requestId, result);
}

void HostCapabilityRuntime::dispatchFile(
    std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
    const quint64 generation,
    const QString &requestId,
    const QString &operation,
    const QJsonObject &payload)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (use == nullptr || !accepting_ || !generationMatches(generation)) {
        return;
    }
    const auto denied = [] {
        return BrokerResult::failure(
            QStringLiteral("capability.denied"),
            QStringLiteral("Capability is not permitted."));
    };
    if (gestureRouter_ == nullptr
        || !gestureRouter_->isActiveBinding(authority_)) {
        (void)queueCompletion(std::move(use), generation, requestId, denied());
        return;
    }
    if (pendingFile_ != nullptr) {
        (void)queueCompletion(
            std::move(use), generation, requestId,
            BrokerResult::failure(QStringLiteral("file.busy"),
                                  QStringLiteral("File dialog is busy.")));
        return;
    }
    if (file_ == nullptr || fileDialogCoordinator_ == nullptr) {
        (void)queueCompletion(std::move(use), generation, requestId, denied());
        return;
    }

    PreparedFileRequestResult preparation = file_->prepareFileRequest(
        operation, payload, {authority_.appIdentity, requestId, nullptr});
    if (!preparation.request.has_value()) {
        (void)queueCompletion(std::move(use), generation, requestId,
                              preparation.rejection);
        return;
    }
    auto pending = std::make_shared<PendingHostFileRequest>(
        authority_, generation, requestId, std::move(*preparation.request));
    pendingFile_ = pending;
    const std::weak_ptr<HostCapabilityRuntime> weakRuntime = weak_from_this();
    std::optional<FileDialogOperation> operationHandle;
    bool admitted = false;
    try {
        admitted = use->publishIfStillAdmitted(
            [this, weakRuntime, pending, &operationHandle] {
                operationHandle = fileDialogCoordinator_->openAsync(
                    FileDialogOpenRequest(
                        pending->prepared,
                        [weakRuntime, pending] {
                            const auto runtime = weakRuntime.lock();
                            return runtime != nullptr
                                && runtime->isActiveFileOwner(pending);
                        }),
                    [weakRuntime, pending](
                        const FileDialogOperationToken &token,
                        FileDialogSelection selection) mutable {
                        try {
                            if (const auto runtime = weakRuntime.lock();
                                runtime != nullptr) {
                                (void)QMetaObject::invokeMethod(
                                    runtime.get(),
                                    [weakRuntime, pending, token,
                                     selection = std::move(selection)]() mutable {
                                        if (const auto owner = weakRuntime.lock();
                                            owner != nullptr) {
                                            owner->completeFile(
                                                pending, token,
                                                std::move(selection));
                                        }
                                    },
                                    Qt::QueuedConnection);
                            }
                        } catch (...) {
                        }
                        pending->callbackQuiesced.store(
                            true, std::memory_order_release);
                    });
                return operationHandle.has_value();
            });
    } catch (...) {
        admitted = false;
    }
    if (!admitted || !operationHandle.has_value()) {
        pending->callbackQuiesced.store(true, std::memory_order_release);
        if (pendingFile_ == pending) pendingFile_.reset();
        (void)queueCompletion(
            std::move(use), generation, requestId,
            BrokerResult::failure(
                QStringLiteral("file.failed"),
                QStringLiteral("Selected file is unavailable.")));
        return;
    }
    pending->operation = std::move(*operationHandle);
}

void HostCapabilityRuntime::completeFile(
    const std::shared_ptr<PendingHostFileRequest> &pending,
    const FileDialogOperationToken &token,
    FileDialogSelection selection)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (pending == nullptr || pendingFile_ != pending
        || !pending->operation.has_value()
        || !(pending->operation->token == token)) {
        return;
    }
    if (!accepting_ || admissionToken_ == nullptr
        || pending->authority != authority_
        || pending->generation != authority_.sessionGeneration
        || pending->generation != pending->authority.sessionGeneration
        || pending->requestId.isEmpty()
        || pending->requestId != pending->prepared.requestId()
        || pending->prepared.appIdentity() != authority_.appIdentity) {
        pendingFile_.reset();
        return;
    }
    auto acquired = admissionToken_->tryAcquireUse();
    if (!acquired.has_value()) {
        pendingFile_.reset();
        return;
    }
    auto use = std::make_shared<AuthorityAdmissionToken::UseGuard>(
        std::move(*acquired));
    if (!isActiveFileOwner(pending)) {
        selection = {};
        selection.status = FileDialogStatus::Denied;
        selection.approvedMaximumBytes = pending->prepared.maximumBytes();
    }
    const BrokerResult result = file_->completeFileRequest(
        pending->prepared, selection);
    pendingFile_.reset();
    (void)queueCompletion(std::move(use), pending->generation,
                          pending->requestId, result);
}

bool HostCapabilityRuntime::isActiveFileOwner(
    const std::shared_ptr<PendingHostFileRequest> &pending) const noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    return pending != nullptr && pendingFile_ == pending && accepting_
        && pending->authority == authority_
        && pending->generation == authority_.sessionGeneration
        && pending->requestId == pending->prepared.requestId()
        && pending->prepared.appIdentity() == authority_.appIdentity
        && gestureRouter_ != nullptr
        && gestureRouter_->isActiveBinding(authority_);
}

bool HostCapabilityRuntime::generationMatches(
    const quint64 generation) const noexcept
{
#ifdef Q_BROWSER_HOST_TESTING
    return !authorityEnforced_
        || generation == authority_.sessionGeneration;
#else
    return generation == authority_.sessionGeneration;
#endif
}

bool HostCapabilityRuntime::queueCompletion(
    std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
    const quint64 generation,
    const QString &requestId,
    const BrokerResult &result)
{
    if (use == nullptr) return false;
    const TabCapabilityAuthority immutableAuthority = authority_;
    const std::weak_ptr<HostCapabilityRuntime> weakRuntime = weak_from_this();
    return QMetaObject::invokeMethod(
        this,
        [weakRuntime, use = std::move(use), immutableAuthority, generation,
         requestId, result] {
            const auto runtime = weakRuntime.lock();
            if (runtime == nullptr) return;
            (void)runtime->publishCompletion(
                use, immutableAuthority, generation, requestId, result);
        },
        Qt::QueuedConnection);
}

bool HostCapabilityRuntime::publishCompletion(
    std::shared_ptr<AuthorityAdmissionToken::UseGuard> use,
    const TabCapabilityAuthority &immutableAuthority,
    const quint64 generation,
    const QString &requestId,
    const BrokerResult &result)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (use == nullptr || !accepting_ || immutableAuthority != authority_
        || !generationMatches(generation)) {
        return false;
    }
    const auto delivery = std::make_shared<CapabilityDeliveryState>(
        immutableAuthority, generation, requestId, result, use);
    return use->publishIfStillAdmitted(
        [this, delivery] {
            const TabCapabilityAuthority &immutableAuthority =
                delivery->authority;
            const quint64 generation = delivery->generation;
            if (!accepting_ || immutableAuthority != authority_
                || !generationMatches(generation)) {
                return false;
            }
            if (completionSubmitter_) {
                const CapabilityCompletionSubmitter submitter =
                    completionSubmitter_;
                return submitter && submitter(delivery);
            }
            emit authorityCompleted(immutableAuthority, generation,
                                    delivery->requestId, delivery->result);
#ifdef Q_BROWSER_HOST_TESTING
            emit completed(generation, delivery->requestId, delivery->result);
#endif
            return true;
        });
}

void HostCapabilityRuntime::invalidate() noexcept
{
    if (!accepting_) return;
    accepting_ = false;
    completionSubmitter_ = {};
    if (pendingFile_ != nullptr && pendingFile_->operation.has_value()) {
        (void)pendingFile_->operation->cancellation.cancel();
    }
    if (gestureBindingRegistered_ && gestureRouter_ != nullptr) {
        gestureRouter_->unregisterBinding(authority_);
        gestureBindingRegistered_ = false;
    } else if (admissionToken_ != nullptr) {
        try {
            (void)admissionToken_->beginRevoke();
        } catch (...) {
        }
    }
}

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
bool isTrustedWorkerGesture(const HostWorkerGestureEvidence &evidence) noexcept
{
    return ::isTrustedWorkerGesture(WorkerGestureEvidence{
        evidence.now,
        evidence.trustedWorkerInput,
        evidence.lastGrantedInput,
        evidence.workerProcessId,
        evidence.focusProcessId,
        evidence.foregroundMatchesHostRoot,
        evidence.focusBelongsToWorkerWindow});
}
}
#endif
