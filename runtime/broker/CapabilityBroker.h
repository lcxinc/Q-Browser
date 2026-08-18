#pragma once

#include "EffectivePolicy.h"

#include <QJsonObject>
#include <QString>

class UserGestureGrant;

struct HostRequestContext {
    QString appIdentity;
    QString requestId;
    UserGestureGrant *userGestureGrant = nullptr;
};

[[nodiscard]] constexpr qint64 maximumIpcBinaryResultBytes() noexcept
{
    // Base64 expands by 4/3; the remaining budget covers a maximum request ID,
    // response envelope, metadata, and worst-case JSON escaping.
    return 760LL * 1024LL;
}

struct BrokerResult {
    bool ok = false;
    QJsonObject value;
    QString errorCode;
    QString errorMessage;

    [[nodiscard]] static BrokerResult success(QJsonObject value = {});
    [[nodiscard]] static BrokerResult failure(const QString &code,
                                              const QString &safeMessage);
};

class CapabilityService
{
public:
    virtual ~CapabilityService() = default;
    [[nodiscard]] virtual BrokerResult invoke(const QString &operation,
                                              const QJsonObject &payload,
                                              const HostRequestContext &context) = 0;
};

struct CapabilityServices {
    CapabilityService *network = nullptr;
    CapabilityService *storage = nullptr;
    CapabilityService *clipboard = nullptr;
    CapabilityService *file = nullptr;
};

class CapabilityBroker final
{
public:
    CapabilityBroker(EffectivePolicy policy, CapabilityServices services);

    [[nodiscard]] BrokerResult dispatch(const QString &capability,
                                        const QString &operation,
                                        const QJsonObject &payload,
                                        const HostRequestContext &context);

private:
    EffectivePolicy policy_;
    CapabilityServices services_;
};
