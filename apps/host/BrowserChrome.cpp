#include "BrowserChrome.h"

#include "NavigationBar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

namespace
{
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

QString displayTitle(const BrowserTabSnapshot &snapshot)
{
    return snapshot.title.isEmpty() ? fallbackTitle(snapshot.kind)
                                    : snapshot.title;
}

QString literalTabText(const QString &title)
{
    QString literal = title;
    literal.replace(u'&', QStringLiteral("&&"));
    return literal;
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

QString stateText(const BrowserTabPresentation &presentation)
{
    switch (presentation.visualState) {
    case BrowserVisualState::Normal:
        return presentation.loading
            ? QStringLiteral("Loading %1%").arg(
                  std::clamp(presentation.progress, 0, 100))
            : QStringLiteral("Ready");
    case BrowserVisualState::Recovering:
        return QStringLiteral("Recovering");
    case BrowserVisualState::Crashed:
        return QStringLiteral("Crashed");
    case BrowserVisualState::TrustedError:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Ready");
}

QString accessibleTabName(const BrowserTabSnapshot &snapshot,
                          const BrowserTabPresentation &presentation)
{
    return QStringLiteral("%1, %2, %3")
        .arg(displayTitle(snapshot),
             identityText(presentation.contentIdentity),
             stateText(presentation));
}

bool canGoBack(const BrowserTabSnapshot &snapshot) noexcept
{
    return snapshot.historyIndex > 0;
}

bool canGoForward(const BrowserTabSnapshot &snapshot) noexcept
{
    return snapshot.historyIndex >= 0
        && snapshot.historyIndex + 1
            < static_cast<int>(snapshot.history.size());
}

QIcon tabIcon(QWidget *owner,
              const BrowserTabPresentation &presentation)
{
    QStyle::StandardPixmap icon = QStyle::SP_FileIcon;
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
            }
        }
        break;
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
        if (index < 0) return;
        const QString id = tabBar_->tabData(index).toString();
        if (!id.isEmpty()) emit tabActivationRequested(id);
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

bool BrowserChrome::synchronizeTabs(
    const QVector<BrowserTabSnapshot> &snapshots,
    const QVector<BrowserTabPresentation> &presentations,
    const QString &activeTabId)
{
    if (snapshots.size() != presentations.size()) return false;

    QSet<QString> desiredIds;
    desiredIds.reserve(snapshots.size());
    int activeIndex = -1;
    for (qsizetype index = 0; index < snapshots.size(); ++index) {
        const QString &id = snapshots.at(index).id;
        if (id.isEmpty() || desiredIds.contains(id)) return false;
        desiredIds.insert(id);
        if (id == activeTabId) activeIndex = static_cast<int>(index);
    }
    if ((snapshots.isEmpty() && !activeTabId.isEmpty())
        || (!snapshots.isEmpty() && activeIndex < 0)) {
        return false;
    }

    const QSignalBlocker blockTabSignals(tabBar_);

    for (int index = tabBar_->count() - 1; index >= 0; --index) {
        if (!desiredIds.contains(tabBar_->tabData(index).toString())) {
            tabBar_->removeTab(index);
        }
    }

    for (int desiredIndex = 0; desiredIndex < snapshots.size(); ++desiredIndex) {
        const BrowserTabSnapshot &snapshot = snapshots.at(desiredIndex);
        const BrowserTabPresentation &presentation =
            presentations.at(desiredIndex);
        int currentIndex = tabIndexForId(snapshot.id);
        if (currentIndex < 0) {
            currentIndex = tabBar_->insertTab(
                desiredIndex, literalTabText(displayTitle(snapshot)));
            tabBar_->setTabData(currentIndex, snapshot.id);
        } else if (currentIndex != desiredIndex) {
            tabBar_->moveTab(currentIndex, desiredIndex);
            currentIndex = desiredIndex;
        }

        tabBar_->setTabData(currentIndex, snapshot.id);
        tabBar_->setTabText(
            currentIndex, literalTabText(displayTitle(snapshot)));
        tabBar_->setTabIcon(currentIndex, tabIcon(this, presentation));
        tabBar_->setAccessibleTabName(
            currentIndex, accessibleTabName(snapshot, presentation));
    }

    tabBar_->setCurrentIndex(activeIndex);
    if (activeIndex < 0) {
        navigationBar_->clearActivePresentation();
        window()->setWindowTitle(QStringLiteral("Q-Browser"));
        updateActionAvailability(false, 0, false, false, false);
        return true;
    }

    const BrowserTabSnapshot &activeSnapshot = snapshots.at(activeIndex);
    const BrowserTabPresentation &activePresentation =
        presentations.at(activeIndex);
    const bool backAvailable = canGoBack(activeSnapshot);
    const bool forwardAvailable = canGoForward(activeSnapshot);
    navigationBar_->setAddressText(activeSnapshot.address);
    navigationBar_->setActivePresentation(
        activePresentation, backAvailable, forwardAvailable);
    window()->setWindowTitle(
        QStringLiteral("%1 - Q-Browser").arg(displayTitle(activeSnapshot)));
    updateActionAvailability(true,
                             static_cast<int>(snapshots.size()),
                             backAvailable,
                             forwardAvailable,
                             activePresentation.loading);
    return true;
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
    if (actionForCommand(command) == nullptr) return;
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
