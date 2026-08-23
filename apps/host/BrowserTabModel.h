#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

enum class BrowserTabKind
{
    Host,
    App,
    Web,
    TrustedError,
};

enum class BrowserTabLifecycle
{
    Dormant,
    Starting,
    Loading,
    Active,
    Background,
    TrustedError,
    Closing,
    Retired,
};

struct BrowserTabSnapshot final
{
    QString id;
    BrowserTabKind kind = BrowserTabKind::Host;
    QString title;
    QString address;
    QStringList history;
    int historyIndex = -1;

    friend bool operator==(const BrowserTabSnapshot &,
                           const BrowserTabSnapshot &) = default;
};

enum class BrowserContentIdentity
{
    QBrowser,
    SignedApplication,
    RestrictedWeb,
};

enum class BrowserVisualState
{
    Normal,
    Recovering,
    Crashed,
    TrustedError,
};

struct BrowserTabPresentation final
{
    bool loading = false;
    int progress = 0;
    BrowserContentIdentity contentIdentity = BrowserContentIdentity::QBrowser;
    BrowserVisualState visualState = BrowserVisualState::Normal;

    friend bool operator==(const BrowserTabPresentation &,
                           const BrowserTabPresentation &) = default;
};

struct BrowserTabAccessiblePresentation final
{
    QString name;
    QString description;

    friend bool operator==(const BrowserTabAccessiblePresentation &,
                           const BrowserTabAccessiblePresentation &) = default;
};

class BrowserTabModel final : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxOpenTabs = 16;
    static constexpr int MaxRecentlyClosed = 16;
    static constexpr int MaxHistoryEntries = 256;
    static constexpr int MaxTitleCodeUnits = 256;

    explicit BrowserTabModel(QObject *parent = nullptr);

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;
    [[nodiscard]] QString activeId() const;
    [[nodiscard]] int indexOfId(const QString &id) const noexcept;

    [[nodiscard]] const BrowserTabSnapshot &snapshotAt(int index) const;
    [[nodiscard]] QVector<BrowserTabSnapshot> snapshots() const;
    [[nodiscard]] BrowserTabLifecycle lifecycleAt(int index) const;
    [[nodiscard]] BrowserTabPresentation presentationAt(int index) const;
    [[nodiscard]] BrowserTabAccessiblePresentation accessiblePresentationAt(
        int index) const;

    [[nodiscard]] QString createTab(BrowserTabKind kind,
                                    const QString &title,
                                    const QString &canonicalAddress,
                                    bool makeActive = true);
    [[nodiscard]] bool activateTab(int index);
    [[nodiscard]] bool moveTab(int from, int to);
    [[nodiscard]] bool closeTab(int index);
    [[nodiscard]] QString reopenMostRecentlyClosed(bool makeActive = true);
    [[nodiscard]] int recentlyClosedCount() const noexcept;

    [[nodiscard]] bool navigateTab(const QString &id,
                                   BrowserTabKind validatedKind,
                                   const QString &canonicalAddress);
    [[nodiscard]] bool canGoBack(const QString &id) const noexcept;
    [[nodiscard]] bool canGoForward(const QString &id) const noexcept;
    [[nodiscard]] bool goBack(const QString &id,
                              BrowserTabKind validatedTargetKind);
    [[nodiscard]] bool goForward(const QString &id,
                                 BrowserTabKind validatedTargetKind);

    [[nodiscard]] bool setTitle(const QString &id, const QString &untrustedTitle);
    [[nodiscard]] bool setLifecycle(const QString &id,
                                    BrowserTabLifecycle lifecycle);
    [[nodiscard]] bool setLoadState(const QString &id,
                                    bool loading,
                                    int progress);
    [[nodiscard]] bool setVisualState(const QString &id,
                                      BrowserVisualState visualState);

    [[nodiscard]] bool replaceFromValidatedSnapshot(
        const QVector<BrowserTabSnapshot> &snapshots,
        int activeIndex);

signals:
    void tabInserted(int index, const QString &id);
    void tabRemoved(int index, const QString &id);
    void tabMoved(int from, int to);
    void tabChanged(int index);
    void activeTabChanged(int oldIndex, int newIndex);
    void persistenceNeeded();

private:
    struct TabState final
    {
        BrowserTabSnapshot snapshot;
        BrowserTabLifecycle lifecycle = BrowserTabLifecycle::Dormant;
        bool loading = false;
        int progress = 0;
        BrowserVisualState visualState = BrowserVisualState::Normal;
    };

    [[nodiscard]] bool isValidIndex(int index) const noexcept;
    [[nodiscard]] QString generateId() const;
    [[nodiscard]] static bool isValidId(const QString &id) noexcept;
    [[nodiscard]] static bool isValidKind(BrowserTabKind kind) noexcept;
    [[nodiscard]] static bool isValidLifecycle(
        BrowserTabLifecycle lifecycle) noexcept;
    [[nodiscard]] static bool isValidVisualState(
        BrowserVisualState visualState) noexcept;
    [[nodiscard]] static std::optional<QString> sanitizedTitle(
        const QString &title);
    [[nodiscard]] static BrowserContentIdentity contentIdentityFor(
        BrowserTabKind kind) noexcept;
    [[nodiscard]] static BrowserVisualState defaultVisualStateFor(
        BrowserTabKind kind) noexcept;
    static void applyValidatedKind(TabState &tab,
                                   BrowserTabKind validatedKind) noexcept;
    [[nodiscard]] static TabState dormantState(BrowserTabSnapshot snapshot);
    [[nodiscard]] static bool isValidRestoredSnapshot(
        const BrowserTabSnapshot &snapshot);

    QVector<TabState> tabs_;
    QVector<BrowserTabSnapshot> recentlyClosed_;
    int activeIndex_ = -1;
};
