#pragma once

#include "CapabilityBroker.h"

#include <QHostAddress>
#include <QList>

#include <memory>

class NetworkAddressResolver
{
public:
    virtual ~NetworkAddressResolver() = default;
    [[nodiscard]] virtual QList<QHostAddress> resolve(const QString &host,
                                                      int timeoutMs,
                                                      bool &timedOut) = 0;
};

class NetworkBroker final : public CapabilityService
{
public:
    explicit NetworkBroker(EffectiveNetworkPolicy policy);
    NetworkBroker(EffectiveNetworkPolicy policy, NetworkAddressResolver &resolver);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveNetworkPolicy policy_;
    std::unique_ptr<NetworkAddressResolver> ownedResolver_;
    NetworkAddressResolver *resolver_ = nullptr;
};
