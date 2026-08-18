#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>

class UserGestureGrantStore final
{
public:
    UserGestureGrantStore();

    [[nodiscard]] bool issue(const QString &appIdentity,
                             const QString &requestId,
                             int lifetimeMs);
    [[nodiscard]] bool consume(const QString &appIdentity, const QString &requestId);

private:
    [[nodiscard]] static QString key(const QString &appIdentity,
                                     const QString &requestId);
    [[nodiscard]] static bool validToken(const QString &token);
    void purgeExpired(qint64 now);

    QElapsedTimer clock_;
    QMutex mutex_;
    QHash<QString, qint64> grants_;
    QSet<QString> used_;
};
