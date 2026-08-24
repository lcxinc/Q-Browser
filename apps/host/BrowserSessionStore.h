#pragma once

#include "BrowserAddress.h"
#include "BrowserTabModel.h"

#include <QRect>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class HostOwnedStateDirectory;

struct BrowserWindowSnapshot final
{
    QRect geometry;
    QString activeTabId;
    QVector<BrowserTabSnapshot> tabs;

    friend bool operator==(const BrowserWindowSnapshot &,
                           const BrowserWindowSnapshot &) = default;
};

enum class BrowserSessionLoadStatus
{
    Missing,
    Loaded,
    Corrupt,
    IoFailure,
};

struct BrowserSessionLoadResult final
{
    BrowserSessionLoadStatus status = BrowserSessionLoadStatus::IoFailure;
    std::optional<BrowserWindowSnapshot> snapshot;
    QString stableError;
};

enum class BrowserSessionSaveStatus
{
    Saved,
    InvalidSnapshot,
    IoFailure,
};

struct BrowserSessionSaveResult final
{
    BrowserSessionSaveStatus status = BrowserSessionSaveStatus::IoFailure;
    QString stableError;
};

struct BrowserSessionResolveResult final
{
    std::optional<BrowserWindowSnapshot> snapshot;
    QString stableError;
};

using RestoredAddressResolver =
    std::function<std::optional<BrowserTabKind>(const BrowserAddress &)>;

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
struct BrowserSessionStoreTestHooks final
{
    std::function<void()> beforeTitleSanitize;
    std::function<void()> beforeAppAuthorityScan;
    std::function<void()> beforeCurrentHistoryEquality;
    std::function<void(const QString &)> afterPrimaryLeafOpenedBeforeRead;
    std::function<void(const QString &)> afterCorruptLeafOpenedBeforeUse;
    std::function<void(const QString &)> beforeAtomicPublish;
    std::function<void(const QString &)> afterAtomicPublishBeforeVerify;
    bool failAtomicPublish = false;
};

void setBrowserSessionStoreTestHooks(BrowserSessionStoreTestHooks hooks);
void resetBrowserSessionStoreTestHooks();
[[nodiscard]] BrowserSessionStoreTestHooks browserSessionStoreTestHooks();
[[nodiscard]] bool isCollectionSizeWithinLimit(qsizetype size,
                                               qsizetype maximum) noexcept;
[[nodiscard]] bool isCollectionIndexInRange(int index,
                                            qsizetype size) noexcept;
}
#endif

class BrowserSessionStore final
{
public:
    explicit BrowserSessionStore(
        std::shared_ptr<const HostOwnedStateDirectory> stateDirectory);

    [[nodiscard]] BrowserSessionLoadResult load();
    [[nodiscard]] BrowserSessionSaveResult save(
        const BrowserWindowSnapshot &snapshot);
    [[nodiscard]] BrowserSessionResolveResult validateAndResolve(
        const BrowserWindowSnapshot &raw,
        const RestoredAddressResolver &resolver) const;

private:
    std::shared_ptr<const HostOwnedStateDirectory> stateDirectory_;
};
