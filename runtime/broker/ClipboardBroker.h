#pragma once

#include "CapabilityBroker.h"

class UserGestureGrantStore;

enum class ClipboardStatus
{
    Success,
    Unavailable,
    TooLarge
};

struct ClipboardReadResult final
{
    ClipboardStatus status = ClipboardStatus::Unavailable;
    QString text;

    [[nodiscard]] static ClipboardReadResult error(ClipboardStatus status);
};

[[nodiscard]] constexpr qint64 maximumClipboardBytes()
{
    return 64 * 1024;
}

class ClipboardBackend
{
public:
    virtual ~ClipboardBackend() = default;
    [[nodiscard]] virtual ClipboardReadResult readText(qint64 maximumBytes) = 0;
    virtual ClipboardStatus writeText(const QString &text, qint64 maximumBytes) = 0;
};

class QtClipboardBackend final : public ClipboardBackend
{
public:
    [[nodiscard]] ClipboardReadResult readText(qint64 maximumBytes) override;
    ClipboardStatus writeText(const QString &text, qint64 maximumBytes) override;
};

class ClipboardBroker final : public CapabilityService
{
public:
    ClipboardBroker(EffectiveClipboardPolicy policy,
                    ClipboardBackend &backend,
                    UserGestureGrantStore &grants);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveClipboardPolicy policy_;
    ClipboardBackend &backend_;
    UserGestureGrantStore &grants_;
};
