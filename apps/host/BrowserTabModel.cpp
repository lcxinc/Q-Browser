#include "BrowserTabModel.h"

#include <QChar>
#include <QScopedValueRollback>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace
{
bool isBidiControl(char16_t value) noexcept
{
    return value == 0x061c || value == 0x200e || value == 0x200f
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

QString identityText(BrowserContentIdentity identity)
{
    switch (identity) {
    case BrowserContentIdentity::QBrowser:
        return QStringLiteral("Q-Browser");
    case BrowserContentIdentity::SignedApplication:
        return QStringLiteral("Signed application");
    case BrowserContentIdentity::RestrictedWeb:
        return QStringLiteral("Restricted web");
    }
    return QStringLiteral("Q-Browser");
}

QString fallbackTitle(BrowserTabKind kind)
{
    switch (kind) {
    case BrowserTabKind::Host:
        return QStringLiteral("New tab");
    case BrowserTabKind::App:
        return QStringLiteral("Application");
    case BrowserTabKind::Web:
        return QStringLiteral("Restricted web");
    case BrowserTabKind::TrustedError:
        return QStringLiteral("Error");
    }
    return QStringLiteral("New tab");
}
}

BrowserTabModel::BrowserTabModel(QObject *parent) : QObject(parent) {}

int BrowserTabModel::count() const noexcept
{
    return static_cast<int>(tabs_.size());
}

bool BrowserTabModel::isEmpty() const noexcept
{
    return tabs_.isEmpty();
}

int BrowserTabModel::activeIndex() const noexcept
{
    return activeIndex_;
}

QString BrowserTabModel::activeId() const
{
    return isValidIndex(activeIndex_) ? tabs_.at(activeIndex_).snapshot.id
                                      : QString();
}

int BrowserTabModel::indexOfId(const QString &id) const noexcept
{
    for (int index = 0; index < count(); ++index) {
        if (tabs_.at(index).snapshot.id == id) return index;
    }
    return -1;
}

BrowserTabSnapshot BrowserTabModel::snapshotAt(int index) const
{
    Q_ASSERT(isValidIndex(index));
    return tabs_.at(index).snapshot;
}

QVector<BrowserTabSnapshot> BrowserTabModel::snapshots() const
{
    QVector<BrowserTabSnapshot> result;
    result.reserve(tabs_.size());
    for (const TabState &tab : tabs_) result.append(tab.snapshot);
    return result;
}

BrowserTabLifecycle BrowserTabModel::lifecycleAt(int index) const
{
    Q_ASSERT(isValidIndex(index));
    return tabs_.at(index).lifecycle;
}

BrowserTabPresentation BrowserTabModel::presentationAt(int index) const
{
    Q_ASSERT(isValidIndex(index));
    const TabState &tab = tabs_.at(index);
    return BrowserTabPresentation{
        tab.loading,
        tab.progress,
        contentIdentityFor(tab.snapshot.kind),
        tab.visualState,
    };
}

BrowserTabAccessiblePresentation BrowserTabModel::accessiblePresentationAt(
    int index) const
{
    Q_ASSERT(isValidIndex(index));
    const TabState &tab = tabs_.at(index);
    const BrowserTabPresentation presentation = presentationAt(index);
    QString stateText;
    switch (presentation.visualState) {
    case BrowserVisualState::Recovering:
        stateText = QStringLiteral("Recovering");
        break;
    case BrowserVisualState::Crashed:
        stateText = QStringLiteral("Crashed");
        break;
    case BrowserVisualState::TrustedError:
        stateText = QStringLiteral("Error");
        break;
    case BrowserVisualState::Normal:
        stateText = presentation.loading
            ? QStringLiteral("Loading %1%").arg(presentation.progress)
            : QStringLiteral("Ready");
        break;
    }
    return BrowserTabAccessiblePresentation{
        tab.snapshot.title.isEmpty() ? fallbackTitle(tab.snapshot.kind)
                                     : tab.snapshot.title,
        QStringLiteral("%1, %2")
            .arg(identityText(presentation.contentIdentity), stateText),
    };
}

QString BrowserTabModel::createTab(BrowserTabKind kind,
                                   const QString &title,
                                   const QString &canonicalAddress,
                                   bool makeActive)
{
    if (mutationInProgress_) return {};
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    if (count() >= MaxOpenTabs || !isValidKind(kind)
        || canonicalAddress.isEmpty()) {
        return {};
    }
    const std::optional<QString> safeTitle = sanitizedTitle(title);
    if (!safeTitle.has_value()) return {};

    BrowserTabSnapshot snapshot;
    snapshot.id = generateId();
    snapshot.kind = kind;
    snapshot.title = *safeTitle;
    snapshot.address = canonicalAddress;
    snapshot.history = {canonicalAddress};
    snapshot.historyIndex = 0;

    const int oldActive = activeIndex_;
    const int insertedIndex = count();
    tabs_.append(dormantState(std::move(snapshot)));
    const QString createdId = tabs_.at(insertedIndex).snapshot.id;
    if (makeActive || activeIndex_ < 0) activeIndex_ = insertedIndex;

    emit tabInserted(insertedIndex, createdId);
    if (activeIndex_ != oldActive) {
        emit activeTabChanged(oldActive, activeIndex_);
    }
    emit persistenceNeeded();
    return createdId;
}

bool BrowserTabModel::activateTab(int index)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    if (!isValidIndex(index)) return false;
    if (index == activeIndex_) return true;
    const int oldActive = activeIndex_;
    activeIndex_ = index;
    emit activeTabChanged(oldActive, activeIndex_);
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::moveTab(int from, int to)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    if (!isValidIndex(from) || !isValidIndex(to)) return false;
    if (from == to) return true;

    const int oldActive = activeIndex_;
    const QString selectedId = activeId();
    tabs_.move(from, to);
    activeIndex_ = indexOfId(selectedId);

    emit tabMoved(from, to);
    if (activeIndex_ != oldActive) {
        emit activeTabChanged(oldActive, activeIndex_);
    }
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::closeTab(int index)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    if (!isValidIndex(index)) return false;

    const int oldActive = activeIndex_;
    const QString oldActiveId = activeId();
    const BrowserTabSnapshot closed = tabs_.at(index).snapshot;
    recentlyClosed_.append(closed);
    while (static_cast<int>(recentlyClosed_.size()) > MaxRecentlyClosed) {
        recentlyClosed_.removeFirst();
    }

    tabs_.removeAt(index);
    if (tabs_.isEmpty()) {
        activeIndex_ = -1;
    } else if (index == oldActive) {
        activeIndex_ = std::min(index, count() - 1);
    } else {
        activeIndex_ = indexOfId(oldActiveId);
    }
    const QString newActiveId = activeId();

    emit tabRemoved(index, closed.id);
    if (oldActive != activeIndex_ || oldActiveId != newActiveId) {
        emit activeTabChanged(oldActive, activeIndex_);
    }
    emit persistenceNeeded();
    return true;
}

QString BrowserTabModel::reopenMostRecentlyClosed(bool makeActive)
{
    if (mutationInProgress_) return {};
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    if (recentlyClosed_.isEmpty() || count() >= MaxOpenTabs) return {};

    BrowserTabSnapshot snapshot = recentlyClosed_.back();
    snapshot.id = generateId();
    const int oldActive = activeIndex_;
    const int insertedIndex = count();
    recentlyClosed_.removeLast();
    tabs_.append(dormantState(std::move(snapshot)));
    const QString reopenedId = tabs_.at(insertedIndex).snapshot.id;
    if (makeActive || activeIndex_ < 0) activeIndex_ = insertedIndex;

    emit tabInserted(insertedIndex, reopenedId);
    if (activeIndex_ != oldActive) {
        emit activeTabChanged(oldActive, activeIndex_);
    }
    emit persistenceNeeded();
    return reopenedId;
}

int BrowserTabModel::recentlyClosedCount() const noexcept
{
    return static_cast<int>(recentlyClosed_.size());
}

bool BrowserTabModel::navigateTab(const QString &id,
                                  BrowserTabKind validatedKind,
                                  const QString &canonicalAddress)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0 || !isValidKind(validatedKind)
        || canonicalAddress.isEmpty()) {
        return false;
    }
    TabState &tab = tabs_[index];
    BrowserTabSnapshot &snapshot = tab.snapshot;
    if (snapshot.historyIndex >= 0
        && snapshot.historyIndex < static_cast<int>(snapshot.history.size())
        && snapshot.history.at(snapshot.historyIndex) == canonicalAddress) {
        if (snapshot.kind == validatedKind) return true;
        applyValidatedKind(tab, validatedKind);
        emit tabChanged(index);
        emit persistenceNeeded();
        return true;
    }

    while (static_cast<int>(snapshot.history.size())
           > snapshot.historyIndex + 1) {
        snapshot.history.removeLast();
    }
    snapshot.history.append(canonicalAddress);
    while (static_cast<int>(snapshot.history.size()) > MaxHistoryEntries) {
        snapshot.history.removeFirst();
    }
    snapshot.historyIndex = static_cast<int>(snapshot.history.size()) - 1;
    snapshot.address = canonicalAddress;
    applyValidatedKind(tab, validatedKind);
    emit tabChanged(index);
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::canGoBack(const QString &id) const noexcept
{
    const int index = indexOfId(id);
    return index >= 0 && tabs_.at(index).snapshot.historyIndex > 0;
}

bool BrowserTabModel::canGoForward(const QString &id) const noexcept
{
    const int index = indexOfId(id);
    return index >= 0
        && tabs_.at(index).snapshot.historyIndex >= 0
        && tabs_.at(index).snapshot.historyIndex + 1
            < static_cast<int>(tabs_.at(index).snapshot.history.size());
}

bool BrowserTabModel::goBack(const QString &id,
                             BrowserTabKind validatedTargetKind)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0 || !isValidKind(validatedTargetKind)
        || !canGoBack(id)) {
        return false;
    }
    TabState &tab = tabs_[index];
    BrowserTabSnapshot &snapshot = tab.snapshot;
    --snapshot.historyIndex;
    snapshot.address = snapshot.history.at(snapshot.historyIndex);
    applyValidatedKind(tab, validatedTargetKind);
    emit tabChanged(index);
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::goForward(const QString &id,
                                BrowserTabKind validatedTargetKind)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0 || !isValidKind(validatedTargetKind)
        || !canGoForward(id)) {
        return false;
    }
    TabState &tab = tabs_[index];
    BrowserTabSnapshot &snapshot = tab.snapshot;
    ++snapshot.historyIndex;
    snapshot.address = snapshot.history.at(snapshot.historyIndex);
    applyValidatedKind(tab, validatedTargetKind);
    emit tabChanged(index);
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::setTitle(const QString &id, const QString &untrustedTitle)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0) return false;
    const std::optional<QString> safeTitle = sanitizedTitle(untrustedTitle);
    if (!safeTitle.has_value()) return false;
    if (tabs_.at(index).snapshot.title == *safeTitle) return true;
    tabs_[index].snapshot.title = *safeTitle;
    emit tabChanged(index);
    emit persistenceNeeded();
    return true;
}

bool BrowserTabModel::setLifecycle(const QString &id,
                                   BrowserTabLifecycle lifecycle)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0 || !isValidLifecycle(lifecycle)) return false;
    if (tabs_.at(index).lifecycle == lifecycle) return true;
    tabs_[index].lifecycle = lifecycle;
    emit tabChanged(index);
    return true;
}

bool BrowserTabModel::setLoadState(const QString &id, bool loading, int progress)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0) return false;
    const int boundedProgress = std::clamp(progress, 0, 100);
    TabState &tab = tabs_[index];
    if (tab.loading == loading && tab.progress == boundedProgress) return true;
    tab.loading = loading;
    tab.progress = boundedProgress;
    emit tabChanged(index);
    return true;
}

bool BrowserTabModel::setVisualState(const QString &id,
                                     BrowserVisualState visualState)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);
    const int index = indexOfId(id);
    if (index < 0 || !isValidVisualState(visualState)) return false;
    if (tabs_.at(index).visualState == visualState) return true;
    tabs_[index].visualState = visualState;
    emit tabChanged(index);
    return true;
}

bool BrowserTabModel::replaceFromValidatedSnapshot(
    const QVector<BrowserTabSnapshot> &snapshots,
    int activeIndex)
{
    if (mutationInProgress_) return false;
    QScopedValueRollback<bool> mutationGuard(mutationInProgress_, true);

    const QVector<BrowserTabSnapshot> ownedSnapshots = snapshots;
    const int restoredCount = static_cast<int>(ownedSnapshots.size());
    if (restoredCount > MaxOpenTabs
        || (restoredCount == 0 && activeIndex != -1)
        || (restoredCount > 0
            && (activeIndex < 0 || activeIndex >= restoredCount))) {
        return false;
    }

    QSet<QString> ids;
    for (const BrowserTabSnapshot &snapshot : ownedSnapshots) {
        if (!isValidRestoredSnapshot(snapshot) || ids.contains(snapshot.id)) {
            return false;
        }
        ids.insert(snapshot.id);
    }

    bool alreadyRestored = activeIndex_ == activeIndex
        && recentlyClosed_.isEmpty()
        && tabs_.size() == ownedSnapshots.size();
    if (alreadyRestored) {
        for (int index = 0; index < restoredCount; ++index) {
            const TabState &tab = tabs_.at(index);
            if (tab.snapshot != ownedSnapshots.at(index)
                || tab.lifecycle != BrowserTabLifecycle::Dormant
                || tab.loading || tab.progress != 0
                || tab.visualState
                    != defaultVisualStateFor(tab.snapshot.kind)) {
                alreadyRestored = false;
                break;
            }
        }
    }
    if (alreadyRestored) return true;

    const int oldActive = activeIndex_;
    QVector<QString> oldIds;
    oldIds.reserve(tabs_.size());
    for (const TabState &tab : tabs_) oldIds.append(tab.snapshot.id);
    QVector<TabState> restoredTabs;
    QVector<QString> restoredIds;
    restoredTabs.reserve(ownedSnapshots.size());
    restoredIds.reserve(ownedSnapshots.size());
    for (const BrowserTabSnapshot &snapshot : ownedSnapshots) {
        restoredTabs.append(dormantState(snapshot));
        restoredIds.append(snapshot.id);
    }
    tabs_.reserve(std::max(count(), restoredCount));
    recentlyClosed_.clear();
    activeIndex_ = -1;

    for (int index = static_cast<int>(oldIds.size()) - 1; index >= 0; --index) {
        tabs_.removeAt(index);
        emit tabRemoved(index, oldIds.at(index));
    }
    for (int index = 0; index < restoredCount; ++index) {
        tabs_.append(std::move(restoredTabs[index]));
        emit tabInserted(index, restoredIds.at(index));
    }
    activeIndex_ = activeIndex;
    if (restoredCount > 0 || oldActive != -1) {
        emit activeTabChanged(oldActive, activeIndex_);
    }
    return true;
}

bool BrowserTabModel::isValidIndex(int index) const noexcept
{
    return index >= 0 && index < count();
}

QString BrowserTabModel::generateId() const
{
    QString id;
    bool recentlyUsed = false;
    do {
        id = QUuid::createUuid().toString(QUuid::Id128).toLower();
        recentlyUsed = std::any_of(
            recentlyClosed_.cbegin(), recentlyClosed_.cend(),
            [&id](const BrowserTabSnapshot &snapshot) {
                return snapshot.id == id;
            });
    } while (indexOfId(id) >= 0 || recentlyUsed);
    return id;
}

bool BrowserTabModel::isValidId(const QString &id) noexcept
{
    if (id.size() != 32) return false;
    for (const QChar value : id) {
        const char16_t codeUnit = value.unicode();
        if (!((codeUnit >= u'0' && codeUnit <= u'9')
              || (codeUnit >= u'a' && codeUnit <= u'f'))) {
            return false;
        }
    }
    return true;
}

bool BrowserTabModel::isValidKind(BrowserTabKind kind) noexcept
{
    switch (kind) {
    case BrowserTabKind::Host:
    case BrowserTabKind::App:
    case BrowserTabKind::Web:
    case BrowserTabKind::TrustedError:
        return true;
    }
    return false;
}

bool BrowserTabModel::isValidLifecycle(BrowserTabLifecycle lifecycle) noexcept
{
    switch (lifecycle) {
    case BrowserTabLifecycle::Dormant:
    case BrowserTabLifecycle::Starting:
    case BrowserTabLifecycle::Loading:
    case BrowserTabLifecycle::Active:
    case BrowserTabLifecycle::Background:
    case BrowserTabLifecycle::TrustedError:
    case BrowserTabLifecycle::Closing:
    case BrowserTabLifecycle::Retired:
        return true;
    }
    return false;
}

bool BrowserTabModel::isValidVisualState(
    BrowserVisualState visualState) noexcept
{
    switch (visualState) {
    case BrowserVisualState::Normal:
    case BrowserVisualState::Recovering:
    case BrowserVisualState::Crashed:
    case BrowserVisualState::TrustedError:
        return true;
    }
    return false;
}

std::optional<QString> BrowserTabModel::sanitizedTitle(const QString &title)
{
    QString result;
    result.reserve(MaxTitleCodeUnits);
    bool prefixComplete = false;
    for (qsizetype index = 0; index < title.size(); ++index) {
        const QChar value = title.at(index);
        if (value.isHighSurrogate()) {
            if (index + 1 >= title.size()
                || !title.at(index + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            if (!prefixComplete
                && result.size() + 2 <= MaxTitleCodeUnits) {
                result.append(value);
                result.append(title.at(index + 1));
                prefixComplete = result.size() == MaxTitleCodeUnits;
            } else if (!prefixComplete) {
                prefixComplete = true;
            }
            ++index;
            continue;
        }
        if (value.isLowSurrogate()) return std::nullopt;
        if (value.category() == QChar::Other_Control
            || isBidiControl(value.unicode())) {
            continue;
        }
        if (!prefixComplete) {
            result.append(value);
            prefixComplete = result.size() == MaxTitleCodeUnits;
        }
    }
    return result;
}

BrowserContentIdentity BrowserTabModel::contentIdentityFor(
    BrowserTabKind kind) noexcept
{
    switch (kind) {
    case BrowserTabKind::Host:
    case BrowserTabKind::TrustedError:
        return BrowserContentIdentity::QBrowser;
    case BrowserTabKind::App:
        return BrowserContentIdentity::SignedApplication;
    case BrowserTabKind::Web:
        return BrowserContentIdentity::RestrictedWeb;
    }
    return BrowserContentIdentity::QBrowser;
}

BrowserVisualState BrowserTabModel::defaultVisualStateFor(
    BrowserTabKind kind) noexcept
{
    return kind == BrowserTabKind::TrustedError
        ? BrowserVisualState::TrustedError
        : BrowserVisualState::Normal;
}

void BrowserTabModel::applyValidatedKind(TabState &tab,
                                         BrowserTabKind validatedKind) noexcept
{
    if (tab.snapshot.kind == validatedKind) return;
    tab.snapshot.kind = validatedKind;
    tab.lifecycle = BrowserTabLifecycle::Dormant;
    tab.loading = false;
    tab.progress = 0;
    tab.visualState = defaultVisualStateFor(validatedKind);
}

BrowserTabModel::TabState BrowserTabModel::dormantState(
    BrowserTabSnapshot snapshot)
{
    TabState state;
    state.snapshot = std::move(snapshot);
    state.visualState = defaultVisualStateFor(state.snapshot.kind);
    return state;
}

bool BrowserTabModel::isValidRestoredSnapshot(
    const BrowserTabSnapshot &snapshot)
{
    if (!isValidId(snapshot.id) || !isValidKind(snapshot.kind)
        || snapshot.address.isEmpty() || snapshot.history.isEmpty()
        || static_cast<int>(snapshot.history.size()) > MaxHistoryEntries
        || snapshot.historyIndex < 0
        || snapshot.historyIndex >= static_cast<int>(snapshot.history.size())
        || snapshot.history.at(snapshot.historyIndex) != snapshot.address) {
        return false;
    }
    for (const QString &entry : snapshot.history) {
        if (entry.isEmpty()) return false;
    }
    const std::optional<QString> title = sanitizedTitle(snapshot.title);
    return title.has_value() && *title == snapshot.title;
}
