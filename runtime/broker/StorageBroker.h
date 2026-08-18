#pragma once

#include "CapabilityBroker.h"

#include <memory>

#ifdef Q_OS_WIN
namespace qbrowser_archive_detail {
class WindowsStableDirectoryTree;
}
#endif

class StorageBroker final : public CapabilityService
{
public:
    ~StorageBroker() override;

    StorageBroker(const StorageBroker &) = delete;
    StorageBroker &operator=(const StorageBroker &) = delete;

    [[nodiscard]] static std::unique_ptr<StorageBroker>
    create(EffectiveStoragePolicy policy,
           const QString &rootDirectory,
           QString *errorCode = nullptr);

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    StorageBroker(EffectiveStoragePolicy policy, QString rootDirectory);
    [[nodiscard]] bool rootIsStable() const;

    EffectiveStoragePolicy policy_;
    QString rootDirectory_;
#ifdef Q_OS_WIN
    std::unique_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> stableRoot_;
#endif
};
