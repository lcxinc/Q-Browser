#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>

class UserGestureGrantStore final
{
public:
    UserGestureGrantStore();

    [[nodiscard]] bool issue(const QString &sessionNonce,
                             const QString &appIdentity,
                             const QString &requestId,
                             int lifetimeMs);
    [[nodiscard]] bool consume(const QString &sessionNonce,
                               const QString &appIdentity,
                               const QString &requestId);
    void invalidateSession(const QString &sessionNonce);

private:
    [[nodiscard]] static QString key(const QString &appIdentity,
                                     const QString &requestId);
    [[nodiscard]] static bool validToken(const QString &token);
    struct SessionState final
    {
        QHash<QString, qint64> grants;
        QHash<QString, qint64> used;
    };

    static void purgeExpired(SessionState &session, qint64 now);

    QElapsedTimer clock_;
    QMutex mutex_;
    QHash<QString, SessionState> sessions_;
};
