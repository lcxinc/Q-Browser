#pragma once

#include "CapabilityBroker.h"

class ClipboardBackend
{
public:
    virtual ~ClipboardBackend() = default;
    [[nodiscard]] virtual QString readText() = 0;
    virtual bool writeText(const QString &text) = 0;
};

class QtClipboardBackend final : public ClipboardBackend
{
public:
    [[nodiscard]] QString readText() override;
    bool writeText(const QString &text) override;
};

class ClipboardBroker final : public CapabilityService
{
public:
    ClipboardBroker(EffectiveClipboardPolicy policy, ClipboardBackend &backend);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveClipboardPolicy policy_;
    ClipboardBackend &backend_;
};
