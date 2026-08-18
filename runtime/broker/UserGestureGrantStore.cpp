#include "UserGestureGrantStore.h"

#include <QMutexLocker>

namespace {
constexpr qsizetype maximumRememberedGrants = 4096;
constexpr qsizetype maximumRememberedPerSession = 8192;
constexpr qsizetype maximumSessions = 128;
constexpr qint64 replayWindowMs = 60000;
}

UserGestureGrantStore::UserGestureGrantStore()
{
    clock_.start();
}

bool UserGestureGrantStore::issue(const QString &sessionNonce,
                                  const QString &appIdentity,
                                  const QString &requestId,
                                  const int lifetimeMs)
{
    if (!validToken(sessionNonce) || !validToken(appIdentity) || !validToken(requestId)
        || lifetimeMs <= 0
        || lifetimeMs > 60000) {
        return false;
    }
    QMutexLocker lock(&mutex_);
    const qint64 now = clock_.elapsed();
    auto sessionIterator = sessions_.find(sessionNonce);
    if (sessionIterator == sessions_.end()) {
        if (sessions_.size() >= maximumSessions) {
            return false;
        }
        sessionIterator = sessions_.insert(sessionNonce, SessionState{});
    }
    SessionState &session = sessionIterator.value();
    purgeExpired(session, now);
    const QString grantKey = key(appIdentity, requestId);
    if (session.grants.contains(grantKey) || session.used.contains(grantKey)
        || session.grants.size() >= maximumRememberedGrants
        || session.grants.size() + session.used.size() >= maximumRememberedPerSession) {
        return false;
    }
    session.grants.insert(grantKey, now + lifetimeMs);
    return true;
}

bool UserGestureGrantStore::consume(const QString &sessionNonce,
                                    const QString &appIdentity,
                                    const QString &requestId)
{
    if (!validToken(sessionNonce) || !validToken(appIdentity) || !validToken(requestId)) {
        return false;
    }
    QMutexLocker lock(&mutex_);
    auto sessionIterator = sessions_.find(sessionNonce);
    if (sessionIterator == sessions_.end()) {
        return false;
    }
    const qint64 now = clock_.elapsed();
    SessionState &session = sessionIterator.value();
    purgeExpired(session, now);
    const QString grantKey = key(appIdentity, requestId);
    const auto grant = session.grants.constFind(grantKey);
    if (grant == session.grants.cend() || *grant <= now) {
        return false;
    }
    session.grants.remove(grantKey);
    session.used.insert(grantKey, now + replayWindowMs);
    return true;
}

void UserGestureGrantStore::invalidateSession(const QString &sessionNonce)
{
    QMutexLocker lock(&mutex_);
    sessions_.remove(sessionNonce);
}

QString UserGestureGrantStore::key(const QString &appIdentity, const QString &requestId)
{
    return appIdentity + QChar::Null + requestId;
}

bool UserGestureGrantStore::validToken(const QString &token)
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

void UserGestureGrantStore::purgeExpired(SessionState &session, const qint64 now)
{
    for (auto iterator = session.grants.begin(); iterator != session.grants.end();) {
        if (iterator.value() <= now) {
            session.used.insert(iterator.key(), now + replayWindowMs);
            iterator = session.grants.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = session.used.begin(); iterator != session.used.end();) {
        if (iterator.value() <= now) {
            iterator = session.used.erase(iterator);
        } else {
            ++iterator;
        }
    }
}
