#pragma once

#include <QString>

#include <memory>
#include <optional>

struct UserGestureSharedState;
class UserGestureGrantStore;

class UserGestureSession final
{
public:
    ~UserGestureSession();
    UserGestureSession(const UserGestureSession &) = delete;
    UserGestureSession &operator=(const UserGestureSession &) = delete;
    UserGestureSession(UserGestureSession &&other) noexcept;
    UserGestureSession &operator=(UserGestureSession &&other) noexcept;

private:
    friend class UserGestureGrantStore;
    UserGestureSession(std::shared_ptr<UserGestureSharedState> state, quint64 sessionId);
    void close() noexcept;

    std::shared_ptr<UserGestureSharedState> state_;
    quint64 sessionId_ = 0;
};

class UserGestureGrant final
{
public:
    ~UserGestureGrant();
    UserGestureGrant(const UserGestureGrant &) = delete;
    UserGestureGrant &operator=(const UserGestureGrant &) = delete;
    UserGestureGrant(UserGestureGrant &&other) noexcept;
    UserGestureGrant &operator=(UserGestureGrant &&other) noexcept;

private:
    friend class UserGestureGrantStore;
    UserGestureGrant(std::shared_ptr<UserGestureSharedState> state,
                     quint64 sessionId,
                     quint64 grantId,
                     QString appIdentity,
                     QString requestId);
    void revoke() noexcept;

    std::shared_ptr<UserGestureSharedState> state_;
    quint64 sessionId_ = 0;
    quint64 grantId_ = 0;
    QString appIdentity_;
    QString requestId_;
    bool active_ = false;
};

class UserGestureGrantStore final
{
public:
    UserGestureGrantStore();
    ~UserGestureGrantStore();
    UserGestureGrantStore(const UserGestureGrantStore &) = delete;
    UserGestureGrantStore &operator=(const UserGestureGrantStore &) = delete;

    [[nodiscard]] std::optional<UserGestureSession>
    openSession(const QString &appIdentity);
    [[nodiscard]] std::optional<UserGestureGrant>
    issue(UserGestureSession &session, const QString &requestId, int lifetimeMs);
    [[nodiscard]] bool consume(UserGestureGrant &grant,
                               const QString &appIdentity,
                               const QString &requestId);

private:
    std::shared_ptr<UserGestureSharedState> state_;
};
