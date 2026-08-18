#pragma once

#include "CapabilityBroker.h"

class StorageBroker final : public CapabilityService
{
public:
    StorageBroker(EffectiveStoragePolicy policy, QString rootDirectory);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveStoragePolicy policy_;
    QString rootDirectory_;
};
