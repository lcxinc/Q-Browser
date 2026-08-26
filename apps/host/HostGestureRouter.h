#pragma once

#include "BrowserCommand.h"
#include "TabCapabilityAuthority.h"
#include "UserGestureGrantStore.h"

#include <QKeySequence>
#include <QObject>

#include <memory>
#include <optional>

struct HostGestureRouterState;
class HostGestureNativeObserver;

struct HostGestureSystemEvidence final
{
    quint32 now = 0;
    quintptr workerWindowId = 0;
    quint32 workerWindowProcessId = 0;
    quint32 focusProcessId = 0;
    bool foregroundMatchesHostRoot = false;
    bool focusBelongsToWorkerWindow = false;
};

class HostGestureRouter final : public QObject
{
    Q_OBJECT

public:
    explicit HostGestureRouter(quintptr hostWindowId,
                               QObject *parent = nullptr);
    ~HostGestureRouter() override;

    HostGestureRouter(const HostGestureRouter &) = delete;
    HostGestureRouter &operator=(const HostGestureRouter &) = delete;

    [[nodiscard]] bool registerBinding(
        const TabCapabilityAuthority &authority,
        std::shared_ptr<AuthorityAdmissionToken> admissionToken,
        std::shared_ptr<UserGestureGrantStore> grantStore,
        UserGestureSession gestureSession,
        QString *errorCode = nullptr);
    void unregisterBinding(const TabCapabilityAuthority &authority) noexcept;
    [[nodiscard]] bool activateBinding(
        const TabCapabilityAuthority &authority);
    void hostDeactivated() noexcept;

    [[nodiscard]] std::optional<UserGestureGrant> issueGrant(
        const TabCapabilityAuthority &authority,
        const QString &requestId,
        int lifetimeMs);

#ifdef Q_BROWSER_HOST_TESTING
    [[nodiscard]] static std::unique_ptr<HostGestureRouter>
    createForTesting(quintptr hostWindowId);
    void setSystemEvidenceForTesting(
        const HostGestureSystemEvidence &evidence) noexcept;
    [[nodiscard]] bool routeKeyboardForTesting(
        QKeyCombination combination,
        bool keyDown,
        quint32 inputTime,
        bool lowerIntegrityInjected = false) noexcept;
    [[nodiscard]] bool observeMouseForTesting(
        quintptr targetWindowId,
        quint32 targetProcessId,
        bool targetBelongsToWorkerWindow,
        quint32 inputTime,
        bool lowerIntegrityInjected = false) noexcept;
#endif

signals:
    void browserCommandRequested(BrowserCommand command);

private:
    friend class HostGestureNativeObserver;
    explicit HostGestureRouter(quintptr hostWindowId,
                               bool nativeHooksDisabledForTesting,
                               QObject *parent);
    [[nodiscard]] bool ensureNativeHooks(QString *errorCode);
    [[nodiscard]] HostGestureSystemEvidence systemEvidence(
        const TabCapabilityAuthority &authority) const noexcept;
    [[nodiscard]] bool routeKeyboard(QKeyCombination combination,
                                     bool keyDown,
                                     quint32 inputTime) noexcept;
    [[nodiscard]] bool observeMouse(quintptr targetWindowId,
                                    quint32 targetProcessId,
                                    bool targetBelongsToWorkerWindow,
                                    quint32 inputTime) noexcept;
    void clearActiveEvidence() noexcept;

    std::unique_ptr<HostGestureRouterState> state_;
};
