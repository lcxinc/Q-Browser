#include "HostCapabilityRuntime.h"

#include "ClipboardBroker.h"
#include "FileBroker.h"
#include "NetworkBroker.h"
#include "PolicyEngine.h"
#include "StorageBroker.h"
#include "UserGestureGrantStore.h"
#include "WorkerRetirementManager.h"

#include <QMetaObject>
#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <windows.h>

#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

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

bool windowBelongsToRoot(const HWND candidate, const HWND root) noexcept
{
    if (candidate == nullptr || root == nullptr) return false;
    return candidate == root || IsChild(root, candidate)
        || GetAncestor(candidate, GA_ROOT) == root;
}

WorkerGestureEvidence queryWorkerGestureEvidence(const quintptr hostWindowId,
                                                 const quintptr workerWindowId,
                                                 const quint32 workerProcessId,
                                                 const quint32 trustedWorkerInput,
                                                 const quint32 lastGrantedInput) noexcept
{
    WorkerGestureEvidence evidence;
    evidence.now = GetTickCount();
    evidence.trustedWorkerInput = trustedWorkerInput;
    evidence.lastGrantedInput = lastGrantedInput;
    evidence.workerProcessId = workerProcessId;
    const HWND workerWindow = reinterpret_cast<HWND>(workerWindowId);
    if (workerWindow == nullptr || !IsWindow(workerWindow)) return evidence;

    const HWND hostRoot = GetAncestor(reinterpret_cast<HWND>(hostWindowId), GA_ROOT);
    const HWND foreground = GetForegroundWindow();
    evidence.foregroundMatchesHostRoot = foreground != nullptr
        && hostRoot != nullptr && GetAncestor(foreground, GA_ROOT) == hostRoot;

    DWORD actualWorkerPid = 0;
    const DWORD workerThread = GetWindowThreadProcessId(workerWindow,
                                                        &actualWorkerPid);
    if (workerThread == 0 || actualWorkerPid != workerProcessId) return evidence;
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(workerThread, &gui)) return evidence;
    DWORD focusPid = 0;
    (void)GetWindowThreadProcessId(gui.hwndFocus, &focusPid);
    evidence.focusProcessId = focusPid;
    evidence.focusBelongsToWorkerWindow = windowBelongsToRoot(gui.hwndFocus,
                                                              workerWindow);
    return evidence;
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

class TrustedWorkerInputObserver final
{
public:
    TrustedWorkerInputObserver(const TrustedWorkerInputObserver &) = delete;
    TrustedWorkerInputObserver &operator=(const TrustedWorkerInputObserver &) = delete;

    ~TrustedWorkerInputObserver()
    {
        if (keyboardHook_ != nullptr) (void)UnhookWindowsHookEx(keyboardHook_);
        if (mouseHook_ != nullptr) (void)UnhookWindowsHookEx(mouseHook_);
        if (active_ == this) active_ = nullptr;
    }

    [[nodiscard]] static std::unique_ptr<TrustedWorkerInputObserver> create(
        const quintptr hostWindowId, const quintptr workerWindowId,
        const quint32 workerProcessId)
    {
        if (active_ != nullptr || hostWindowId == 0 || workerWindowId == 0
            || workerProcessId == 0) {
            return nullptr;
        }
        auto observer = std::unique_ptr<TrustedWorkerInputObserver>(
            new TrustedWorkerInputObserver(hostWindowId, workerWindowId,
                                           workerProcessId));
        active_ = observer.get();
        const HINSTANCE module = GetModuleHandleW(nullptr);
        observer->mouseHook_ = SetWindowsHookExW(
            WH_MOUSE_LL, &TrustedWorkerInputObserver::mouseHook, module, 0);
        observer->keyboardHook_ = SetWindowsHookExW(
            WH_KEYBOARD_LL, &TrustedWorkerInputObserver::keyboardHook, module, 0);
        if (observer->mouseHook_ == nullptr || observer->keyboardHook_ == nullptr) {
            return nullptr;
        }
        return observer;
    }

    [[nodiscard]] quint32 lastTrustedInput() const noexcept
    {
        return lastTrustedInput_;
    }

private:
    TrustedWorkerInputObserver(const quintptr hostWindowId,
                               const quintptr workerWindowId,
                               const quint32 workerProcessId)
        : hostWindow_(reinterpret_cast<HWND>(hostWindowId)),
          workerWindow_(reinterpret_cast<HWND>(workerWindowId)),
          workerProcessId_(workerProcessId)
    {
    }

    [[nodiscard]] bool foregroundMatchesHostRoot() const noexcept
    {
        const HWND hostRoot = GetAncestor(hostWindow_, GA_ROOT);
        const HWND foreground = GetForegroundWindow();
        return hostRoot != nullptr && foreground != nullptr
            && GetAncestor(foreground, GA_ROOT) == hostRoot;
    }

    [[nodiscard]] bool foregroundAndFocusMatchWorker() const noexcept
    {
        if (!foregroundMatchesHostRoot()) return false;
        DWORD actualWorkerPid = 0;
        const DWORD workerThread = GetWindowThreadProcessId(workerWindow_,
                                                             &actualWorkerPid);
        if (workerThread == 0 || actualWorkerPid != workerProcessId_) return false;
        GUITHREADINFO gui{sizeof(gui)};
        if (!GetGUIThreadInfo(workerThread, &gui)) return false;
        DWORD focusPid = 0;
        (void)GetWindowThreadProcessId(gui.hwndFocus, &focusPid);
        return focusPid == workerProcessId_
            && windowBelongsToRoot(gui.hwndFocus, workerWindow_);
    }

    void observeMouse(const WPARAM message,
                      const MSLLHOOKSTRUCT &input) noexcept
    {
        if (message != WM_LBUTTONDOWN && message != WM_RBUTTONDOWN
            && message != WM_MBUTTONDOWN && message != WM_XBUTTONDOWN) {
            return;
        }
        const HWND target = WindowFromPoint(input.pt);
        DWORD targetPid = 0;
        (void)GetWindowThreadProcessId(target, &targetPid);
        const bool targetMatches = targetPid == workerProcessId_
            && windowBelongsToRoot(target, workerWindow_);
        if (targetMatches && foregroundMatchesHostRoot()) {
            lastTrustedInput_ = input.time;
        }
    }

    void observeKeyboard(const WPARAM message,
                         const KBDLLHOOKSTRUCT &input) noexcept
    {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
            && foregroundAndFocusMatchWorker()) {
            lastTrustedInput_ = input.time;
        }
    }

    static LRESULT CALLBACK mouseHook(const int code, const WPARAM message,
                                      const LPARAM data) noexcept
    {
        if (code == HC_ACTION && active_ != nullptr && data != 0) {
            active_->observeMouse(message,
                *reinterpret_cast<const MSLLHOOKSTRUCT *>(data));
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK keyboardHook(const int code, const WPARAM message,
                                         const LPARAM data) noexcept
    {
        if (code == HC_ACTION && active_ != nullptr && data != 0) {
            active_->observeKeyboard(message,
                *reinterpret_cast<const KBDLLHOOKSTRUCT *>(data));
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static thread_local TrustedWorkerInputObserver *active_;
    HWND hostWindow_ = nullptr;
    HWND workerWindow_ = nullptr;
    quint32 workerProcessId_ = 0;
    quint32 lastTrustedInput_ = 0;
    HHOOK mouseHook_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
};

thread_local TrustedWorkerInputObserver *TrustedWorkerInputObserver::active_ = nullptr;

struct CapabilityDelivery final
{
    quint64 generation = 0;
    QString requestId;
    BrokerResult result;
};

struct CapabilityDeliveryState final
{
    std::mutex mutex;
    std::deque<CapabilityDelivery> pending;
    std::atomic_bool accepting{true};
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

HostCapabilityRuntime::HostCapabilityRuntime(QString appIdentity,
                                             EffectivePolicy policy,
                                             const quintptr hostWindowId,
                                             const quintptr workerWindowId,
                                             const quint32 workerProcessId)
    : appIdentity_(std::move(appIdentity)),
      policy_(std::move(policy)), hostWindowId_(hostWindowId),
      workerWindowId_(workerWindowId),
      workerProcessId_(workerProcessId)
{
}

HostCapabilityRuntime::~HostCapabilityRuntime()
{
    Q_ASSERT(workerThread_ == nullptr || !workerThread_->isRunning());
    delete std::exchange(workerThread_, nullptr);
}

std::shared_ptr<HostCapabilityRuntime> HostCapabilityRuntime::create(
    const QString &appIdentity,
    const ManifestPermissions &permissions,
    const QUrl &mockOrigin,
    const QString &storageDirectory,
    const quintptr hostWindowId,
    const quintptr workerWindowId,
    const quint32 workerProcessId,
    QString *errorCode)
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
    auto runtime = std::shared_ptr<HostCapabilityRuntime>(
        new HostCapabilityRuntime(appIdentity, std::move(effective),
                                  hostWindowId, workerWindowId, workerProcessId));
    if (!runtime->initialize(storageDirectory, errorCode)) return nullptr;
    if (errorCode != nullptr) errorCode->clear();
    return runtime;
}

void HostCapabilityRuntime::retire(
    std::shared_ptr<HostCapabilityRuntime> runtime) noexcept
{
    if (runtime == nullptr) return;
    runtime->invalidate();
    (void)QObject::disconnect(runtime.get(), nullptr, nullptr, nullptr);
    if (runtime->deliveryState_ != nullptr) {
        runtime->deliveryState_->accepting.store(false, std::memory_order_release);
        std::lock_guard lock(runtime->deliveryState_->mutex);
        runtime->deliveryState_->pending.clear();
    }
    if (runtime->deliveryTimer_ != nullptr) runtime->deliveryTimer_->stop();
    runtime->guiBroker_.reset();
    runtime->file_.reset();
    runtime->clipboard_.reset();
    runtime->gestureSession_.reset();
    runtime->gestureGrants_.reset();
    runtime->inputObserver_.reset();
    runtime->fileBackend_.reset();
    runtime->clipboardBackend_.reset();
    CapabilityWorkerLane *const lane = runtime->workerLane_;
    QThread *const thread = runtime->workerThread_;
    if (thread == nullptr) return;
    if (lane != nullptr && thread->isRunning()) {
        if (!QMetaObject::invokeMethod(
                lane, [lane] {
                    delete lane;
                    QThread::currentThread()->quit();
                }, Qt::QueuedConnection)) {
            thread->quit();
        }
    } else {
        thread->quit();
    }
    struct RetirementState final
    {
        std::shared_ptr<HostCapabilityRuntime> runtime;
        QThread *thread = nullptr;
    };
    auto state = std::make_shared<RetirementState>(
        RetirementState{std::move(runtime), thread});
    (void)WorkerRetirementManager::instance().retire(
        [state] {
            if (!state->thread->wait(1'000)) {
                return WorkerRetirementAttemptResult{
                    false,
                    QStringLiteral("host.capability.retirement_pending")};
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
                    state->runtime->workerThread_ = nullptr;
                    delete std::exchange(state->thread, nullptr);
                    state->runtime.reset();
                }, Qt::QueuedConnection);
        });
}

bool HostCapabilityRuntime::initialize(const QString &storageDirectory,
                                       QString *errorCode)
{
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
    gestureGrants_ = std::make_unique<UserGestureGrantStore>();
    if (policy_.clipboard.has_value()) {
        gestureSession_ = gestureGrants_->openSession(appIdentity_);
        if (!gestureSession_.has_value()) {
            if (errorCode != nullptr) {
                *errorCode = QStringLiteral("host.capability.gesture_session_unavailable");
            }
            delete lane;
            delete thread;
            return false;
        }
        inputObserver_ = TrustedWorkerInputObserver::create(
            hostWindowId_, workerWindowId_, workerProcessId_);
        if (inputObserver_ == nullptr) {
            if (errorCode != nullptr) {
                *errorCode = QStringLiteral(
                    "host.capability.input_observer_unavailable");
            }
            delete lane;
            delete thread;
            return false;
        }
        clipboard_ = std::make_unique<ClipboardBroker>(
            *policy_.clipboard, *clipboardBackend_, *gestureGrants_);
    }
    if (policy_.file.has_value()) {
        file_ = std::make_unique<FileBroker>(*policy_.file, *fileBackend_);
    }
    guiBroker_ = std::make_unique<CapabilityBroker>(
        policy_, CapabilityServices{nullptr, nullptr, clipboard_.get(), file_.get()});
    if (!lane->moveToThread(thread)) {
        delete lane;
        delete thread;
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral("host.capability.worker_unavailable");
        }
        return false;
    }
    deliveryState_ = std::make_shared<CapabilityDeliveryState>();
    deliveryTimer_ = new QTimer(this);
    deliveryTimer_->setInterval(5);
    connect(deliveryTimer_, &QTimer::timeout, this, [this] {
        std::deque<CapabilityDelivery> ready;
        {
            std::lock_guard lock(deliveryState_->mutex);
            ready.swap(deliveryState_->pending);
        }
        if (!accepting_) return;
        for (const CapabilityDelivery &delivery : ready) {
            emit completed(delivery.generation, delivery.requestId,
                           delivery.result);
        }
    });
    deliveryTimer_->start();
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
    if (!accepting_) return;
    const HostRequestContext context{appIdentity_, requestId, nullptr};
    if (capability == QStringLiteral("network")
        || capability == QStringLiteral("storage")) {
        CapabilityWorkerLane *const lane = workerLane_;
        const std::shared_ptr<CapabilityDeliveryState> delivery = deliveryState_;
        if (lane == nullptr || !QMetaObject::invokeMethod(
                lane,
                [lane, delivery, generation, requestId, capability, operation,
                 payload, context] {
                    const BrokerResult result = lane->dispatch(
                        capability, operation, payload, context);
                    if (!delivery->accepting.load(std::memory_order_acquire)) return;
                    std::lock_guard lock(delivery->mutex);
                    if (delivery->accepting.load(std::memory_order_relaxed)
                        && delivery->pending.empty()) {
                        delivery->pending.push_back(
                            CapabilityDelivery{generation, requestId, result});
                    }
                }, Qt::QueuedConnection)) {
            emit completed(generation, requestId,
                           BrokerResult::failure(
                               QStringLiteral("capability.unavailable"),
                               QStringLiteral("Capability service is unavailable.")));
        }
        return;
    }
    if (capability == QStringLiteral("clipboard")
        && operation == QStringLiteral("read")
        && gestureSession_.has_value()) {
        const WorkerGestureEvidence evidence = queryWorkerGestureEvidence(
            hostWindowId_, workerWindowId_, workerProcessId_,
            inputObserver_ != nullptr ? inputObserver_->lastTrustedInput() : 0,
            lastGrantedInputTick_);
        if (isTrustedWorkerGesture(evidence)) {
            auto grant = gestureGrants_->issue(*gestureSession_, requestId,
                                               issuedGestureLifetimeMs);
            if (grant.has_value()) {
                lastGrantedInputTick_ = evidence.trustedWorkerInput;
                const HostRequestContext gestureContext{
                    appIdentity_, requestId, &*grant};
                emit completed(generation, requestId,
                               guiBroker_->dispatch(capability, operation,
                                                    payload, gestureContext));
                return;
            }
        }
    }
    emit completed(generation, requestId,
                   guiBroker_->dispatch(capability, operation, payload, context));
}

void HostCapabilityRuntime::invalidate() noexcept
{
    accepting_ = false;
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
