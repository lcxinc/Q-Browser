#include "UserGestureGrantStore.h"

#include <QMutexLocker>

namespace {
constexpr qsizetype maximumRememberedGrants = 4096;
constexpr qint64 replayWindowMs = 60000;
}

UserGestureGrantStore::UserGestureGrantStore()
{
    clock_.start();
}

bool UserGestureGrantStore::issue(const QString &appIdentity,
                                  const QString &requestId,
                                  const int lifetimeMs)
{
    if (!validToken(appIdentity) || !validToken(requestId) || lifetimeMs <= 0
        || lifetimeMs > 60000) {
        return false;
    }
    QMutexLocker lock(&mutex_);
    const qint64 now = clock_.elapsed();
    purgeExpired(now);
    const QString grantKey = key(appIdentity, requestId);
    if (grants_.contains(grantKey) || used_.contains(grantKey)
        || grants_.size() >= maximumRememberedGrants) {
        return false;
    }
    grants_.insert(grantKey, now + lifetimeMs);
    return true;
}

bool UserGestureGrantStore::consume(const QString &appIdentity, const QString &requestId)
{
    if (!validToken(appIdentity) || !validToken(requestId)) {
        return false;
    }
    QMutexLocker lock(&mutex_);
    const qint64 now = clock_.elapsed();
    purgeExpired(now);
    const QString grantKey = key(appIdentity, requestId);
    const auto grant = grants_.constFind(grantKey);
    if (grant == grants_.cend() || *grant <= now) {
        return false;
    }
    grants_.remove(grantKey);
    used_.insert(grantKey, now + replayWindowMs);
    return true;
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

void UserGestureGrantStore::purgeExpired(const qint64 now)
{
    for (auto iterator = grants_.begin(); iterator != grants_.end();) {
        if (iterator.value() <= now) {
            used_.insert(iterator.key(), now + replayWindowMs);
            iterator = grants_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = used_.begin(); iterator != used_.end();) {
        if (iterator.value() <= now) {
            iterator = used_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}
