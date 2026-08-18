#pragma once

#include "CapabilityBroker.h"

class NetworkBroker final : public CapabilityService
{
public:
    explicit NetworkBroker(EffectiveNetworkPolicy policy);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveNetworkPolicy policy_;
};
