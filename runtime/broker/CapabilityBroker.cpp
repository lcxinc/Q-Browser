#include "CapabilityBroker.h"

#include <utility>

namespace {

BrokerResult denied()
{
    return BrokerResult::failure(QStringLiteral("capability.denied"),
                                 QStringLiteral("Capability is not permitted."));
}

bool validHostIdentity(const QString &identity)
{
    if (identity.isEmpty() || identity.size() > 256) {
        return false;
    }
    for (const QChar character : identity) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}

bool operationAllowed(const QString &capability, const QString &operation)
{
    if (capability == QStringLiteral("network")) {
        return operation == QStringLiteral("request");
    }
    if (capability == QStringLiteral("storage")) {
        return operation == QStringLiteral("get") || operation == QStringLiteral("set")
            || operation == QStringLiteral("remove");
    }
    if (capability == QStringLiteral("clipboard")) {
        return operation == QStringLiteral("read") || operation == QStringLiteral("write");
    }
    if (capability == QStringLiteral("file")) {
        return operation == QStringLiteral("open");
    }
    return false;
}

} // namespace

BrokerResult BrokerResult::success(QJsonObject value)
{
    return {true, std::move(value), {}, {}};
}

BrokerResult BrokerResult::failure(const QString &code, const QString &safeMessage)
{
    return {false, {}, code, safeMessage};
}

CapabilityBroker::CapabilityBroker(EffectivePolicy policy, CapabilityServices services)
    : policy_(std::move(policy)), services_(services)
{
}

BrokerResult CapabilityBroker::dispatch(const QString &capability,
                                        const QString &operation,
                                        const QJsonObject &payload,
                                        const HostRequestContext &context)
{
    if (!validHostIdentity(context.appIdentity)) {
        return denied();
    }

    CapabilityService *service = nullptr;
    if (capability == QStringLiteral("network") && policy_.network.has_value()) {
        service = services_.network;
    } else if (capability == QStringLiteral("storage") && policy_.storage.has_value()) {
        service = services_.storage;
    } else if (capability == QStringLiteral("clipboard") && policy_.clipboard.has_value()) {
        service = services_.clipboard;
    } else if (capability == QStringLiteral("file") && policy_.file.has_value()) {
        service = services_.file;
    }
    if (service == nullptr || !operationAllowed(capability, operation)) {
        return denied();
    }
    return service->invoke(operation, payload, context);
}
