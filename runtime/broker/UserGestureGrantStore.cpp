#include "UserGestureGrantStore.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <utility>

namespace {
constexpr qsizetype maximumActiveGrants = 4096;
constexpr qsizetype maximumRememberedRecords = 8192;
constexpr qsizetype maximumSessions = 128;
constexpr qint64 replayWindowMs = 60000;

struct GrantRecord final
{
    QString requestId;
    qint64 expiresAt = 0;
};

struct SessionState final
{
    QString appIdentity;
    QHash<quint64, GrantRecord> active;
    QHash<QString, qint64> used;
};

bool validToken(const QString &token)
{
    if (token.isEmpty() || token.size() > 256) {
        return false;
    }
    for (const QChar character : token) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}
}

struct UserGestureSharedState final
{
    UserGestureSharedState() { clock.start(); }

    QElapsedTimer clock;
    QMutex mutex;
    QHash<quint64, SessionState> sessions;
    quint64 nextSessionId = 1;
    quint64 nextGrantId = 1;
    bool accepting = true;
};

namespace {
void purgeExpired(UserGestureSharedState &state, const qint64 now)
{
    for (auto session = state.sessions.begin(); session != state.sessions.end(); ++session) {
        for (auto grant = session->active.begin(); grant != session->active.end();) {
            if (grant->expiresAt <= now) {
                session->used.insert(grant->requestId, now + replayWindowMs);
                grant = session->active.erase(grant);
            } else {
                ++grant;
            }
        }
        for (auto used = session->used.begin(); used != session->used.end();) {
            if (used.value() <= now) {
                used = session->used.erase(used);
            } else {
                ++used;
            }
        }
    }
}

qsizetype recordCount(const UserGestureSharedState &state)
{
    qsizetype count = 0;
    for (auto session = state.sessions.cbegin(); session != state.sessions.cend(); ++session) {
        count += session->active.size() + session->used.size();
    }
    return count;
}

qsizetype activeCount(const UserGestureSharedState &state)
{
    qsizetype count = 0;
    for (auto session = state.sessions.cbegin(); session != state.sessions.cend(); ++session) {
        count += session->active.size();
    }
    return count;
}
}

UserGestureSession::UserGestureSession(std::shared_ptr<UserGestureSharedState> state,
                                       const quint64 sessionId)
    : state_(std::move(state)), sessionId_(sessionId)
{
}

UserGestureSession::~UserGestureSession()
{
    close();
}

UserGestureSession::UserGestureSession(UserGestureSession &&other) noexcept
    : state_(std::move(other.state_)), sessionId_(std::exchange(other.sessionId_, 0))
{
}

UserGestureSession &UserGestureSession::operator=(UserGestureSession &&other) noexcept
{
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        sessionId_ = std::exchange(other.sessionId_, 0);
    }
    return *this;
}

void UserGestureSession::close() noexcept
{
    if (state_ != nullptr && sessionId_ != 0) {
        QMutexLocker lock(&state_->mutex);
        state_->sessions.remove(sessionId_);
    }
    sessionId_ = 0;
    state_.reset();
}

UserGestureGrant::UserGestureGrant(std::shared_ptr<UserGestureSharedState> state,
                                   const quint64 sessionId,
                                   const quint64 grantId,
                                   QString appIdentity,
                                   QString requestId)
    : state_(std::move(state)), sessionId_(sessionId), grantId_(grantId),
      appIdentity_(std::move(appIdentity)), requestId_(std::move(requestId)), active_(true)
{
}

UserGestureGrant::~UserGestureGrant()
{
    revoke();
}

UserGestureGrant::UserGestureGrant(UserGestureGrant &&other) noexcept
    : state_(std::move(other.state_)), sessionId_(std::exchange(other.sessionId_, 0)),
      grantId_(std::exchange(other.grantId_, 0)), appIdentity_(std::move(other.appIdentity_)),
      requestId_(std::move(other.requestId_)), active_(std::exchange(other.active_, false))
{
}

UserGestureGrant &UserGestureGrant::operator=(UserGestureGrant &&other) noexcept
{
    if (this != &other) {
        revoke();
        state_ = std::move(other.state_);
        sessionId_ = std::exchange(other.sessionId_, 0);
        grantId_ = std::exchange(other.grantId_, 0);
        appIdentity_ = std::move(other.appIdentity_);
        requestId_ = std::move(other.requestId_);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

void UserGestureGrant::revoke() noexcept
{
    if (active_ && state_ != nullptr && sessionId_ != 0 && grantId_ != 0) {
        QMutexLocker lock(&state_->mutex);
        auto session = state_->sessions.find(sessionId_);
        if (session != state_->sessions.end() && session->active.remove(grantId_)) {
            session->used.insert(requestId_, state_->clock.elapsed() + replayWindowMs);
        }
    }
    active_ = false;
    state_.reset();
}

UserGestureGrantStore::UserGestureGrantStore()
    : state_(std::make_shared<UserGestureSharedState>())
{
}

UserGestureGrantStore::~UserGestureGrantStore()
{
    QMutexLocker lock(&state_->mutex);
    state_->accepting = false;
    state_->sessions.clear();
}

std::optional<UserGestureSession>
UserGestureGrantStore::openSession(const QString &appIdentity)
{
    if (!validToken(appIdentity)) {
        return std::nullopt;
    }
    QMutexLocker lock(&state_->mutex);
    if (!state_->accepting || state_->sessions.size() >= maximumSessions) {
        return std::nullopt;
    }
    const quint64 id = state_->nextSessionId++;
    state_->sessions.insert(id, SessionState{appIdentity, {}, {}});
    return UserGestureSession(state_, id);
}

std::optional<UserGestureGrant>
UserGestureGrantStore::issue(UserGestureSession &session,
                             const QString &requestId,
                             const int lifetimeMs)
{
    if (session.state_ != state_ || session.sessionId_ == 0 || !validToken(requestId)
        || lifetimeMs <= 0 || lifetimeMs > 60000) {
        return std::nullopt;
    }
    QMutexLocker lock(&state_->mutex);
    return issueLocked(session, requestId, lifetimeMs);
}

std::optional<UserGestureGrant>
UserGestureGrantStore::tryIssue(UserGestureSession &session,
                                const QString &requestId,
                                const int lifetimeMs)
{
    if (session.state_ != state_ || session.sessionId_ == 0 || !validToken(requestId)
        || lifetimeMs <= 0 || lifetimeMs > 60000 || !state_->mutex.tryLock()) {
        return std::nullopt;
    }
    const auto unlock = qScopeGuard([this] { state_->mutex.unlock(); });
    return issueLocked(session, requestId, lifetimeMs);
}

std::optional<UserGestureGrant>
UserGestureGrantStore::issueLocked(UserGestureSession &session,
                                   const QString &requestId,
                                   const int lifetimeMs)
{
    const qint64 now = state_->clock.elapsed();
    purgeExpired(*state_, now);
    auto found = state_->sessions.find(session.sessionId_);
    if (!state_->accepting || found == state_->sessions.end()
        || activeCount(*state_) >= maximumActiveGrants
        || recordCount(*state_) >= maximumRememberedRecords) {
        return std::nullopt;
    }
    for (auto grant = found->active.cbegin(); grant != found->active.cend(); ++grant) {
        if (grant->requestId == requestId) {
            return std::nullopt;
        }
    }
    if (found->used.contains(requestId)) {
        return std::nullopt;
    }
    const quint64 grantId = state_->nextGrantId++;
    found->active.insert(grantId, GrantRecord{requestId, now + lifetimeMs});
    return UserGestureGrant(state_, session.sessionId_, grantId, found->appIdentity, requestId);
}

bool UserGestureGrantStore::consume(UserGestureGrant &grant,
                                    const QString &appIdentity,
                                    const QString &requestId)
{
    if (grant.state_ != state_ || !grant.active_ || grant.appIdentity_ != appIdentity
        || grant.requestId_ != requestId) {
        return false;
    }
    QMutexLocker lock(&state_->mutex);
    const qint64 now = state_->clock.elapsed();
    purgeExpired(*state_, now);
    auto session = state_->sessions.find(grant.sessionId_);
    if (!state_->accepting || session == state_->sessions.end()
        || session->appIdentity != appIdentity) {
        return false;
    }
    const auto record = session->active.constFind(grant.grantId_);
    if (record == session->active.cend() || record->requestId != requestId
        || record->expiresAt <= now) {
        return false;
    }
    session->active.remove(grant.grantId_);
    session->used.insert(requestId, now + replayWindowMs);
    grant.active_ = false;
    return true;
}

bool UserGestureGrantStore::revokeOutstanding(
    UserGestureSession &session) noexcept
{
    if (session.state_ != state_ || session.sessionId_ == 0) return false;
    QMutexLocker lock(&state_->mutex);
    auto found = state_->sessions.find(session.sessionId_);
    if (found == state_->sessions.end()) return false;
    const qint64 now = state_->clock.elapsed();
    purgeExpired(*state_, now);
    for (auto grant = found->active.cbegin(); grant != found->active.cend(); ++grant) {
        found->used.insert(grant->requestId, now + replayWindowMs);
    }
    found->active.clear();
    return true;
}
