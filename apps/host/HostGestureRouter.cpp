#include "HostGestureRouter.h"

#include <QCoreApplication>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <windows.h>

#include <memory>
#include <utility>

namespace {

constexpr quint32 maximumGestureAgeMs = 1'000;

bool windowBelongsToRoot(const HWND candidate, const HWND root) noexcept
{
    if (candidate == nullptr || root == nullptr) return false;
    return candidate == root || IsChild(root, candidate)
        || GetAncestor(candidate, GA_ROOT) == root;
}

bool validSystemEvidence(const HostGestureSystemEvidence &evidence,
                         const TabCapabilityAuthority &authority) noexcept
{
    return evidence.now != 0
        && evidence.workerWindowId == authority.workerWindowId
        && evidence.workerWindowProcessId == authority.workerProcessId
        && evidence.focusProcessId == authority.workerProcessId
        && evidence.foregroundMatchesHostRoot
        && evidence.focusBelongsToWorkerWindow;
}

Qt::Key qtKeyFromVirtualKey(const DWORD virtualKey) noexcept
{
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return static_cast<Qt::Key>(Qt::Key_A + virtualKey - 'A');
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return static_cast<Qt::Key>(Qt::Key_0 + virtualKey - '0');
    }
    switch (virtualKey) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Qt::Key_Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Qt::Key_Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Qt::Key_Alt;
    case VK_LWIN:
    case VK_RWIN:
        return Qt::Key_Meta;
    case VK_TAB:
        return Qt::Key_Tab;
    case VK_LEFT:
        return Qt::Key_Left;
    case VK_RIGHT:
        return Qt::Key_Right;
    case VK_F5:
        return Qt::Key_F5;
    case VK_ESCAPE:
        return Qt::Key_Escape;
    default:
        return Qt::Key_unknown;
    }
}

QKeyCombination keyCombination(const DWORD virtualKey) noexcept
{
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= Qt::ControlModifier;
    }
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= Qt::ShiftModifier;
    }
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= Qt::AltModifier;
    }
    return QKeyCombination(modifiers, qtKeyFromVirtualKey(virtualKey));
}

struct GestureBinding final
{
    GestureBinding(TabCapabilityAuthority immutableAuthority,
                   std::shared_ptr<AuthorityAdmissionToken> admission,
                   std::shared_ptr<UserGestureGrantStore> store,
                   UserGestureSession ownedSession)
        : authority(std::move(immutableAuthority)),
          token(std::move(admission)), grantStore(std::move(store)),
          gestureSession(std::move(ownedSession))
    {
    }

    const TabCapabilityAuthority authority;
    const std::shared_ptr<AuthorityAdmissionToken> token;
    const std::shared_ptr<UserGestureGrantStore> grantStore;
    UserGestureSession gestureSession;
    quint32 lastTrustedInput = 0;
    quint32 lastGrantedInput = 0;
};

bool clearEvidence(const std::shared_ptr<GestureBinding> &binding) noexcept
{
    if (binding == nullptr) return true;
    binding->lastTrustedInput = 0;
    binding->lastGrantedInput = 0;
    return binding->grantStore != nullptr
        && binding->grantStore->revokeOutstanding(binding->gestureSession);
}

} // namespace

struct HostGestureRouterState final
{
    ~HostGestureRouterState();

    quintptr hostWindowId = 0;
    bool nativeHooksDisabledForTesting = false;
    QHash<QString, std::shared_ptr<GestureBinding>> bindings;
    std::shared_ptr<GestureBinding> activeBinding;
    std::unique_ptr<HostGestureNativeObserver> nativeObserver;
    QSet<int> suppressedKeys;
#ifdef Q_BROWSER_HOST_TESTING
    std::optional<HostGestureSystemEvidence> testingEvidence;
#endif
};

class HostGestureNativeObserver final
{
public:
    HostGestureNativeObserver(const HostGestureNativeObserver &) = delete;
    HostGestureNativeObserver &operator=(const HostGestureNativeObserver &) = delete;

    ~HostGestureNativeObserver()
    {
        if (keyboardHook_ != nullptr) (void)UnhookWindowsHookEx(keyboardHook_);
        if (mouseHook_ != nullptr) (void)UnhookWindowsHookEx(mouseHook_);
        if (active_ == this) active_ = nullptr;
    }

    [[nodiscard]] static std::unique_ptr<HostGestureNativeObserver> create(
        HostGestureRouter *router)
    {
        if (router == nullptr || active_ != nullptr) return nullptr;
        auto observer = std::unique_ptr<HostGestureNativeObserver>(
            new HostGestureNativeObserver(router));
        active_ = observer.get();
        const HINSTANCE module = GetModuleHandleW(nullptr);
        observer->mouseHook_ = SetWindowsHookExW(
            WH_MOUSE_LL, &HostGestureNativeObserver::mouseHook, module, 0);
        observer->keyboardHook_ = SetWindowsHookExW(
            WH_KEYBOARD_LL, &HostGestureNativeObserver::keyboardHook, module, 0);
        if (observer->mouseHook_ == nullptr
            || observer->keyboardHook_ == nullptr) {
            return nullptr;
        }
        return observer;
    }

private:
    explicit HostGestureNativeObserver(HostGestureRouter *router)
        : router_(router)
    {
    }

    static LRESULT CALLBACK mouseHook(const int code,
                                      const WPARAM message,
                                      const LPARAM data) noexcept
    {
        if (code == HC_ACTION && active_ != nullptr && data != 0
            && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
                || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN)) {
            const auto &input = *reinterpret_cast<const MSLLHOOKSTRUCT *>(data);
            if ((input.flags & LLMHF_LOWER_IL_INJECTED) != 0) {
                return CallNextHookEx(nullptr, code, message, data);
            }
            const HWND target = WindowFromPoint(input.pt);
            DWORD targetProcessId = 0;
            (void)GetWindowThreadProcessId(target, &targetProcessId);
            const auto activeBinding = active_->router_->state_->activeBinding;
            const HWND workerWindow = activeBinding != nullptr
                ? reinterpret_cast<HWND>(activeBinding->authority.workerWindowId)
                : nullptr;
            const bool belongs = windowBelongsToRoot(target, workerWindow);
            (void)active_->router_->observeMouse(
                reinterpret_cast<quintptr>(target), targetProcessId, belongs,
                input.time);
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK keyboardHook(const int code,
                                         const WPARAM message,
                                         const LPARAM data) noexcept
    {
        if (code == HC_ACTION && active_ != nullptr && data != 0) {
            const bool keyDown = message == WM_KEYDOWN
                || message == WM_SYSKEYDOWN;
            const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (keyDown || keyUp) {
                const auto &input =
                    *reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
                if ((input.flags & LLKHF_LOWER_IL_INJECTED) != 0) {
                    return CallNextHookEx(nullptr, code, message, data);
                }
                if (active_->router_->routeKeyboard(
                        keyCombination(input.vkCode), keyDown, input.time)) {
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    // Native creation, callbacks, and destruction are all constrained to the
    // QCoreApplication GUI thread, making this a process-wide singleton.
    static HostGestureNativeObserver *active_;
    HostGestureRouter *router_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
};

HostGestureNativeObserver *HostGestureNativeObserver::active_ = nullptr;

HostGestureRouterState::~HostGestureRouterState() = default;

HostGestureRouter::HostGestureRouter(const quintptr hostWindowId,
                                     QObject *parent)
    : HostGestureRouter(hostWindowId, false, parent)
{
}

HostGestureRouter::HostGestureRouter(
    const quintptr hostWindowId,
    const bool nativeHooksDisabledForTesting,
    QObject *parent)
    : QObject(parent), state_(std::make_unique<HostGestureRouterState>())
{
    state_->hostWindowId = hostWindowId;
    state_->nativeHooksDisabledForTesting = nativeHooksDisabledForTesting;
}

HostGestureRouter::~HostGestureRouter()
{
    Q_ASSERT(QThread::currentThread() == thread());
    hostDeactivated();
    const auto bindings = state_->bindings;
    for (const std::shared_ptr<GestureBinding> &binding : bindings) {
        if (binding != nullptr && binding->token != nullptr) {
            (void)binding->token->beginRevoke();
        }
    }
    state_->bindings.clear();
    state_->nativeObserver.reset();
}

bool HostGestureRouter::registerBinding(
    const TabCapabilityAuthority &authority,
    std::shared_ptr<AuthorityAdmissionToken> admissionToken,
    std::shared_ptr<UserGestureGrantStore> grantStore,
    UserGestureSession gestureSession,
    QString *errorCode)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto fail = [errorCode](const QString &code) {
        if (errorCode != nullptr) *errorCode = code;
        return false;
    };
    if (!authority.isValid() || admissionToken == nullptr
        || grantStore == nullptr || state_->hostWindowId == 0) {
        return fail(QStringLiteral("host.gesture.invalid_binding"));
    }
    if (!ensureNativeHooks(errorCode)) return false;
    for (auto iterator = state_->bindings.cbegin();
         iterator != state_->bindings.cend(); ++iterator) {
        if (iterator.value() != nullptr
            && iterator.value()->token == admissionToken
            && (iterator.value()->authority != authority
                || iterator.key() != authority.tabId)) {
            return fail(QStringLiteral("host.gesture.token_reused"));
        }
    }
    const auto existing = state_->bindings.value(authority.tabId);
    if (existing != nullptr && existing->authority == authority
        && existing->token == admissionToken) {
        if (errorCode != nullptr) errorCode->clear();
        return true;
    }
    if (existing != nullptr) unregisterBinding(existing->authority);

    auto guard = admissionToken->tryAcquireUse();
    if (!guard.has_value()) {
        return fail(QStringLiteral("host.gesture.authority_revoked"));
    }
    auto binding = std::make_shared<GestureBinding>(
        authority, std::move(admissionToken), std::move(grantStore),
        std::move(gestureSession));
    const bool attached = guard->publishIfStillAdmitted([this, binding] {
        state_->bindings.insert(binding->authority.tabId, binding);
        return true;
    });
    if (!attached) {
        return fail(QStringLiteral("host.gesture.authority_revoked"));
    }
    if (errorCode != nullptr) errorCode->clear();
    return true;
}

void HostGestureRouter::unregisterBinding(
    const TabCapabilityAuthority &authority) noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = state_->bindings.find(authority.tabId);
    if (found == state_->bindings.end() || found.value() == nullptr
        || found.value()->authority != authority) {
        return;
    }
    const std::shared_ptr<GestureBinding> binding = found.value();
    if (state_->activeBinding == binding) {
        (void)clearEvidence(binding);
        state_->activeBinding.reset();
    } else {
        (void)clearEvidence(binding);
    }
    if (binding->token != nullptr) {
        try {
            (void)binding->token->beginRevoke();
        } catch (...) {
        }
    }
    state_->bindings.erase(found);
}

bool HostGestureRouter::activateBinding(
    const TabCapabilityAuthority &authority)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto binding = state_->bindings.value(authority.tabId);
    if (binding == nullptr || binding->authority != authority
        || binding->token == nullptr) {
        clearActiveEvidence();
        return false;
    }
    if (state_->activeBinding == binding) {
        auto guard = binding->token->tryAcquireUse();
        if (guard.has_value()
            && guard->publishIfStillAdmitted(
                [this, binding] {
                    return state_->activeBinding == binding;
                })) {
            return true;
        }
        clearActiveEvidence();
        return false;
    }

    clearActiveEvidence();
    if (!clearEvidence(binding)) return false;
    auto guard = binding->token->tryAcquireUse();
    if (!guard.has_value()) return false;
    return guard->publishIfStillAdmitted([this, binding] {
        state_->activeBinding = binding;
        return true;
    });
}

void HostGestureRouter::hostDeactivated() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    clearActiveEvidence();
}

std::optional<UserGestureGrant> HostGestureRouter::issueGrant(
    const TabCapabilityAuthority &authority,
    const QString &requestId,
    const int lifetimeMs)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const std::shared_ptr<GestureBinding> binding = state_->activeBinding;
    if (binding == nullptr || binding->authority != authority
        || binding->token == nullptr || binding->grantStore == nullptr) {
        return std::nullopt;
    }
    const HostGestureSystemEvidence evidence = systemEvidence(authority);
    if (!validSystemEvidence(evidence, authority)
        || binding->lastTrustedInput == 0
        || binding->lastTrustedInput == binding->lastGrantedInput
        || static_cast<quint32>(evidence.now - binding->lastTrustedInput)
               > maximumGestureAgeMs) {
        (void)clearEvidence(binding);
        return std::nullopt;
    }
    auto guard = binding->token->tryAcquireUse();
    if (!guard.has_value()) {
        (void)clearEvidence(binding);
        return std::nullopt;
    }
    if (state_->activeBinding != binding
        || !validSystemEvidence(systemEvidence(authority), authority)) {
        (void)clearEvidence(binding);
        return std::nullopt;
    }
    std::optional<UserGestureGrant> grant;
    const bool published = guard->publishIfStillAdmitted(
        [binding, &grant, &requestId, lifetimeMs] {
            grant = binding->grantStore->tryIssue(
                binding->gestureSession, requestId, lifetimeMs);
            if (!grant.has_value()) return false;
            binding->lastGrantedInput = binding->lastTrustedInput;
            return true;
        });
    if (!published) {
        grant.reset();
        (void)clearEvidence(binding);
    }
    return grant;
}

#ifdef Q_BROWSER_HOST_TESTING
std::unique_ptr<HostGestureRouter> HostGestureRouter::createForTesting(
    const quintptr hostWindowId)
{
    if (hostWindowId == 0) return nullptr;
    return std::unique_ptr<HostGestureRouter>(
        new HostGestureRouter(hostWindowId, true, nullptr));
}

void HostGestureRouter::setSystemEvidenceForTesting(
    const HostGestureSystemEvidence &evidence) noexcept
{
    state_->testingEvidence = evidence;
}

bool HostGestureRouter::routeKeyboardForTesting(
    const QKeyCombination combination,
    const bool keyDown,
    const quint32 inputTime,
    const bool lowerIntegrityInjected) noexcept
{
    if (lowerIntegrityInjected) return false;
    return routeKeyboard(combination, keyDown, inputTime);
}

bool HostGestureRouter::observeMouseForTesting(
    const quintptr targetWindowId,
    const quint32 targetProcessId,
    const bool targetBelongsToWorkerWindow,
    const quint32 inputTime,
    const bool lowerIntegrityInjected) noexcept
{
    if (lowerIntegrityInjected) return false;
    return observeMouse(targetWindowId, targetProcessId,
                        targetBelongsToWorkerWindow, inputTime);
}
#endif

bool HostGestureRouter::ensureNativeHooks(QString *errorCode)
{
    if (state_->nativeHooksDisabledForTesting) return true;
    if (state_->nativeObserver != nullptr) return true;
    const QCoreApplication *const application = QCoreApplication::instance();
    if (application == nullptr
        || application->thread() != QThread::currentThread()) {
        if (errorCode != nullptr) {
            *errorCode = QStringLiteral("host.gesture.input_observer_wrong_thread");
        }
        return false;
    }
    state_->nativeObserver = HostGestureNativeObserver::create(this);
    if (state_->nativeObserver != nullptr) return true;
    if (errorCode != nullptr) {
        *errorCode = QStringLiteral("host.gesture.input_observer_unavailable");
    }
    return false;
}

HostGestureSystemEvidence HostGestureRouter::systemEvidence(
    const TabCapabilityAuthority &authority) const noexcept
{
#ifdef Q_BROWSER_HOST_TESTING
    if (state_->testingEvidence.has_value()) return *state_->testingEvidence;
#endif
    HostGestureSystemEvidence evidence;
    evidence.now = GetTickCount();
    evidence.workerWindowId = authority.workerWindowId;
    const HWND workerWindow = reinterpret_cast<HWND>(authority.workerWindowId);
    if (workerWindow == nullptr || !IsWindow(workerWindow)) return evidence;
    const HWND hostRoot = GetAncestor(
        reinterpret_cast<HWND>(state_->hostWindowId), GA_ROOT);
    const HWND foreground = GetForegroundWindow();
    evidence.foregroundMatchesHostRoot = foreground != nullptr
        && hostRoot != nullptr && GetAncestor(foreground, GA_ROOT) == hostRoot;
    DWORD actualWorkerProcessId = 0;
    const DWORD workerThread = GetWindowThreadProcessId(
        workerWindow, &actualWorkerProcessId);
    evidence.workerWindowProcessId = actualWorkerProcessId;
    if (workerThread == 0) return evidence;
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(workerThread, &gui)) return evidence;
    DWORD focusProcessId = 0;
    (void)GetWindowThreadProcessId(gui.hwndFocus, &focusProcessId);
    evidence.focusProcessId = focusProcessId;
    evidence.focusBelongsToWorkerWindow = windowBelongsToRoot(
        gui.hwndFocus, workerWindow);
    return evidence;
}

bool HostGestureRouter::routeKeyboard(
    const QKeyCombination combination,
    const bool keyDown,
    const quint32 inputTime) noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    const Qt::Key observedKey = combination.key();
    const int key = static_cast<int>(observedKey);
    if (!keyDown && state_->suppressedKeys.remove(key)) return true;
    if (!keyDown) return false;
    if (state_->suppressedKeys.contains(key)) return true;
    if (observedKey == Qt::Key_Shift || observedKey == Qt::Key_Control
        || observedKey == Qt::Key_Alt || observedKey == Qt::Key_Meta) {
        return false;
    }

    const std::shared_ptr<GestureBinding> binding = state_->activeBinding;
    if (binding == nullptr) return false;
    const HostGestureSystemEvidence evidence = systemEvidence(binding->authority);
    const bool trustedFocus = validSystemEvidence(evidence, binding->authority);
    const std::optional<BrowserCommand> command =
        browserCommandForKeyCombination(combination);
    if (command.has_value() && trustedFocus) {
        state_->suppressedKeys.insert(key);
        if (binding->token == nullptr) {
            (void)clearEvidence(binding);
            return true;
        }
        auto guard = binding->token->tryAcquireUse();
        if (!guard.has_value()
            || state_->activeBinding != binding
            || !validSystemEvidence(systemEvidence(binding->authority),
                                    binding->authority)) {
            (void)clearEvidence(binding);
            return true;
        }
        const BrowserCommand queuedCommand = *command;
        const TabCapabilityAuthority queuedAuthority = binding->authority;
        if (!guard->publishIfStillAdmitted(
                [this, binding, queuedAuthority, queuedCommand] {
                    return QMetaObject::invokeMethod(
                        this,
                        [this, binding, queuedAuthority, queuedCommand] {
                            if (binding->token == nullptr) {
                                (void)clearEvidence(binding);
                                return;
                            }
                            auto queuedGuard = binding->token->tryAcquireUse();
                            if (!queuedGuard.has_value()) {
                                (void)clearEvidence(binding);
                                return;
                            }
                            const bool emitted =
                                queuedGuard->publishIfStillAdmitted(
                                    [this, binding, queuedAuthority,
                                     queuedCommand] {
                                        if (state_->activeBinding != binding
                                            || binding->authority
                                                != queuedAuthority
                                            || !validSystemEvidence(
                                                systemEvidence(queuedAuthority),
                                                queuedAuthority)) {
                                            return false;
                                        }
                                        emit browserCommandRequested(
                                            queuedCommand);
                                        return true;
                                    });
                            if (!emitted) (void)clearEvidence(binding);
                        },
                        Qt::QueuedConnection);
                })) {
            (void)clearEvidence(binding);
        }
        return true;
    }
    if (!trustedFocus || inputTime == 0 || binding->token == nullptr) {
        if (!trustedFocus) (void)clearEvidence(binding);
        return false;
    }
    auto guard = binding->token->tryAcquireUse();
    if (!guard.has_value()) {
        (void)clearEvidence(binding);
        return false;
    }
    if (state_->activeBinding != binding
        || !validSystemEvidence(systemEvidence(binding->authority),
                                binding->authority)) {
        (void)clearEvidence(binding);
        return false;
    }
    (void)guard->publishIfStillAdmitted([binding, inputTime] {
        binding->lastTrustedInput = inputTime;
        return true;
    });
    return false;
}

bool HostGestureRouter::observeMouse(
    const quintptr targetWindowId,
    const quint32 targetProcessId,
    const bool targetBelongsToWorkerWindow,
    const quint32 inputTime) noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    const std::shared_ptr<GestureBinding> binding = state_->activeBinding;
    if (binding == nullptr || binding->token == nullptr || inputTime == 0) {
        return false;
    }
    const HostGestureSystemEvidence evidence = systemEvidence(binding->authority);
    const bool trustedTarget = targetWindowId != 0
        && targetProcessId == binding->authority.workerProcessId
        && targetBelongsToWorkerWindow && evidence.foregroundMatchesHostRoot
        && evidence.workerWindowId == binding->authority.workerWindowId
        && evidence.workerWindowProcessId == binding->authority.workerProcessId;
    if (!trustedTarget) {
        (void)clearEvidence(binding);
        return false;
    }
    auto guard = binding->token->tryAcquireUse();
    if (!guard.has_value()) {
        (void)clearEvidence(binding);
        return false;
    }
    if (state_->activeBinding != binding) {
        (void)clearEvidence(binding);
        return false;
    }
    return guard->publishIfStillAdmitted([binding, inputTime] {
        binding->lastTrustedInput = inputTime;
        return true;
    });
}

void HostGestureRouter::clearActiveEvidence() noexcept
{
    const std::shared_ptr<GestureBinding> old = state_->activeBinding;
    state_->activeBinding.reset();
    (void)clearEvidence(old);
}
