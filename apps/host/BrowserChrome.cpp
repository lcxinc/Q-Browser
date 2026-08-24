#include "BrowserChrome.h"

#include "NavigationBar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <vector>

namespace
{
constexpr int MaxSynchronizationPasses = BrowserTabModel::MaxOpenTabs;

QString literalTabText(const QString &title)
{
    QString literal = title;
    literal.replace(u'&', QStringLiteral("&&"));
    return literal;
}

bool isValidId(const QString &id) noexcept
{
    if (id.size() != 32) return false;
    for (const QChar value : id) {
        if ((value < QLatin1Char('0') || value > QLatin1Char('9'))
            && (value < QLatin1Char('a') || value > QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

std::optional<BrowserContentIdentity> identityForKind(
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
    return std::nullopt;
}

bool isValidVisualState(BrowserVisualState state) noexcept
{
    switch (state) {
    case BrowserVisualState::Normal:
    case BrowserVisualState::Recovering:
    case BrowserVisualState::Crashed:
    case BrowserVisualState::TrustedError:
        return true;
    }
    return false;
}

bool isValidSnapshot(const BrowserTabSnapshot &snapshot)
{
    const std::optional<BrowserContentIdentity> identity =
        identityForKind(snapshot.kind);
    if (!identity.has_value() || !isValidId(snapshot.id)
        || snapshot.address.isEmpty()
        || snapshot.history.isEmpty()
        || snapshot.history.size() > BrowserTabModel::MaxHistoryEntries
        || snapshot.historyIndex < 0) {
        return false;
    }
    const int historyCount = static_cast<int>(snapshot.history.size());
    if (snapshot.historyIndex >= historyCount
        || snapshot.address != snapshot.history.at(snapshot.historyIndex)) {
        return false;
    }
    for (const QString &entry : snapshot.history) {
        if (entry.isEmpty()) return false;
    }
    const std::optional<QString> canonical =
        BrowserTabModel::canonicalTitle(snapshot.title);
    return canonical.has_value() && *canonical == snapshot.title;
}

bool canGoBack(const BrowserTabSnapshot &snapshot) noexcept
{
    return snapshot.historyIndex > 0;
}

bool canGoForward(const BrowserTabSnapshot &snapshot) noexcept
{
    return static_cast<qsizetype>(snapshot.historyIndex) + 1
        < snapshot.history.size();
}

QIcon tabIcon(QWidget *owner,
              const BrowserTabPresentation &presentation)
{
    QStyle::StandardPixmap icon;
    switch (presentation.visualState) {
    case BrowserVisualState::Crashed:
        icon = QStyle::SP_MessageBoxCritical;
        break;
    case BrowserVisualState::TrustedError:
        icon = QStyle::SP_MessageBoxWarning;
        break;
    case BrowserVisualState::Recovering:
        icon = QStyle::SP_BrowserReload;
        break;
    case BrowserVisualState::Normal:
        if (presentation.loading) {
            icon = QStyle::SP_BrowserReload;
        } else {
            switch (presentation.contentIdentity) {
            case BrowserContentIdentity::QBrowser:
                icon = QStyle::SP_DirHomeIcon;
                break;
            case BrowserContentIdentity::SignedApplication:
                icon = QStyle::SP_ComputerIcon;
                break;
            case BrowserContentIdentity::RestrictedWeb:
                icon = QStyle::SP_DriveNetIcon;
                break;
            default:
                Q_UNREACHABLE_RETURN(QIcon());
            }
        }
        break;
    default:
        Q_UNREACHABLE_RETURN(QIcon());
    }
    return owner->style()->standardIcon(icon, nullptr, owner);
}

int numberedTabIndex(BrowserCommand command) noexcept
{
    switch (command) {
    case BrowserCommand::SelectTab1:
        return 0;
    case BrowserCommand::SelectTab2:
        return 1;
    case BrowserCommand::SelectTab3:
        return 2;
    case BrowserCommand::SelectTab4:
        return 3;
    case BrowserCommand::SelectTab5:
        return 4;
    case BrowserCommand::SelectTab6:
        return 5;
    case BrowserCommand::SelectTab7:
        return 6;
    case BrowserCommand::SelectTab8:
        return 7;
    case BrowserCommand::NewTab:
    case BrowserCommand::CloseTab:
    case BrowserCommand::ReopenClosedTab:
    case BrowserCommand::NextTab:
    case BrowserCommand::PreviousTab:
    case BrowserCommand::SelectLastTab:
    case BrowserCommand::FocusAddress:
    case BrowserCommand::Back:
    case BrowserCommand::Forward:
    case BrowserCommand::Reload:
    case BrowserCommand::Stop:
    case BrowserCommand::Home:
        return -1;
    }
    return -1;
}
}

BrowserChrome::BrowserChrome(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("browser-chrome"));
    setAccessibleName(QStringLiteral("Browser chrome"));
    setAccessibleDescription(
        QStringLiteral("Browser tabs, navigation, and address controls"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tabRow = new QWidget(this);
    tabRow->setObjectName(QStringLiteral("browser-tab-row"));
    tabRow->setAccessibleName(QStringLiteral("Tab strip"));
    tabRow->setAccessibleDescription(
        QStringLiteral("Open and arrange browser tabs"));
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(8, 4, 8, 4);
    tabLayout->setSpacing(4);

    tabBar_ = new QTabBar(tabRow);
    tabBar_->setObjectName(QStringLiteral("browser-tab-bar"));
    tabBar_->setAccessibleName(QStringLiteral("Browser tabs"));
    tabBar_->setAccessibleDescription(
        QStringLiteral("Open browser tabs in their current order"));
    tabBar_->setMovable(true);
    tabBar_->setTabsClosable(true);
    tabBar_->setExpanding(false);
    tabBar_->setElideMode(Qt::ElideRight);
    tabBar_->setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);

    newTabButton_ = new QToolButton(tabRow);
    newTabButton_->setObjectName(QStringLiteral("browser-new-tab"));
    newTabButton_->setAccessibleName(QStringLiteral("New tab"));
    newTabButton_->setAccessibleDescription(QStringLiteral("Open a new tab"));
    newTabButton_->setToolTip(QStringLiteral("Open a new tab"));
    newTabButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    tabLayout->addWidget(tabBar_, 1);
    tabLayout->addWidget(newTabButton_);
    layout->addWidget(tabRow);

    navigationBar_ = new NavigationBar(this);
    layout->addWidget(navigationBar_);

    createActions();
    newTabButton_->setDefaultAction(actionForCommand(BrowserCommand::NewTab));
    newTabButton_->setObjectName(QStringLiteral("browser-new-tab"));
    newTabButton_->setAccessibleName(QStringLiteral("New tab"));
    newTabButton_->setAccessibleDescription(QStringLiteral("Open a new tab"));
    navigationBar_->setReloadStopActions(
        actionForCommand(BrowserCommand::Reload),
        actionForCommand(BrowserCommand::Stop));

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0) {
            selectedTabId_.clear();
            return;
        }
        const QString id = tabBar_->tabData(index).toString();
        if (id.isEmpty() || id == selectedTabId_) return;
        selectedTabId_ = id;
        emit tabActivationRequested(id);
    });
    connect(tabBar_, &QTabBar::tabMoved, this, [this](int from, int to) {
        const QString id = tabBar_->tabData(to).toString();
        if (!id.isEmpty()) emit tabMoveRequested(id, from, to);
    });
    connect(tabBar_, &QTabBar::tabCloseRequested, this, [this](int index) {
        const QString id = tabBar_->tabData(index).toString();
        if (!id.isEmpty()) emit tabCloseRequested(id);
    });
    connect(navigationBar_, &NavigationBar::navigateRequested, this,
            &BrowserChrome::addressSubmitted);
    connect(navigationBar_, &NavigationBar::backRequested, this,
            [this] { triggerAction(BrowserCommand::Back); });
    connect(navigationBar_, &NavigationBar::forwardRequested, this,
            [this] { triggerAction(BrowserCommand::Forward); });
    connect(navigationBar_, &NavigationBar::homeRequested, this,
            [this] { triggerAction(BrowserCommand::Home); });

    updateActionAvailability(false, 0, false, false, false);
    navigationBar_->clearActivePresentation();
}

bool BrowserChrome::synchronizeTabs(const BrowserTabModel &model)
{
    if (terminalSynchronizationPhase_) return false;

    std::optional<PresentationBatch> batch = batchFromModel(model);
    if (!batch.has_value()) return false;

    if (synchronizationInProgress_) {
        pendingBatch_ = std::move(*batch);
        return true;
    }

    QScopedValueRollback<bool> synchronizationGuard(
        synchronizationInProgress_, true);
    PresentationBatch next = std::move(*batch);
    for (int pass = 0; pass < MaxSynchronizationPasses; ++pass) {
        pendingBatch_.reset();
        applyBatch(next);
        if (!pendingBatch_.has_value()) return true;
        next = std::move(*pendingBatch_);
    }

    // A signal observer that continually requests alternating states must not
    // turn presentation into unbounded recursion or a livelock. Apply the
    // newest bounded pending batch once with the common observer signals
    // blocked. Event filters can still observe QAction changes, so explicitly
    // reject synchronization requests during this terminal phase instead of
    // acknowledging a request that cannot be committed.
    pendingBatch_.reset();
    const QScopedValueRollback<bool> terminalGuard(
        terminalSynchronizationPhase_, true);
    applyBatchWithObserverSignalsBlocked(next);
    Q_ASSERT(!pendingBatch_.has_value());
    return true;
}

std::optional<BrowserChrome::PresentationBatch> BrowserChrome::batchFromModel(
    const BrowserTabModel &model) const
{
    const int modelCount = model.count();
    if (modelCount < 0 || modelCount > BrowserTabModel::MaxOpenTabs) {
        return std::nullopt;
    }

    const QVector<BrowserTabSnapshot> snapshots = model.snapshots();
    const QString activeTabId = model.activeId();
    const int activeIndex = model.activeIndex();
    if (snapshots.size() > BrowserTabModel::MaxOpenTabs
        || snapshots.size() != modelCount
        || (modelCount == 0
            && (activeIndex != -1 || !activeTabId.isEmpty()))
        || (modelCount > 0
            && (activeIndex < 0 || activeIndex >= modelCount))) {
        return std::nullopt;
    }

    PresentationBatch batch;
    batch.tabs.reserve(modelCount);
    batch.activeTabId = activeTabId;
    batch.tabCount = modelCount;
    batch.activeIndex = activeIndex;
    QSet<QString> ids;
    ids.reserve(modelCount);
    for (int index = 0; index < modelCount; ++index) {
        BrowserTabSnapshot snapshot = snapshots.at(index);
        if (!isValidSnapshot(snapshot) || ids.contains(snapshot.id)
            || !identityForKind(snapshot.kind).has_value()) {
            return std::nullopt;
        }
        ids.insert(snapshot.id);

        OwnedTabPresentation owned;
        owned.snapshot = std::move(snapshot);
        batch.tabs.append(std::move(owned));
    }

    if (modelCount > 0
        && (activeTabId.isEmpty()
            || batch.tabs.at(activeIndex).snapshot.id != activeTabId)) {
        return std::nullopt;
    }

    for (int index = 0; index < modelCount; ++index) {
        OwnedTabPresentation &owned = batch.tabs[index];
        const std::optional<BrowserContentIdentity> expectedIdentity =
            identityForKind(owned.snapshot.kind);
        const BrowserTabPresentation presentation = model.presentationAt(index);
        if (!expectedIdentity.has_value()
            || presentation.contentIdentity != *expectedIdentity
            || !isValidVisualState(presentation.visualState)
            || presentation.progress < 0 || presentation.progress > 100) {
            return std::nullopt;
        }
        BrowserTabAccessiblePresentation accessible =
            model.accessiblePresentationAt(index);
        if (accessible.name.isEmpty() || accessible.description.isEmpty()) {
            return std::nullopt;
        }
        owned.presentation = presentation;
        owned.accessiblePresentation = std::move(accessible);
        owned.displayTitle = owned.accessiblePresentation.name;
        owned.accessibleTabName = QStringLiteral("%1, %2")
                                      .arg(owned.accessiblePresentation.name,
                                           owned.accessiblePresentation
                                               .description);
    }
    return batch;
}

void BrowserChrome::applyBatch(const PresentationBatch &batch)
{
    Q_ASSERT(batch.tabCount >= 0
             && batch.tabCount <= BrowserTabModel::MaxOpenTabs
             && batch.tabs.size() == batch.tabCount);
    QSet<QString> desiredIds;
    desiredIds.reserve(batch.tabs.size());
    for (const OwnedTabPresentation &tab : batch.tabs) {
        desiredIds.insert(tab.snapshot.id);
    }

    const QSignalBlocker blockTabSignals(tabBar_);
    bool corruptView = tabBar_->count() > BrowserTabModel::MaxOpenTabs;
    QSet<QString> existingIds;
    for (int index = 0; !corruptView && index < tabBar_->count(); ++index) {
        const QString id = tabBar_->tabData(index).toString();
        if (id.isEmpty() || existingIds.contains(id)) {
            corruptView = true;
            break;
        }
        existingIds.insert(id);
    }
    if (corruptView) {
        while (tabBar_->count() > 0) tabBar_->removeTab(0);
    }

    for (int index = tabBar_->count() - 1; index >= 0; --index) {
        if (!desiredIds.contains(tabBar_->tabData(index).toString())) {
            tabBar_->removeTab(index);
        }
    }

    for (int desiredIndex = 0; desiredIndex < batch.tabCount;
         ++desiredIndex) {
        const OwnedTabPresentation &tab = batch.tabs.at(desiredIndex);
        int currentIndex = tabIndexForId(tab.snapshot.id);
        if (currentIndex < 0) {
            currentIndex = tabBar_->insertTab(
                desiredIndex, literalTabText(tab.displayTitle));
            tabBar_->setTabData(currentIndex, tab.snapshot.id);
        } else if (currentIndex != desiredIndex) {
            tabBar_->moveTab(currentIndex, desiredIndex);
            currentIndex = desiredIndex;
        }

        tabBar_->setTabData(currentIndex, tab.snapshot.id);
        tabBar_->setTabText(currentIndex,
                            literalTabText(tab.displayTitle));
        tabBar_->setTabIcon(currentIndex,
                            tabIcon(this, tab.presentation));
        tabBar_->setAccessibleTabName(currentIndex,
                                      tab.accessibleTabName);
    }

    selectedTabId_ = batch.activeTabId;
    tabBar_->setCurrentIndex(batch.activeIndex);
    if (batch.activeIndex < 0) {
        navigationBar_->clearActivePresentation();
        window()->setWindowTitle(QStringLiteral("Q-Browser"));
        updateActionAvailability(false, 0, false, false, false);
        return;
    }

    const OwnedTabPresentation &active = batch.tabs.at(batch.activeIndex);
    const bool backAvailable = canGoBack(active.snapshot);
    const bool forwardAvailable = canGoForward(active.snapshot);
    navigationBar_->setAddressText(active.snapshot.address);
    navigationBar_->setActivePresentation(active.presentation,
                                           backAvailable,
                                           forwardAvailable);
    window()->setWindowTitle(QStringLiteral("%1 - Q-Browser")
                                 .arg(active.displayTitle));
    updateActionAvailability(true,
                             batch.tabCount,
                             backAvailable,
                             forwardAvailable,
                             active.presentation.loading);
}

void BrowserChrome::applyBatchWithObserverSignalsBlocked(
    const PresentationBatch &batch)
{
    const QSignalBlocker windowSignals(window());
    const QSignalBlocker navigationSignals(navigationBar_);
    QLineEdit *const address = navigationBar_->findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    std::optional<QSignalBlocker> addressSignals;
    if (address != nullptr) addressSignals.emplace(address);
    std::vector<QSignalBlocker> actionSignals;
    actionSignals.reserve(static_cast<std::size_t>(commandActions_.size()));
    for (const CommandAction &mapping : commandActions_) {
        actionSignals.emplace_back(mapping.action);
    }
    applyBatch(batch);
}

QTabBar *BrowserChrome::tabBar() const noexcept
{
    return tabBar_;
}

NavigationBar *BrowserChrome::navigationBar() const noexcept
{
    return navigationBar_;
}

QAction *BrowserChrome::actionForCommand(BrowserCommand command) const noexcept
{
    for (const CommandAction &mapping : commandActions_) {
        if (mapping.command == command) return mapping.action;
    }
    return nullptr;
}

void BrowserChrome::dispatchCommand(BrowserCommand command)
{
    if (synchronizationInProgress_) return;

    QAction *const action = actionForCommand(command);
    if (action == nullptr || !action->isEnabled()) return;
    if (command == BrowserCommand::FocusAddress) {
        navigationBar_->focusAddressAndSelectAll();
    }
    emit commandRequested(command);
}

void BrowserChrome::createActions()
{
    const QList<BrowserCommand> commands = browserCommands();
    commandActions_.reserve(commands.size());
    for (BrowserCommand command : commands) {
        auto *action = new QAction(browserCommandText(command), this);
        action->setObjectName(browserCommandObjectName(command));
        action->setData(QVariant::fromValue(command));
        action->setShortcutContext(Qt::WindowShortcut);
        action->setShortcuts(browserCommandShortcuts(command));
        addAction(action);
        commandActions_.append({command, action});
        connect(action, &QAction::triggered, this,
                [this, command] { dispatchCommand(command); });
    }
}

void BrowserChrome::triggerAction(BrowserCommand command)
{
    QAction *const action = actionForCommand(command);
    if (action != nullptr) action->trigger();
}

void BrowserChrome::updateActionAvailability(bool hasActiveTab,
                                             int tabCount,
                                             bool canNavigateBack,
                                             bool canNavigateForward,
                                             bool loading)
{
    for (const CommandAction &mapping : commandActions_) {
        bool enabled = true;
        const int selectedIndex = numberedTabIndex(mapping.command);
        if (selectedIndex >= 0) {
            enabled = selectedIndex < tabCount;
        } else {
            switch (mapping.command) {
            case BrowserCommand::NewTab:
            case BrowserCommand::ReopenClosedTab:
                enabled = tabCount < BrowserTabModel::MaxOpenTabs;
                break;
            case BrowserCommand::FocusAddress:
                enabled = true;
                break;
            case BrowserCommand::CloseTab:
            case BrowserCommand::SelectLastTab:
            case BrowserCommand::Home:
            case BrowserCommand::Reload:
                enabled = hasActiveTab;
                break;
            case BrowserCommand::NextTab:
            case BrowserCommand::PreviousTab:
                enabled = tabCount > 1;
                break;
            case BrowserCommand::Back:
                enabled = canNavigateBack;
                break;
            case BrowserCommand::Forward:
                enabled = canNavigateForward;
                break;
            case BrowserCommand::Stop:
                enabled = hasActiveTab && loading;
                break;
            case BrowserCommand::SelectTab1:
            case BrowserCommand::SelectTab2:
            case BrowserCommand::SelectTab3:
            case BrowserCommand::SelectTab4:
            case BrowserCommand::SelectTab5:
            case BrowserCommand::SelectTab6:
            case BrowserCommand::SelectTab7:
            case BrowserCommand::SelectTab8:
                Q_UNREACHABLE();
                break;
            }
        }
        mapping.action->setEnabled(enabled);
    }
}

int BrowserChrome::tabIndexForId(const QString &tabId) const noexcept
{
    for (int index = 0; index < tabBar_->count(); ++index) {
        if (tabBar_->tabData(index).toString() == tabId) return index;
    }
    return -1;
}
