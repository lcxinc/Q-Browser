#include "BrowserChrome.h"
#include "BrowserCommand.h"
#include "NavigationBar.h"

#include <QAccessible>
#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QSignalSpy>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVector>

#include <concepts>
#include <functional>
#include <optional>
#include <utility>

namespace
{
QString tabId(QChar digit)
{
    return QString(32, digit);
}

BrowserTabSnapshot tab(const QString &id,
                       BrowserTabKind kind,
                       const QString &title,
                       const QStringList &history,
                       int historyIndex)
{
    BrowserTabSnapshot snapshot;
    snapshot.id = id;
    snapshot.kind = kind;
    snapshot.title = title;
    snapshot.history = history;
    snapshot.historyIndex = historyIndex;
    snapshot.address = history.at(historyIndex);
    return snapshot;
}

BrowserTabPresentation presentation(
    BrowserContentIdentity identity = BrowserContentIdentity::QBrowser,
    bool loading = false,
    int progress = 0,
    BrowserVisualState visualState = BrowserVisualState::Normal)
{
    BrowserTabPresentation value;
    value.loading = loading;
    value.progress = progress;
    value.contentIdentity = identity;
    value.visualState = visualState;
    return value;
}

QToolButton *toolButton(BrowserChrome &chrome, const QString &objectName)
{
    return chrome.findChild<QToolButton *>(objectName);
}

QAccessibleInterface *accessibleInterface(QObject *object)
{
    QAccessibleInterface *const interface =
        QAccessible::queryAccessibleInterface(object);
    Q_ASSERT(interface != nullptr);
    return interface;
}

QStringList accessiblePageTabNames(QTabBar *tabBar)
{
    QStringList names;
    QAccessibleInterface *const tabList = accessibleInterface(tabBar);
    for (int index = 0; index < tabList->childCount(); ++index) {
        QAccessibleInterface *const child = tabList->child(index);
        if (child != nullptr && child->role() == QAccessible::PageTab) {
            names.append(child->text(QAccessible::Name));
        }
    }
    return names;
}

bool collectUniqueAccessibleNodeIds(QAccessibleInterface *root,
                                    QSet<QAccessible::Id> &ids,
                                    int depth = 0)
{
    if (root == nullptr || depth > 64) return false;
    const QAccessible::Id id = QAccessible::uniqueId(root);
    if (id == 0 || ids.contains(id)) return false;
    ids.insert(id);
    for (int index = 0; index < root->childCount(); ++index) {
        QAccessibleInterface *const child = root->child(index);
        if (child != nullptr
            && !collectUniqueAccessibleNodeIds(child, ids, depth + 1)) {
            return false;
        }
    }
    return true;
}

template <typename Chrome>
concept HasModelSynchronization = requires(
    Chrome &chrome, const BrowserTabModel &model) {
    { chrome.synchronizeTabs(model) } -> std::same_as<bool>;
};

template <typename Chrome>
concept HasFreeVectorSynchronization = requires(
    Chrome &chrome,
    const QVector<BrowserTabSnapshot> &snapshots,
    const QVector<BrowserTabPresentation> &presentations,
    const QString &activeTabId) {
    { chrome.synchronizeTabs(snapshots, presentations, activeTabId) }
        -> std::same_as<bool>;
};

static_assert(HasModelSynchronization<BrowserChrome>);
static_assert(!HasFreeVectorSynchronization<BrowserChrome>);

bool restoreModel(BrowserTabModel &model,
                  const QVector<BrowserTabSnapshot> &snapshots,
                  const QVector<BrowserTabPresentation> &presentations,
                  const QString &activeTabId)
{
    if (snapshots.size() != presentations.size()
        || snapshots.size() > BrowserTabModel::MaxOpenTabs) {
        return false;
    }
    const int snapshotCount = static_cast<int>(snapshots.size());
    int activeIndex = -1;
    for (int index = 0; index < snapshotCount; ++index) {
        if (snapshots.at(index).id == activeTabId) activeIndex = index;
    }
    if ((snapshots.isEmpty() && !activeTabId.isEmpty())
        || (!snapshots.isEmpty() && activeIndex < 0)
        || !model.replaceFromValidatedSnapshot(snapshots, activeIndex)) {
        return false;
    }
    for (int index = 0; index < model.count(); ++index) {
        const BrowserTabPresentation expected = presentations.at(index);
        if (expected.progress < 0 || expected.progress > 100
            || model.presentationAt(index).contentIdentity
                != expected.contentIdentity
            || !model.setLoadState(model.snapshotAt(index).id,
                                   expected.loading,
                                   expected.progress)
            || !model.setVisualState(model.snapshotAt(index).id,
                                     expected.visualState)) {
            return false;
        }
    }
    return true;
}

bool synchronizeChrome(
    BrowserChrome &chrome,
    const QVector<BrowserTabSnapshot> &snapshots,
    const QVector<BrowserTabPresentation> &presentations,
    const QString &activeTabId)
{
    BrowserTabModel model;
    return restoreModel(model, snapshots, presentations, activeTabId)
        && chrome.synchronizeTabs(model);
}

enum class ObservableSynchronizationTransition
{
    BackToNoBack,
    ActiveToEmpty,
    EmptyToActive,
    IdleToLoading,
    LoadingToIdle,
    OneToMaximum,
    MaximumToOne,
};

class AlternatingActionSynchronizationFilter final : public QObject
{
public:
    AlternatingActionSynchronizationFilter(BrowserChrome *chrome,
                                           QAction *observedAction,
                                           const BrowserTabModel *idleModel,
                                           const BrowserTabModel *loadingModel)
        : chrome_(chrome),
          observedAction_(observedAction),
          idleModel_(idleModel),
          loadingModel_(loadingModel)
    {
    }

    bool enabled = false;
    int eventCount = 0;
    int maximumDepth = 0;
    bool lastRequestedLoading = false;
    QVector<bool> synchronizationResults;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!enabled || watched != chrome_
            || event->type() != QEvent::ActionChanged) {
            return false;
        }
        const auto *const actionEvent = static_cast<QActionEvent *>(event);
        if (actionEvent->action() != observedAction_) return false;

        ++depth_;
        maximumDepth = qMax(maximumDepth, depth_);
        ++eventCount;
        lastRequestedLoading = eventCount % 2 == 0;
        synchronizationResults.append(chrome_->synchronizeTabs(
            lastRequestedLoading ? *loadingModel_ : *idleModel_));
        --depth_;
        return false;
    }

private:
    BrowserChrome *chrome_ = nullptr;
    QAction *observedAction_ = nullptr;
    const BrowserTabModel *idleModel_ = nullptr;
    const BrowserTabModel *loadingModel_ = nullptr;
    int depth_ = 0;
};

enum class TabMutationObserver
{
    AddressTextChanged,
    WindowTitleChanged,
    ActionChanged,
};

class OneShotActionChangedObserver final : public QObject
{
public:
    OneShotActionChangedObserver(BrowserChrome *chrome,
                                 QAction *observedAction,
                                 std::function<void()> callback)
        : chrome_(chrome),
          observedAction_(observedAction),
          callback_(std::move(callback))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (invoked_ || watched != chrome_
            || event->type() != QEvent::ActionChanged) {
            return false;
        }
        const auto *const actionEvent = static_cast<QActionEvent *>(event);
        if (actionEvent->action() != observedAction_) return false;

        invoked_ = true;
        callback_();
        return false;
    }

private:
    BrowserChrome *chrome_ = nullptr;
    QAction *observedAction_ = nullptr;
    std::function<void()> callback_;
    bool invoked_ = false;
};
}

class BrowserChromeTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void tabRowUsesBrowserSizingAndOverflow();
    void tabOverflowKeepsNewTabAvailable();
    void stableIdsSurviveInsertMoveCloseAndDriveViewRequests();
    void modelToViewSynchronizationBlocksAllViewRequests();
    void navigationPresentationAndAddressEventsAreExplicit();
    void reloadStopUsesOneButtonAndChangesCommandWithLoading();
    void shortcutDispatch_data();
    void shortcutDispatch();
    void focusAddressShortcutFocusesAndSelectsAll();
    void chordTableIsUniqueAndSharedByRegisteredActions();
    void stableNamesPlainTextAndActualAccessibleRoles();
    void actualTabAccessibilityTracksLoadingRecoveryAndCrash();
    void modelBoundSynchronizationValidatesCanonicalStateAtomically();
    void nestedSynchronizationUsesOwnedLatestBatch();
    void alternatingNestedSynchronizationIsBounded();
    void commandsAreInertDuringObservableSynchronization_data();
    void commandsAreInertDuringObservableSynchronization();
    void terminalSynchronizationRejectsUncommittedRequest();
    void observableSynchronizationRepairsTabMutations_data();
    void observableSynchronizationRepairsTabMutations();
    void standaloneNavigationBarStartsNeutral();
    void disabledForeignAndDirectDispatchIsInert();
    void tabMovesDoNotRequestRedundantActivation();
    void corruptTabDataIsReconciledExactly();
    void actionCapacityMatrixIsExact();
    void accessibleTreeHasNoDuplicateReloadStopActions();
};

void BrowserChromeTest::initTestCase()
{
    qRegisterMetaType<BrowserCommand>();
}

void BrowserChromeTest::tabRowUsesBrowserSizingAndOverflow()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot shortTitle = tab(
        tabId(u'1'), BrowserTabKind::Host, QStringLiteral("Short"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    const BrowserTabSnapshot longTitle = tab(
        tabId(u'2'), BrowserTabKind::Web,
        QStringLiteral("A deliberately long browser tab title that must elide"),
        {QStringLiteral("app://pilot/web/help")}, 0);
    QVERIFY(synchronizeChrome(
        chrome,
        {shortTitle, longTitle},
        {presentation(),
         presentation(BrowserContentIdentity::RestrictedWeb)},
        shortTitle.id));

    chrome.resize(900, 160);
    chrome.show();
    QCoreApplication::processEvents();

    QWidget *const tabRow = chrome.findChild<QWidget *>(
        QStringLiteral("browser-tab-row"));
    QVERIFY(tabRow != nullptr);
    QCOMPARE(tabRow->height(), 42);

    QTabBar *const tabBar = chrome.tabBar();
    QVERIFY(tabBar != nullptr);
    QVERIFY(tabBar->documentMode());
    QVERIFY(tabBar->usesScrollButtons());
    QVERIFY(!tabBar->drawBase());

    QWidget *closeButton = tabBar->tabButton(0, QTabBar::RightSide);
    if (closeButton == nullptr) {
        closeButton = tabBar->tabButton(0, QTabBar::LeftSide);
    }
    QVERIFY(closeButton != nullptr);
    QVERIFY(closeButton->isVisible());
    QSignalSpy closeRequests(&chrome, &BrowserChrome::tabCloseRequested);
    QTest::mouseClick(closeButton, Qt::LeftButton);
    QCOMPARE(closeRequests.count(), 1);
    QCOMPARE(closeRequests.takeFirst().at(0).toString(), shortTitle.id);

    QToolButton *const newTab = toolButton(
        chrome, QStringLiteral("browser-new-tab"));
    QVERIFY(newTab != nullptr);
    QVERIFY(newTab->isVisible());
    QCOMPARE(newTab->text(), QStringLiteral("+"));
    QCOMPARE(newTab->focusPolicy(), Qt::StrongFocus);
    QVERIFY(!newTab->accessibleName().isEmpty());
    QVERIFY(!newTab->accessibleDescription().isEmpty());

    for (int index = 0; index < tabBar->count(); ++index) {
        const QRect tabRect = tabBar->tabRect(index);
        QVERIFY(tabBar->rect().contains(tabRect));
        QVERIFY(tabRect.width() >= 120);
        QVERIFY(tabRect.width() <= 240);
    }
}

void BrowserChromeTest::tabOverflowKeepsNewTabAvailable()
{
    BrowserChrome chrome;
    QVector<BrowserTabSnapshot> tabs;
    QVector<BrowserTabPresentation> presentations;
    const int tabCount = BrowserTabModel::MaxOpenTabs - 1;
    tabs.reserve(tabCount);
    presentations.reserve(tabCount);
    for (int index = 0; index < tabCount; ++index) {
        const QString id = QString::number(index + 1, 16)
            .rightJustified(32, QLatin1Char('0'));
        tabs.append(tab(
            id, BrowserTabKind::App,
            QStringLiteral("Overflow tab with a long title %1").arg(index + 1),
            {QStringLiteral("app://pilot/orders/%1").arg(index + 1)}, 0));
        presentations.append(
            presentation(BrowserContentIdentity::SignedApplication));
    }
    QVERIFY(synchronizeChrome(chrome, tabs, presentations, tabs.first().id));

    chrome.resize(360, 160);
    chrome.show();
    QCoreApplication::processEvents();

    QTabBar *const tabBar = chrome.tabBar();
    QToolButton *const newTab = toolButton(
        chrome, QStringLiteral("browser-new-tab"));
    QVERIFY(tabBar != nullptr);
    QVERIFY(newTab != nullptr);
    QVERIFY(newTab->isVisible());
    QVERIFY(newTab->isEnabled());

    bool hasVisibleScrollButton = false;
    const QList<QToolButton *> tabBarButtons =
        tabBar->findChildren<QToolButton *>(QString(),
                                            Qt::FindDirectChildrenOnly);
    for (const QToolButton *const button : tabBarButtons) {
        if (button->isVisible() && button->arrowType() != Qt::NoArrow) {
            hasVisibleScrollButton = true;
            break;
        }
    }
    QVERIFY(hasVisibleScrollButton);
}

void BrowserChromeTest::stableIdsSurviveInsertMoveCloseAndDriveViewRequests()
{
    BrowserChrome chrome;
    QTabBar *const tabBar = chrome.tabBar();
    QVERIFY(tabBar != nullptr);
    QVERIFY(tabBar->isMovable());
    QVERIFY(tabBar->tabsClosable());

    const BrowserTabSnapshot first = tab(
        tabId(u'1'), BrowserTabKind::Host, QStringLiteral("First"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    const BrowserTabSnapshot second = tab(
        tabId(u'2'), BrowserTabKind::App, QStringLiteral("Second"),
        {QStringLiteral("app://pilot/orders")}, 0);
    const BrowserTabSnapshot inserted = tab(
        tabId(u'3'), BrowserTabKind::Web, QStringLiteral("Inserted"),
        {QStringLiteral("app://pilot/web/help")}, 0);

    QVERIFY(synchronizeChrome(chrome,
        {first, second},
        {presentation(),
         presentation(BrowserContentIdentity::SignedApplication)},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), first.id);
    QCOMPARE(tabBar->tabData(1).toString(), second.id);

    QVERIFY(synchronizeChrome(chrome,
        {inserted, first, second},
        {presentation(BrowserContentIdentity::RestrictedWeb),
         presentation(),
         presentation(BrowserContentIdentity::SignedApplication)},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), inserted.id);
    QCOMPARE(tabBar->tabData(1).toString(), first.id);
    QCOMPARE(tabBar->tabData(2).toString(), second.id);

    QVERIFY(synchronizeChrome(chrome,
        {second, inserted, first},
        {presentation(BrowserContentIdentity::SignedApplication),
         presentation(BrowserContentIdentity::RestrictedWeb),
         presentation()},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), second.id);
    QCOMPARE(tabBar->tabData(1).toString(), inserted.id);
    QCOMPARE(tabBar->tabData(2).toString(), first.id);

    QVERIFY(synchronizeChrome(chrome,
        {second, first},
        {presentation(BrowserContentIdentity::SignedApplication),
         presentation()},
        first.id));
    QCOMPARE(tabBar->count(), 2);
    QCOMPARE(tabBar->tabData(0).toString(), second.id);
    QCOMPARE(tabBar->tabData(1).toString(), first.id);

    QSignalSpy activated(&chrome, &BrowserChrome::tabActivationRequested);
    QSignalSpy moved(&chrome, &BrowserChrome::tabMoveRequested);
    QSignalSpy closed(&chrome, &BrowserChrome::tabCloseRequested);

    tabBar->setCurrentIndex(0);
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.takeFirst().at(0).toString(), second.id);

    tabBar->moveTab(0, 1);
    QCOMPARE(moved.count(), 1);
    QCOMPARE(moved.at(0).at(0).toString(), second.id);
    QCOMPARE(moved.at(0).at(1).toInt(), 0);
    QCOMPARE(moved.at(0).at(2).toInt(), 1);
    QCOMPARE(tabBar->tabData(0).toString(), first.id);
    QCOMPARE(tabBar->tabData(1).toString(), second.id);

    QVERIFY(QMetaObject::invokeMethod(tabBar, "tabCloseRequested",
                                      Qt::DirectConnection, Q_ARG(int, 0)));
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.takeFirst().at(0).toString(), first.id);
}

void BrowserChromeTest::modelToViewSynchronizationBlocksAllViewRequests()
{
    BrowserChrome chrome;
    QTabBar *const tabBar = chrome.tabBar();
    QSignalSpy rawCurrentChanged(tabBar, &QTabBar::currentChanged);
    QSignalSpy rawMoved(tabBar, &QTabBar::tabMoved);
    QSignalSpy activated(&chrome, &BrowserChrome::tabActivationRequested);
    QSignalSpy moved(&chrome, &BrowserChrome::tabMoveRequested);
    QSignalSpy closed(&chrome, &BrowserChrome::tabCloseRequested);
    QSignalSpy submitted(&chrome, &BrowserChrome::addressSubmitted);
    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);

    const BrowserTabSnapshot first = tab(
        tabId(u'a'), BrowserTabKind::Host, QStringLiteral("A"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    const BrowserTabSnapshot second = tab(
        tabId(u'b'), BrowserTabKind::App, QStringLiteral("B"),
        {QStringLiteral("app://pilot/orders")}, 0);
    const BrowserTabSnapshot third = tab(
        tabId(u'c'), BrowserTabKind::Web, QStringLiteral("C"),
        {QStringLiteral("app://pilot/web/help")}, 0);

    QVERIFY(synchronizeChrome(chrome,
        {first, second, third},
        {presentation(),
         presentation(BrowserContentIdentity::SignedApplication),
         presentation(BrowserContentIdentity::RestrictedWeb)},
        second.id));
    QVERIFY(synchronizeChrome(chrome,
        {third, first},
        {presentation(BrowserContentIdentity::RestrictedWeb, true, 71),
         presentation()},
        third.id));

    QCOMPARE(rawCurrentChanged.count(), 0);
    QCOMPARE(rawMoved.count(), 0);
    QCOMPARE(activated.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(closed.count(), 0);
    QCOMPARE(submitted.count(), 0);
    QCOMPARE(commands.count(), 0);

    const QVector<BrowserTabSnapshot> beforeTabs{third, first};
    const QVector<BrowserTabPresentation> beforePresentations{
        presentation(BrowserContentIdentity::RestrictedWeb, true, 71),
        presentation()};
    QVERIFY(!synchronizeChrome(
        chrome, beforeTabs, {presentation()}, third.id));
    QVERIFY(!synchronizeChrome(chrome,
                               beforeTabs,
                               beforePresentations,
                               QStringLiteral("missing")));
    QCOMPARE(tabBar->count(), 2);
    QCOMPARE(tabBar->tabData(0).toString(), third.id);
    QCOMPARE(tabBar->tabData(1).toString(), first.id);
    QCOMPARE(rawCurrentChanged.count(), 0);
    QCOMPARE(rawMoved.count(), 0);
    QCOMPARE(activated.count(), 0);
    QCOMPARE(moved.count(), 0);
}

void BrowserChromeTest::navigationPresentationAndAddressEventsAreExplicit()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot orders = tab(
        tabId(u'4'), BrowserTabKind::App, QStringLiteral("Orders"),
        {QStringLiteral("app://pilot/dashboard"),
         QStringLiteral("app://pilot/orders"),
         QStringLiteral("app://pilot/orders/42")},
        1);
    QVERIFY(synchronizeChrome(chrome,
        {orders},
        {presentation(BrowserContentIdentity::SignedApplication)},
        orders.id));

    QToolButton *const back = toolButton(chrome, QStringLiteral("navigation-back"));
    QToolButton *const forward = toolButton(
        chrome, QStringLiteral("navigation-forward"));
    QToolButton *const home = toolButton(chrome, QStringLiteral("navigation-home"));
    QLabel *const identity = chrome.findChild<QLabel *>(
        QStringLiteral("navigation-content-identity"));
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(back != nullptr);
    QVERIFY(forward != nullptr);
    QVERIFY(home != nullptr);
    QVERIFY(identity != nullptr);
    QVERIFY(address != nullptr);
    QVERIFY(back->isEnabled());
    QVERIFY(forward->isEnabled());
    QCOMPARE(identity->text(), QStringLiteral("Signed application"));
    QCOMPARE(identity->textFormat(), Qt::PlainText);
    QCOMPARE(address->text(), QStringLiteral("app://pilot/orders"));
    QCOMPARE(chrome.window()->windowTitle(), QStringLiteral("Orders - Q-Browser"));

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    back->click();
    forward->click();
    home->click();
    QCOMPARE(commands.count(), 3);
    QCOMPARE(commands.at(0).at(0).value<BrowserCommand>(), BrowserCommand::Back);
    QCOMPARE(commands.at(1).at(0).value<BrowserCommand>(),
             BrowserCommand::Forward);
    QCOMPARE(commands.at(2).at(0).value<BrowserCommand>(), BrowserCommand::Home);

    QSignalSpy submitted(&chrome, &BrowserChrome::addressSubmitted);
    address->setText(QStringLiteral("not parsed by chrome"));
    QTest::keyClick(address, Qt::Key_Return);
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.takeFirst().at(0).toString(),
             QStringLiteral("not parsed by chrome"));

    BrowserTabSnapshot atStart = orders;
    atStart.address = atStart.history.first();
    atStart.historyIndex = 0;
    QVERIFY(synchronizeChrome(chrome,
        {atStart},
        {presentation(BrowserContentIdentity::SignedApplication)},
        atStart.id));
    QVERIFY(!back->isEnabled());
    QVERIFY(forward->isEnabled());
    QVERIFY(!chrome.actionForCommand(BrowserCommand::Back)->isEnabled());
    QVERIFY(chrome.actionForCommand(BrowserCommand::Forward)->isEnabled());
}

void BrowserChromeTest::reloadStopUsesOneButtonAndChangesCommandWithLoading()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot page = tab(
        tabId(u'5'), BrowserTabKind::Web, QStringLiteral("Help"),
        {QStringLiteral("app://pilot/web/help")}, 0);
    QVERIFY(synchronizeChrome(chrome,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb)},
        page.id));

    const QList<QToolButton *> reloadStopButtons =
        chrome.findChildren<QToolButton *>(
            QStringLiteral("navigation-reload-stop"));
    QCOMPARE(reloadStopButtons.size(), 1);
    QToolButton *const reloadStop = reloadStopButtons.first();
    QCOMPARE(reloadStop->text(), QStringLiteral("Reload"));
    QCOMPARE(reloadStop->accessibleName(), QStringLiteral("Reload"));
    QCOMPARE(reloadStop->defaultAction(),
             chrome.actionForCommand(BrowserCommand::Reload));

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    reloadStop->click();
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(),
             BrowserCommand::Reload);

    QVERIFY(synchronizeChrome(chrome,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb, true, 37)},
        page.id));
    QCOMPARE(chrome.findChildren<QToolButton *>(
                 QStringLiteral("navigation-reload-stop")).size(),
             1);
    QCOMPARE(reloadStopButtons.first(), reloadStop);
    QCOMPARE(reloadStop->text(), QStringLiteral("Stop"));
    QCOMPARE(reloadStop->accessibleName(), QStringLiteral("Stop"));
    QVERIFY(reloadStop->accessibleDescription().contains(
        QStringLiteral("37%")));
    QCOMPARE(reloadStop->property("loadProgress").toInt(), 37);
    QCOMPARE(reloadStop->defaultAction(),
             chrome.actionForCommand(BrowserCommand::Stop));

    reloadStop->click();
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(),
             BrowserCommand::Stop);
}

void BrowserChromeTest::shortcutDispatch_data()
{
    QTest::addColumn<BrowserCommand>("expected");
    QTest::addColumn<int>("key");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");

    const auto add = [](const char *name,
                        BrowserCommand command,
                        Qt::Key key,
                        Qt::KeyboardModifiers modifiers) {
        QTest::newRow(name) << command << static_cast<int>(key) << modifiers;
    };
    add("ctrl-t", BrowserCommand::NewTab, Qt::Key_T, Qt::ControlModifier);
    add("ctrl-w", BrowserCommand::CloseTab, Qt::Key_W, Qt::ControlModifier);
    add("ctrl-shift-t", BrowserCommand::ReopenClosedTab, Qt::Key_T,
        Qt::ControlModifier | Qt::ShiftModifier);
    add("ctrl-tab", BrowserCommand::NextTab, Qt::Key_Tab,
        Qt::ControlModifier);
    add("ctrl-shift-tab", BrowserCommand::PreviousTab, Qt::Key_Tab,
        Qt::ControlModifier | Qt::ShiftModifier);
    add("ctrl-1", BrowserCommand::SelectTab1, Qt::Key_1, Qt::ControlModifier);
    add("ctrl-2", BrowserCommand::SelectTab2, Qt::Key_2, Qt::ControlModifier);
    add("ctrl-3", BrowserCommand::SelectTab3, Qt::Key_3, Qt::ControlModifier);
    add("ctrl-4", BrowserCommand::SelectTab4, Qt::Key_4, Qt::ControlModifier);
    add("ctrl-5", BrowserCommand::SelectTab5, Qt::Key_5, Qt::ControlModifier);
    add("ctrl-6", BrowserCommand::SelectTab6, Qt::Key_6, Qt::ControlModifier);
    add("ctrl-7", BrowserCommand::SelectTab7, Qt::Key_7, Qt::ControlModifier);
    add("ctrl-8", BrowserCommand::SelectTab8, Qt::Key_8, Qt::ControlModifier);
    add("ctrl-9-last", BrowserCommand::SelectLastTab, Qt::Key_9,
        Qt::ControlModifier);
    add("ctrl-l", BrowserCommand::FocusAddress, Qt::Key_L,
        Qt::ControlModifier);
    add("alt-left", BrowserCommand::Back, Qt::Key_Left, Qt::AltModifier);
    add("alt-right", BrowserCommand::Forward, Qt::Key_Right, Qt::AltModifier);
    add("ctrl-r", BrowserCommand::Reload, Qt::Key_R, Qt::ControlModifier);
    add("f5", BrowserCommand::Reload, Qt::Key_F5, Qt::NoModifier);
    add("escape", BrowserCommand::Stop, Qt::Key_Escape, Qt::NoModifier);
}

void BrowserChromeTest::shortcutDispatch()
{
    QFETCH(BrowserCommand, expected);
    QFETCH(int, key);
    QFETCH(Qt::KeyboardModifiers, modifiers);

    BrowserChrome chrome;
    QVector<BrowserTabSnapshot> tabs;
    QVector<BrowserTabPresentation> presentations;
    for (int index = 0; index < 9; ++index) {
        const QString id = QString::number(index + 1, 16)
            .rightJustified(32, QLatin1Char('0'));
        tabs.append(tab(id,
                        BrowserTabKind::App,
                        QStringLiteral("Tab %1").arg(index + 1),
                        {QStringLiteral("app://pilot/orders/0"),
                         QStringLiteral("app://pilot/orders/1"),
                         QStringLiteral("app://pilot/orders/2")},
                        1));
        presentations.append(presentation(
            BrowserContentIdentity::SignedApplication, true, 50));
    }
    QVERIFY(synchronizeChrome(chrome, tabs, presentations, tabs.at(4).id));
    chrome.resize(900, 160);
    chrome.show();
    chrome.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&chrome));
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);
    address->setFocus(Qt::OtherFocusReason);
    QVERIFY(address->hasFocus());

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    QTest::keyClick(address, static_cast<Qt::Key>(key), modifiers);

    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(), expected);
}

void BrowserChromeTest::focusAddressShortcutFocusesAndSelectsAll()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot page = tab(
        tabId(u'6'), BrowserTabKind::Host, QStringLiteral("New tab"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    QVERIFY(synchronizeChrome(chrome, {page}, {presentation()}, page.id));
    chrome.resize(800, 160);
    chrome.show();
    chrome.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&chrome));

    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QToolButton *const newTab = toolButton(
        chrome, QStringLiteral("browser-new-tab"));
    QVERIFY(address != nullptr);
    QVERIFY(newTab != nullptr);
    newTab->setFocus(Qt::OtherFocusReason);
    QVERIFY(!address->hasFocus());

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    QTest::keyClick(newTab, Qt::Key_L, Qt::ControlModifier);

    QVERIFY(address->hasFocus());
    QCOMPARE(address->selectedText(), QStringLiteral("qbrowser://newtab"));
    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(),
             BrowserCommand::FocusAddress);
}

void BrowserChromeTest::chordTableIsUniqueAndSharedByRegisteredActions()
{
    const QList<BrowserCommandChord> chords = browserCommandChords();
    QCOMPARE(chords.size(), 20);
    QSet<int> uniqueChords;
    int reloadChordCount = 0;
    for (const BrowserCommandChord &mapping : chords) {
        const int chord = mapping.keyCombination.toCombined();
        QVERIFY2(!uniqueChords.contains(chord),
                 qPrintable(QKeySequence(mapping.keyCombination)
                                .toString(QKeySequence::PortableText)));
        uniqueChords.insert(chord);
        const std::optional<BrowserCommand> mapped =
            browserCommandForKeyCombination(mapping.keyCombination);
        QVERIFY(mapped.has_value());
        QCOMPARE(*mapped, mapping.command);
        QCOMPARE(browserCommandForShortcut(QKeySequence(mapping.keyCombination)),
                 mapped);
        if (mapping.command == BrowserCommand::Reload) ++reloadChordCount;
    }
    QCOMPARE(reloadChordCount, 2);

    BrowserChrome chrome;
    const QList<BrowserCommand> commands = browserCommands();
    QCOMPARE(chrome.actions().size(), commands.size());
    for (BrowserCommand command : commands) {
        QAction *const action = chrome.actionForCommand(command);
        QVERIFY(action != nullptr);
        QVERIFY(chrome.actions().contains(action));
        QCOMPARE(action->shortcutContext(), Qt::WindowShortcut);
        QCOMPARE(action->data().value<BrowserCommand>(), command);
        QVERIFY(!action->objectName().isEmpty());
    }

    for (const BrowserCommandChord &mapping : chords) {
        const QKeySequence shortcut(mapping.keyCombination);
        int owningActionCount = 0;
        for (QAction *const action : chrome.actions()) {
            if (action->shortcuts().contains(shortcut)) ++owningActionCount;
        }
        QCOMPARE(owningActionCount, 1);
        QVERIFY(chrome.actionForCommand(mapping.command)
                    ->shortcuts()
                    .contains(shortcut));
    }
}

void BrowserChromeTest::stableNamesPlainTextAndActualAccessibleRoles()
{
    BrowserChrome chrome;
    QCOMPARE(chrome.objectName(), QStringLiteral("browser-chrome"));
    QVERIFY(!chrome.accessibleName().isEmpty());
    QVERIFY(!chrome.accessibleDescription().isEmpty());

    const BrowserTabSnapshot page = tab(
        tabId(u'7'), BrowserTabKind::App, QStringLiteral("<b>R&D</b>"),
        {QStringLiteral("app://pilot/orders")}, 0);
    QVERIFY(synchronizeChrome(chrome,
        {page},
        {presentation(BrowserContentIdentity::SignedApplication)},
        page.id));

    QTabBar *const tabBar = chrome.tabBar();
    QCOMPARE(tabBar->objectName(), QStringLiteral("browser-tab-bar"));
    QCOMPARE(tabBar->tabText(0), QStringLiteral("<b>R&&D</b>"));
    QCOMPARE(chrome.window()->windowTitle(),
             QStringLiteral("<b>R&D</b> - Q-Browser"));
    QVERIFY(!tabBar->accessibleName().isEmpty());
    QVERIFY(!tabBar->accessibleDescription().isEmpty());

    const struct AccessibleWidget final {
        const char *objectName;
        QAccessible::Role role;
    } expected[] = {
        {"browser-tab-bar", QAccessible::PageTabList},
        {"browser-new-tab", QAccessible::Button},
        {"navigation-back", QAccessible::Button},
        {"navigation-forward", QAccessible::Button},
        {"navigation-reload-stop", QAccessible::Button},
        {"navigation-home", QAccessible::Button},
        {"navigation-address", QAccessible::EditableText},
        {"navigation-content-identity", QAccessible::StaticText},
    };
    for (const AccessibleWidget &entry : expected) {
        QObject *const object = chrome.findChild<QObject *>(
            QString::fromLatin1(entry.objectName));
        QVERIFY2(object != nullptr, entry.objectName);
        QAccessibleInterface *const interface = accessibleInterface(object);
        QCOMPARE(interface->role(), entry.role);
        QVERIFY2(!interface->text(QAccessible::Name).isEmpty(),
                 entry.objectName);
        // Qt's specialized PageTabList interface does not expose the
        // QWidget description; all concrete controls must expose both.
        if (entry.role != QAccessible::PageTabList) {
            QVERIFY2(!interface->text(QAccessible::Description).isEmpty(),
                     entry.objectName);
        }
        const auto *const widget = qobject_cast<QWidget *>(object);
        QVERIFY(widget != nullptr);
        QVERIFY2(!widget->accessibleName().isEmpty(), entry.objectName);
        QVERIFY2(!widget->accessibleDescription().isEmpty(), entry.objectName);
    }

    QAccessibleInterface *const tabList = accessibleInterface(tabBar);
    QCOMPARE(tabList->role(), QAccessible::PageTabList);
    int pageTabCount = 0;
    for (int index = 0; index < tabList->childCount(); ++index) {
        QAccessibleInterface *const child = tabList->child(index);
        if (child == nullptr || child->role() != QAccessible::PageTab) continue;
        ++pageTabCount;
        QCOMPARE(child->text(QAccessible::Name),
                 QStringLiteral("<b>R&D</b>, Signed application, Ready"));
    }
    QCOMPARE(pageTabCount, 1);

    QLabel *const identity = chrome.findChild<QLabel *>(
        QStringLiteral("navigation-content-identity"));
    QVERIFY(identity != nullptr);
    QCOMPARE(identity->textFormat(), Qt::PlainText);
}

void BrowserChromeTest::actualTabAccessibilityTracksLoadingRecoveryAndCrash()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot loading = tab(
        tabId(u'8'), BrowserTabKind::Web, QStringLiteral("Loading page"),
        {QStringLiteral("app://pilot/web/help")}, 0);
    const BrowserTabSnapshot recovering = tab(
        tabId(u'9'), BrowserTabKind::App, QStringLiteral("Orders"),
        {QStringLiteral("app://pilot/orders")}, 0);
    const BrowserTabSnapshot crashed = tab(
        tabId(u'a'), BrowserTabKind::App, QStringLiteral("Customers"),
        {QStringLiteral("app://pilot/customers")}, 0);

    QVERIFY(synchronizeChrome(chrome,
        {loading, recovering, crashed},
        {presentation(BrowserContentIdentity::RestrictedWeb, true, 63),
         presentation(BrowserContentIdentity::SignedApplication,
                      false,
                      0,
                      BrowserVisualState::Recovering),
         presentation(BrowserContentIdentity::SignedApplication,
                      false,
                      0,
                      BrowserVisualState::Crashed)},
        loading.id));

    QTabBar *const tabBar = chrome.tabBar();
    QCOMPARE(accessibleInterface(tabBar)->role(), QAccessible::PageTabList);
    const QStringList accessibleNames = accessiblePageTabNames(tabBar);
    QCOMPARE(accessibleNames.size(), 3);
    QVERIFY(accessibleNames.contains(
        QStringLiteral("Loading page, Restricted web, Loading 63%")));
    QVERIFY(accessibleNames.contains(
        QStringLiteral("Orders, Signed application, Recovering")));
    QVERIFY(accessibleNames.contains(
        QStringLiteral("Customers, Signed application, Crashed")));

    for (int index = 0; index < tabBar->count(); ++index) {
        QVERIFY(!tabBar->accessibleTabName(index).isEmpty());
    }
}

void BrowserChromeTest::modelBoundSynchronizationValidatesCanonicalStateAtomically()
{
    BrowserChrome chrome;
    BrowserTabModel model;
    const QString rawUnicodeTitle =
        QStringLiteral("Safe & literal\u202e\u0001 \U0001f680 title");
    const std::optional<QString> canonicalTitle =
        BrowserTabModel::canonicalTitle(rawUnicodeTitle);
    QVERIFY(canonicalTitle.has_value());
    QVERIFY(*canonicalTitle != rawUnicodeTitle);

    QVector<BrowserTabSnapshot> maximum;
    maximum.reserve(BrowserTabModel::MaxOpenTabs);
    for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
        const BrowserTabKind kind = [&] {
            switch (index % 4) {
            case 0:
                return BrowserTabKind::Host;
            case 1:
                return BrowserTabKind::App;
            case 2:
                return BrowserTabKind::Web;
            default:
                return BrowserTabKind::TrustedError;
            }
        }();
        const QString address = QStringLiteral("app://pilot/%1/1").arg(index);
        maximum.append(tab(
            QString::number(index, 16).rightJustified(32, QLatin1Char('0')),
            kind,
            index == 10 ? *canonicalTitle
                        : QStringLiteral("Tab %1").arg(index + 1),
            {QStringLiteral("app://pilot/%1/0").arg(index),
             address,
             QStringLiteral("app://pilot/%1/2").arg(index)},
            1));
    }
    constexpr int activeIndex = 10;
    QVERIFY(model.replaceFromValidatedSnapshot(maximum, activeIndex));
    QVERIFY(model.setLoadState(maximum.at(activeIndex).id, true, 58));
    QVERIFY(model.setVisualState(maximum.at(3).id,
                                 BrowserVisualState::Crashed));

    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(model.presentationAt(1).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(model.presentationAt(2).contentIdentity,
             BrowserContentIdentity::RestrictedWeb);
    QCOMPARE(model.presentationAt(3).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(model.accessiblePresentationAt(activeIndex).name,
             *canonicalTitle);
    QVERIFY(model.accessiblePresentationAt(activeIndex)
                .description
                .contains(QStringLiteral("Restricted web")));

    QVERIFY(chrome.synchronizeTabs(model));
    QCOMPARE(chrome.tabBar()->count(), BrowserTabModel::MaxOpenTabs);
    QCOMPARE(chrome.tabBar()->currentIndex(), activeIndex);
    QCOMPARE(chrome.tabBar()->tabText(activeIndex),
             QString(*canonicalTitle).replace(u'&', QStringLiteral("&&")));
    QCOMPARE(chrome.windowTitle(),
             QStringLiteral("%1 - Q-Browser").arg(*canonicalTitle));
    QVERIFY(chrome.actionForCommand(BrowserCommand::Back)->isEnabled());
    QVERIFY(chrome.actionForCommand(BrowserCommand::Forward)->isEnabled());
    QVERIFY(!chrome.actionForCommand(BrowserCommand::NewTab)->isEnabled());

    const QString stableActiveId =
        chrome.tabBar()->tabData(chrome.tabBar()->currentIndex()).toString();
    const QString stableTitle = chrome.windowTitle();
    const QString stableAddress = chrome.navigationBar()->addressText();

    QVector<BrowserTabSnapshot> oversized = maximum;
    oversized.append(tab(tabId(u'f'),
                         BrowserTabKind::Host,
                         QStringLiteral("Too many"),
                         {QStringLiteral("qbrowser://too-many")},
                         0));
    BrowserTabModel rejected;
    QVERIFY(!rejected.replaceFromValidatedSnapshot(oversized, 0));

    QVector<BrowserTabSnapshot> invalid{maximum.first()};
    invalid[0].title = rawUnicodeTitle;
    QVERIFY(!rejected.replaceFromValidatedSnapshot(invalid, 0));
    invalid[0] = maximum.first();
    invalid[0].historyIndex = static_cast<int>(invalid[0].history.size());
    QVERIFY(!rejected.replaceFromValidatedSnapshot(invalid, 0));
    invalid[0] = maximum.first();
    invalid[0].address = QStringLiteral("app://pilot/not-current-history");
    QVERIFY(!rejected.replaceFromValidatedSnapshot(invalid, 0));
    invalid[0] = maximum.first();
    invalid[0].kind = static_cast<BrowserTabKind>(99);
    QVERIFY(!rejected.replaceFromValidatedSnapshot(invalid, 0));
    QVERIFY(!rejected.replaceFromValidatedSnapshot(
        {maximum.first(), maximum.first()}, 0));
    QVERIFY(!rejected.replaceFromValidatedSnapshot({maximum.first()}, -1));
    QVERIFY(!rejected.replaceFromValidatedSnapshot({maximum.first()}, 1));
    QCOMPARE(rejected.count(), 0);

    QCOMPARE(chrome.tabBar()->count(), BrowserTabModel::MaxOpenTabs);
    QCOMPARE(chrome.tabBar()
                 ->tabData(chrome.tabBar()->currentIndex())
                 .toString(),
             stableActiveId);
    QCOMPARE(chrome.windowTitle(), stableTitle);
    QCOMPARE(chrome.navigationBar()->addressText(), stableAddress);
}

void BrowserChromeTest::nestedSynchronizationUsesOwnedLatestBatch()
{
    BrowserChrome chrome;
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QAction *const backAction = chrome.actionForCommand(BrowserCommand::Back);
    QLabel *const identity = chrome.findChild<QLabel *>(
        QStringLiteral("navigation-content-identity"));
    QVERIFY(address != nullptr);
    QVERIFY(backAction != nullptr);
    QVERIFY(identity != nullptr);

    const QVector<BrowserTabSnapshot> outerTabs{tab(
        tabId(u'1'), BrowserTabKind::App, QStringLiteral("Outer"),
        {QStringLiteral("app://pilot/outer/0"),
         QStringLiteral("app://pilot/outer/1")},
        1)};
    const QVector<BrowserTabPresentation> outerPresentations{
        presentation(BrowserContentIdentity::SignedApplication)};
    const QVector<BrowserTabSnapshot> addressTabs{tab(
        tabId(u'2'), BrowserTabKind::Web, QStringLiteral("Address nested"),
        {QStringLiteral("app://pilot/web/address")}, 0)};
    const QVector<BrowserTabPresentation> addressPresentations{
        presentation(BrowserContentIdentity::RestrictedWeb)};
    const QVector<BrowserTabSnapshot> titleTabs{tab(
        tabId(u'3'), BrowserTabKind::App, QStringLiteral("Title nested"),
        {QStringLiteral("app://pilot/title/0"),
         QStringLiteral("app://pilot/title/1")},
        1)};
    const QVector<BrowserTabPresentation> titlePresentations{
        presentation(BrowserContentIdentity::SignedApplication, true, 42)};
    const QVector<BrowserTabSnapshot> finalTabs{tab(
        tabId(u'4'), BrowserTabKind::Web, QStringLiteral("Latest"),
        {QStringLiteral("app://pilot/web/latest")}, 0)};
    const QVector<BrowserTabPresentation> finalPresentations{
        presentation(BrowserContentIdentity::RestrictedWeb,
                     false,
                     0,
                     BrowserVisualState::Crashed)};

    BrowserTabModel outerModel;
    BrowserTabModel addressModel;
    BrowserTabModel titleModel;
    BrowserTabModel finalModel;
    QVERIFY(restoreModel(outerModel,
                         outerTabs,
                         outerPresentations,
                         outerTabs.first().id));
    QVERIFY(restoreModel(addressModel,
                         addressTabs,
                         addressPresentations,
                         addressTabs.first().id));
    QVERIFY(restoreModel(titleModel,
                         titleTabs,
                         titlePresentations,
                         titleTabs.first().id));
    QVERIFY(restoreModel(finalModel,
                         finalTabs,
                         finalPresentations,
                         finalTabs.first().id));

    bool addressObserverRan = false;
    bool titleObserverRan = false;
    bool actionObserverRan = false;
    bool nestedAccepted = true;
    int observerDepth = 0;
    int maximumObserverDepth = 0;
    connect(address, &QLineEdit::textChanged, &chrome,
            [&](const QString &) {
                if (addressObserverRan) return;
                addressObserverRan = true;
                ++observerDepth;
                maximumObserverDepth = qMax(maximumObserverDepth,
                                            observerDepth);
                // Mutate the source model while the observer is suspended.
                // BrowserChrome must already own every value from its batch.
                nestedAccepted = outerModel.setTitle(
                    outerTabs.first().id,
                    QStringLiteral("Aliased outer mutation"));
                nestedAccepted = nestedAccepted
                    && chrome.synchronizeTabs(addressModel);
                --observerDepth;
            });
    connect(&chrome, &QWidget::windowTitleChanged, &chrome,
            [&](const QString &) {
                if (titleObserverRan) return;
                titleObserverRan = true;
                ++observerDepth;
                maximumObserverDepth = qMax(maximumObserverDepth,
                                            observerDepth);
                nestedAccepted = nestedAccepted
                    && chrome.synchronizeTabs(titleModel);
                --observerDepth;
            });
    connect(backAction, &QAction::changed, &chrome, [&] {
        if (actionObserverRan) return;
        actionObserverRan = true;
        ++observerDepth;
        maximumObserverDepth = qMax(maximumObserverDepth, observerDepth);
        nestedAccepted = nestedAccepted && chrome.synchronizeTabs(finalModel);
        --observerDepth;
    });

    QVERIFY(chrome.synchronizeTabs(outerModel));
    QVERIFY(nestedAccepted);
    QVERIFY(addressObserverRan);
    QVERIFY(titleObserverRan);
    QVERIFY(actionObserverRan);
    QCOMPARE(maximumObserverDepth, 1);

    QCOMPARE(chrome.tabBar()->count(), 1);
    QCOMPARE(chrome.tabBar()->tabData(0).toString(), finalTabs.first().id);
    QCOMPARE(chrome.tabBar()->tabText(0), QStringLiteral("Latest"));
    QCOMPARE(chrome.tabBar()->currentIndex(), 0);
    QCOMPARE(address->text(), finalTabs.first().address);
    QCOMPARE(identity->text(), QStringLiteral("Restricted web"));
    QCOMPARE(chrome.windowTitle(), QStringLiteral("Latest - Q-Browser"));
    QVERIFY(!backAction->isEnabled());
    QCOMPARE(toolButton(chrome, QStringLiteral("navigation-reload-stop"))
                 ->defaultAction(),
             chrome.actionForCommand(BrowserCommand::Reload));
}

void BrowserChromeTest::alternatingNestedSynchronizationIsBounded()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot first = tab(
        tabId(u'1'), BrowserTabKind::App, QStringLiteral("First"),
        {QStringLiteral("app://pilot/first")}, 0);
    const BrowserTabSnapshot second = tab(
        tabId(u'2'), BrowserTabKind::Web, QStringLiteral("Second"),
        {QStringLiteral("app://pilot/web/second")}, 0);
    BrowserTabModel firstModel;
    BrowserTabModel secondModel;
    QVERIFY(restoreModel(
        firstModel,
        {first},
        {presentation(BrowserContentIdentity::SignedApplication)},
        first.id));
    QVERIFY(restoreModel(
        secondModel,
        {second},
        {presentation(BrowserContentIdentity::RestrictedWeb)},
        second.id));

    int requestCount = 0;
    int observerDepth = 0;
    int maximumObserverDepth = 0;
    bool nestedAccepted = true;
    connect(&chrome, &QWidget::windowTitleChanged, &chrome,
            [&](const QString &) {
                if (requestCount >= 64) return;
                ++observerDepth;
                maximumObserverDepth = qMax(maximumObserverDepth,
                                            observerDepth);
                ++requestCount;
                nestedAccepted = nestedAccepted
                    && chrome.synchronizeTabs(
                        requestCount % 2 == 0 ? firstModel : secondModel);
                --observerDepth;
            });

    QVERIFY(chrome.synchronizeTabs(firstModel));
    QVERIFY(nestedAccepted);
    QCOMPARE(requestCount, BrowserTabModel::MaxOpenTabs);
    QCOMPARE(maximumObserverDepth, 1);
    QCOMPARE(chrome.tabBar()->count(), 1);
    QCOMPARE(chrome.tabBar()->tabData(0).toString(), first.id);
    QCOMPARE(chrome.navigationBar()->addressText(), first.address);
    QCOMPARE(chrome.windowTitle(), QStringLiteral("First - Q-Browser"));
    QCOMPARE(chrome.actionForCommand(BrowserCommand::Reload)->isEnabled(),
             true);
}

void BrowserChromeTest::commandsAreInertDuringObservableSynchronization_data()
{
    QTest::addColumn<int>("transitionValue");

    QTest::newRow("active-back-to-active-no-back")
        << static_cast<int>(
               ObservableSynchronizationTransition::BackToNoBack);
    QTest::newRow("active-to-empty")
        << static_cast<int>(
               ObservableSynchronizationTransition::ActiveToEmpty);
    QTest::newRow("empty-to-active")
        << static_cast<int>(
               ObservableSynchronizationTransition::EmptyToActive);
    QTest::newRow("idle-to-loading")
        << static_cast<int>(
               ObservableSynchronizationTransition::IdleToLoading);
    QTest::newRow("loading-to-idle")
        << static_cast<int>(
               ObservableSynchronizationTransition::LoadingToIdle);
    QTest::newRow("one-to-maximum")
        << static_cast<int>(
               ObservableSynchronizationTransition::OneToMaximum);
    QTest::newRow("maximum-to-one")
        << static_cast<int>(
               ObservableSynchronizationTransition::MaximumToOne);
}

void BrowserChromeTest::commandsAreInertDuringObservableSynchronization()
{
    QFETCH(int, transitionValue);
    const auto transition =
        static_cast<ObservableSynchronizationTransition>(transitionValue);

    QVector<BrowserTabSnapshot> beforeTabs;
    QVector<BrowserTabPresentation> beforePresentations;
    QString beforeActiveId;
    QVector<BrowserTabSnapshot> afterTabs;
    QVector<BrowserTabPresentation> afterPresentations;
    QString afterActiveId;

    auto appendMaximum = [](QVector<BrowserTabSnapshot> &tabs,
                            QVector<BrowserTabPresentation> &presentations,
                            const QString &stateName) {
        for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
            const QString id = QString::number(index, 16)
                                   .rightJustified(32, QLatin1Char('0'));
            tabs.append(tab(
                id,
                BrowserTabKind::App,
                index == 0 ? stateName
                           : QStringLiteral("%1 tab %2")
                                 .arg(stateName)
                                 .arg(index + 1),
                {QStringLiteral("app://pilot/%1/%2")
                     .arg(stateName.toLower())
                     .arg(index)},
                0));
            presentations.append(presentation(
                BrowserContentIdentity::SignedApplication));
        }
    };

    switch (transition) {
    case ObservableSynchronizationTransition::BackToNoBack:
        beforeTabs.append(tab(
            tabId(u'1'), BrowserTabKind::App, QStringLiteral("Back source"),
            {QStringLiteral("app://pilot/back/0"),
             QStringLiteral("app://pilot/back/1")},
            1));
        beforePresentations.append(presentation(
            BrowserContentIdentity::SignedApplication));
        beforeActiveId = beforeTabs.first().id;
        afterTabs.append(tab(
            tabId(u'1'), BrowserTabKind::App,
            QStringLiteral("No-back target"),
            {QStringLiteral("app://pilot/no-back/0"),
             QStringLiteral("app://pilot/no-back/1")},
            0));
        afterPresentations.append(presentation(
            BrowserContentIdentity::SignedApplication));
        afterActiveId = afterTabs.first().id;
        break;
    case ObservableSynchronizationTransition::ActiveToEmpty:
        beforeTabs.append(tab(
            tabId(u'2'), BrowserTabKind::Host,
            QStringLiteral("Active source"),
            {QStringLiteral("qbrowser://active-source")}, 0));
        beforePresentations.append(presentation());
        beforeActiveId = beforeTabs.first().id;
        break;
    case ObservableSynchronizationTransition::EmptyToActive:
        afterTabs.append(tab(
            tabId(u'3'), BrowserTabKind::Host,
            QStringLiteral("Active target"),
            {QStringLiteral("qbrowser://active-target")}, 0));
        afterPresentations.append(presentation());
        afterActiveId = afterTabs.first().id;
        break;
    case ObservableSynchronizationTransition::IdleToLoading:
        beforeTabs.append(tab(
            tabId(u'4'), BrowserTabKind::Web, QStringLiteral("Idle source"),
            {QStringLiteral("app://pilot/web/idle-source")}, 0));
        beforePresentations.append(presentation(
            BrowserContentIdentity::RestrictedWeb));
        beforeActiveId = beforeTabs.first().id;
        afterTabs.append(tab(
            tabId(u'4'), BrowserTabKind::Web,
            QStringLiteral("Loading target"),
            {QStringLiteral("app://pilot/web/loading-target")}, 0));
        afterPresentations.append(presentation(
            BrowserContentIdentity::RestrictedWeb, true, 47));
        afterActiveId = afterTabs.first().id;
        break;
    case ObservableSynchronizationTransition::LoadingToIdle:
        beforeTabs.append(tab(
            tabId(u'5'), BrowserTabKind::Web,
            QStringLiteral("Loading source"),
            {QStringLiteral("app://pilot/web/loading-source")}, 0));
        beforePresentations.append(presentation(
            BrowserContentIdentity::RestrictedWeb, true, 63));
        beforeActiveId = beforeTabs.first().id;
        afterTabs.append(tab(
            tabId(u'5'), BrowserTabKind::Web, QStringLiteral("Idle target"),
            {QStringLiteral("app://pilot/web/idle-target")}, 0));
        afterPresentations.append(presentation(
            BrowserContentIdentity::RestrictedWeb));
        afterActiveId = afterTabs.first().id;
        break;
    case ObservableSynchronizationTransition::OneToMaximum:
        beforeTabs.append(tab(
            tabId(u'e'), BrowserTabKind::App, QStringLiteral("One source"),
            {QStringLiteral("app://pilot/one-source")}, 0));
        beforePresentations.append(presentation(
            BrowserContentIdentity::SignedApplication));
        beforeActiveId = beforeTabs.first().id;
        appendMaximum(afterTabs,
                      afterPresentations,
                      QStringLiteral("Maximum target"));
        afterActiveId = afterTabs.first().id;
        break;
    case ObservableSynchronizationTransition::MaximumToOne:
        appendMaximum(beforeTabs,
                      beforePresentations,
                      QStringLiteral("Maximum source"));
        beforeActiveId = beforeTabs.first().id;
        afterTabs.append(tab(
            tabId(u'e'), BrowserTabKind::App, QStringLiteral("One target"),
            {QStringLiteral("app://pilot/one-target")}, 0));
        afterPresentations.append(presentation(
            BrowserContentIdentity::SignedApplication));
        afterActiveId = afterTabs.first().id;
        break;
    }

    BrowserTabModel beforeModel;
    BrowserTabModel afterModel;
    QVERIFY(restoreModel(beforeModel,
                         beforeTabs,
                         beforePresentations,
                         beforeActiveId));
    QVERIFY(restoreModel(afterModel,
                         afterTabs,
                         afterPresentations,
                         afterActiveId));

    BrowserChrome chrome;
    QVERIFY(chrome.synchronizeTabs(beforeModel));
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    int addressObserverCount = 0;
    int titleObserverCount = 0;
    bool observeSynchronization = true;
    auto attemptEveryCommandSource = [&] {
        for (BrowserCommand command : browserCommands()) {
            chrome.dispatchCommand(command);
        }
        for (const BrowserCommandChord &chord : browserCommandChords()) {
            const std::optional<BrowserCommand> foreignCommand =
                browserCommandForKeyCombination(chord.keyCombination);
            if (foreignCommand.has_value()) {
                chrome.dispatchCommand(*foreignCommand);
            }
        }
        for (BrowserCommand command : browserCommands()) {
            QAction *const action = chrome.actionForCommand(command);
            if (action != nullptr) action->trigger();
        }
        const QStringList toolButtonNames{
            QStringLiteral("browser-new-tab"),
            QStringLiteral("navigation-back"),
            QStringLiteral("navigation-forward"),
            QStringLiteral("navigation-reload-stop"),
            QStringLiteral("navigation-home"),
        };
        for (const QString &name : toolButtonNames) {
            QToolButton *const button = toolButton(chrome, name);
            if (button != nullptr) button->click();
        }
    };
    connect(address, &QLineEdit::textChanged, &chrome, [&](const QString &) {
        if (!observeSynchronization) return;
        ++addressObserverCount;
        attemptEveryCommandSource();
    });
    connect(&chrome, &QWidget::windowTitleChanged, &chrome,
            [&](const QString &) {
                if (!observeSynchronization) return;
                ++titleObserverCount;
                attemptEveryCommandSource();
            });

    QVERIFY(chrome.synchronizeTabs(afterModel));
    observeSynchronization = false;
    QVERIFY(addressObserverCount > 0);
    QVERIFY(titleObserverCount > 0);
    QCOMPARE(commands.count(), 0);
    QVERIFY(!address->hasSelectedText());

    auto enabled = [&](BrowserCommand command) {
        QAction *const action = chrome.actionForCommand(command);
        Q_ASSERT(action != nullptr);
        return action->isEnabled();
    };
    BrowserCommand steadyEnabled = BrowserCommand::Reload;
    BrowserCommand steadyDisabled = BrowserCommand::Stop;
    switch (transition) {
    case ObservableSynchronizationTransition::BackToNoBack:
        QVERIFY(!enabled(BrowserCommand::Back));
        QVERIFY(enabled(BrowserCommand::Forward));
        QVERIFY(enabled(BrowserCommand::CloseTab));
        break;
    case ObservableSynchronizationTransition::ActiveToEmpty:
        QVERIFY(!enabled(BrowserCommand::CloseTab));
        QVERIFY(!enabled(BrowserCommand::Reload));
        QVERIFY(!enabled(BrowserCommand::Home));
        QVERIFY(enabled(BrowserCommand::NewTab));
        steadyEnabled = BrowserCommand::NewTab;
        steadyDisabled = BrowserCommand::CloseTab;
        break;
    case ObservableSynchronizationTransition::EmptyToActive:
        QVERIFY(enabled(BrowserCommand::CloseTab));
        QVERIFY(enabled(BrowserCommand::Reload));
        QVERIFY(enabled(BrowserCommand::Home));
        break;
    case ObservableSynchronizationTransition::IdleToLoading:
        QVERIFY(enabled(BrowserCommand::Stop));
        QVERIFY(enabled(BrowserCommand::Reload));
        steadyEnabled = BrowserCommand::Stop;
        steadyDisabled = BrowserCommand::Back;
        break;
    case ObservableSynchronizationTransition::LoadingToIdle:
        QVERIFY(!enabled(BrowserCommand::Stop));
        QVERIFY(enabled(BrowserCommand::Reload));
        break;
    case ObservableSynchronizationTransition::OneToMaximum:
        QVERIFY(!enabled(BrowserCommand::NewTab));
        QVERIFY(!enabled(BrowserCommand::ReopenClosedTab));
        QVERIFY(enabled(BrowserCommand::CloseTab));
        steadyEnabled = BrowserCommand::CloseTab;
        steadyDisabled = BrowserCommand::NewTab;
        break;
    case ObservableSynchronizationTransition::MaximumToOne:
        QVERIFY(enabled(BrowserCommand::NewTab));
        QVERIFY(enabled(BrowserCommand::ReopenClosedTab));
        QVERIFY(enabled(BrowserCommand::CloseTab));
        break;
    }

    chrome.dispatchCommand(steadyEnabled);
    chrome.actionForCommand(steadyEnabled)->trigger();
    QCOMPARE(commands.count(), 2);
    chrome.dispatchCommand(steadyDisabled);
    chrome.actionForCommand(steadyDisabled)->trigger();
    QCOMPARE(commands.count(), 2);
}

void BrowserChromeTest::terminalSynchronizationRejectsUncommittedRequest()
{
    const BrowserTabSnapshot page = tab(
        tabId(u'6'), BrowserTabKind::Web, QStringLiteral("Alternating"),
        {QStringLiteral("app://pilot/web/alternating")}, 0);
    BrowserTabModel idleModel;
    BrowserTabModel loadingModel;
    QVERIFY(restoreModel(
        idleModel,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb)},
        page.id));
    QVERIFY(restoreModel(
        loadingModel,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb, true, 71)},
        page.id));

    BrowserChrome chrome;
    QVERIFY(chrome.synchronizeTabs(idleModel));
    QAction *const stopAction =
        chrome.actionForCommand(BrowserCommand::Stop);
    QToolButton *const reloadStop = toolButton(
        chrome, QStringLiteral("navigation-reload-stop"));
    QVERIFY(stopAction != nullptr);
    QVERIFY(reloadStop != nullptr);

    AlternatingActionSynchronizationFilter filter(
        &chrome, stopAction, &idleModel, &loadingModel);
    chrome.installEventFilter(&filter);
    filter.enabled = true;

    QVERIFY(chrome.synchronizeTabs(loadingModel));
    filter.enabled = false;
    QCOMPARE(filter.maximumDepth, 1);
    QCOMPARE(filter.eventCount, BrowserTabModel::MaxOpenTabs + 1);
    QCOMPARE(filter.synchronizationResults.size(), filter.eventCount);
    for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
        QVERIFY(filter.synchronizationResults.at(index));
    }
    QVERIFY(!filter.lastRequestedLoading);
    QCOMPARE(reloadStop->defaultAction(), stopAction);
    QVERIFY(stopAction->isEnabled());
    QCOMPARE(filter.synchronizationResults.last(), false);

    QVERIFY(chrome.synchronizeTabs(idleModel));
    QCOMPARE(reloadStop->defaultAction(),
             chrome.actionForCommand(BrowserCommand::Reload));
    QVERIFY(!stopAction->isEnabled());
}

void BrowserChromeTest::observableSynchronizationRepairsTabMutations_data()
{
    QTest::addColumn<int>("observerValue");
    QTest::addColumn<bool>("nestedEventLoop");

    const struct ObserverRow final {
        const char *name;
        TabMutationObserver observer;
    } observers[] = {
        {"address", TabMutationObserver::AddressTextChanged},
        {"window-title", TabMutationObserver::WindowTitleChanged},
        {"action", TabMutationObserver::ActionChanged},
    };
    for (const ObserverRow &row : observers) {
        QTest::newRow(row.name)
            << static_cast<int>(row.observer) << false;
        QTest::newRow(qPrintable(QStringLiteral("%1-nested-loop")
                                     .arg(QString::fromLatin1(row.name))))
            << static_cast<int>(row.observer) << true;
    }
}

void BrowserChromeTest::observableSynchronizationRepairsTabMutations()
{
    QFETCH(int, observerValue);
    QFETCH(bool, nestedEventLoop);
    const auto observer = static_cast<TabMutationObserver>(observerValue);

    const QVector<BrowserTabSnapshot> tabs{
        tab(tabId(u'1'),
            BrowserTabKind::Host,
            QStringLiteral("First batch"),
            {QStringLiteral("qbrowser://first-batch")},
            0),
        tab(tabId(u'2'),
            BrowserTabKind::Web,
            QStringLiteral("Active batch"),
            {QStringLiteral("app://pilot/web/active-batch/0"),
             QStringLiteral("app://pilot/web/active-batch/1")},
            1),
        tab(tabId(u'3'),
            BrowserTabKind::App,
            QStringLiteral("Third batch"),
            {QStringLiteral("app://pilot/third-batch")},
            0),
    };
    const QVector<BrowserTabPresentation> presentations{
        presentation(),
        presentation(BrowserContentIdentity::RestrictedWeb, true, 68),
        presentation(BrowserContentIdentity::SignedApplication),
    };
    BrowserTabModel model;
    QVERIFY(restoreModel(model, tabs, presentations, tabs.at(1).id));

    BrowserChrome chrome;
    QTabBar *const tabBar = chrome.tabBar();
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QAction *const backAction = chrome.actionForCommand(BrowserCommand::Back);
    QVERIFY(tabBar != nullptr);
    QVERIFY(address != nullptr);
    QVERIFY(backAction != nullptr);

    QSignalSpy rawCurrentChanges(tabBar, &QTabBar::currentChanged);
    QSignalSpy rawMoves(tabBar, &QTabBar::tabMoved);
    QSignalSpy activationRequests(
        &chrome, &BrowserChrome::tabActivationRequested);
    QSignalSpy moveRequests(&chrome, &BrowserChrome::tabMoveRequested);
    int observerInvocationCount = 0;
    bool nestedLoopExited = !nestedEventLoop;
    auto mutateTabs = [&] {
        tabBar->moveTab(0, 2);
        tabBar->setCurrentIndex(2);
    };
    auto runMutation = [&] {
        if (observerInvocationCount != 0) return;
        ++observerInvocationCount;
        if (!nestedEventLoop) {
            mutateTabs();
            return;
        }

        QEventLoop loop;
        QTimer::singleShot(0, &loop, [&] {
            mutateTabs();
            nestedLoopExited = true;
            loop.quit();
        });
        QCOMPARE(loop.exec(QEventLoop::ExcludeUserInputEvents), 0);
    };

    OneShotActionChangedObserver actionObserver(
        &chrome, backAction, runMutation);
    switch (observer) {
    case TabMutationObserver::AddressTextChanged:
        connect(address, &QLineEdit::textChanged, &chrome,
                [&](const QString &) { runMutation(); });
        break;
    case TabMutationObserver::WindowTitleChanged:
        connect(&chrome, &QWidget::windowTitleChanged, &chrome,
                [&](const QString &) { runMutation(); });
        break;
    case TabMutationObserver::ActionChanged:
        chrome.installEventFilter(&actionObserver);
        break;
    }

    QVERIFY(chrome.synchronizeTabs(model));
    QCOMPARE(observerInvocationCount, 1);
    QVERIFY(nestedLoopExited);
    QCOMPARE(rawCurrentChanges.count(), 0);
    QCOMPARE(rawMoves.count(), 0);
    QCOMPARE(activationRequests.count(), 0);
    QCOMPARE(moveRequests.count(), 0);

    QStringList compactOrder;
    for (int index = 0; index < tabBar->count(); ++index) {
        compactOrder.append(tabBar->tabData(index).toString().left(1));
    }
    const QString currentId = tabBar->currentIndex() < 0
        ? QStringLiteral("none")
        : tabBar->tabData(tabBar->currentIndex()).toString().left(1);
    const QString compactState = QStringLiteral("%1;current=%2")
                                     .arg(compactOrder.join(u','), currentId);
    QCOMPARE(compactState, QStringLiteral("1,2,3;current=2"));

    QCOMPARE(tabBar->count(), tabs.size());
    for (int index = 0; index < tabs.size(); ++index) {
        QCOMPARE(tabBar->tabData(index).toString(), tabs.at(index).id);
        QCOMPARE(tabBar->tabText(index), tabs.at(index).title);
        QVERIFY(!tabBar->tabIcon(index).isNull());
        const BrowserTabAccessiblePresentation accessible =
            model.accessiblePresentationAt(index);
        QCOMPARE(tabBar->accessibleTabName(index),
                 QStringLiteral("%1, %2")
                     .arg(accessible.name, accessible.description));
    }
    QCOMPARE(tabBar->currentIndex(), 1);
    QCOMPARE(tabBar->tabData(tabBar->currentIndex()).toString(),
             tabs.at(1).id);
    QCOMPARE(address->text(), tabs.at(1).address);
    QCOMPARE(chrome.windowTitle(), QStringLiteral("Active batch - Q-Browser"));
    QVERIFY(backAction->isEnabled());
    QVERIFY(!chrome.actionForCommand(BrowserCommand::Forward)->isEnabled());
    QVERIFY(chrome.actionForCommand(BrowserCommand::Reload)->isEnabled());
    QVERIFY(chrome.actionForCommand(BrowserCommand::Stop)->isEnabled());
    QVERIFY(chrome.actionForCommand(BrowserCommand::Home)->isEnabled());
    QCOMPARE(toolButton(chrome, QStringLiteral("navigation-reload-stop"))
                 ->defaultAction(),
             chrome.actionForCommand(BrowserCommand::Stop));

    tabBar->setCurrentIndex(2);
    QCOMPARE(activationRequests.count(), 1);
    QCOMPARE(activationRequests.first().at(0).toString(), tabs.at(2).id);
    tabBar->moveTab(0, 2);
    QCOMPARE(moveRequests.count(), 1);
    QCOMPARE(moveRequests.first().at(0).toString(), tabs.at(0).id);
    QCOMPARE(moveRequests.first().at(1).toInt(), 0);
    QCOMPARE(moveRequests.first().at(2).toInt(), 2);
    QCOMPARE(activationRequests.count(), 1);
}

void BrowserChromeTest::standaloneNavigationBarStartsNeutral()
{
    NavigationBar navigation;
    QToolButton *const reloadStop = navigation.findChild<QToolButton *>(
        QStringLiteral("navigation-reload-stop"));
    QToolButton *const home = navigation.findChild<QToolButton *>(
        QStringLiteral("navigation-home"));
    QLabel *const identity = navigation.findChild<QLabel *>(
        QStringLiteral("navigation-content-identity"));
    QLineEdit *const address = navigation.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(reloadStop != nullptr);
    QVERIFY(home != nullptr);
    QVERIFY(identity != nullptr);
    QVERIFY(address != nullptr);

    navigation.resize(800, 80);
    navigation.show();
    QCoreApplication::processEvents();
    QVERIFY(navigation.isVisible());
    QVERIFY(address->isVisible());

    QVERIFY(!reloadStop->isVisible());
    QVERIFY(!reloadStop->isEnabled());
    QVERIFY(!home->isVisible());
    QVERIFY(!home->isEnabled());
    QVERIFY(!identity->isVisible());
    QVERIFY(identity->text().isEmpty());
    QVERIFY(!accessibleInterface(identity)
                 ->text(QAccessible::Description)
                 .contains(QStringLiteral("Active content: Q-Browser")));

    QSignalSpy homeRequests(&navigation, &NavigationBar::homeRequested);
    reloadStop->click();
    home->click();
    QCOMPARE(homeRequests.count(), 0);
}

void BrowserChromeTest::disabledForeignAndDirectDispatchIsInert()
{
    BrowserChrome chrome;
    QLineEdit *const address = chrome.findChild<QLineEdit *>(
        QStringLiteral("navigation-address"));
    QVERIFY(address != nullptr);
    address->setText(QStringLiteral("retain-selection"));

    const struct DisabledDispatch final {
        BrowserCommand command;
        std::optional<QKeyCombination> chord;
    } disabled[] = {
        {BrowserCommand::CloseTab,
         QKeyCombination(Qt::ControlModifier, Qt::Key_W)},
        {BrowserCommand::NextTab,
         QKeyCombination(Qt::ControlModifier, Qt::Key_Tab)},
        {BrowserCommand::PreviousTab,
         QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier,
                         Qt::Key_Tab)},
        {BrowserCommand::SelectTab1,
         QKeyCombination(Qt::ControlModifier, Qt::Key_1)},
        {BrowserCommand::SelectTab2,
         QKeyCombination(Qt::ControlModifier, Qt::Key_2)},
        {BrowserCommand::SelectTab3,
         QKeyCombination(Qt::ControlModifier, Qt::Key_3)},
        {BrowserCommand::SelectTab4,
         QKeyCombination(Qt::ControlModifier, Qt::Key_4)},
        {BrowserCommand::SelectTab5,
         QKeyCombination(Qt::ControlModifier, Qt::Key_5)},
        {BrowserCommand::SelectTab6,
         QKeyCombination(Qt::ControlModifier, Qt::Key_6)},
        {BrowserCommand::SelectTab7,
         QKeyCombination(Qt::ControlModifier, Qt::Key_7)},
        {BrowserCommand::SelectTab8,
         QKeyCombination(Qt::ControlModifier, Qt::Key_8)},
        {BrowserCommand::SelectLastTab,
         QKeyCombination(Qt::ControlModifier, Qt::Key_9)},
        {BrowserCommand::Back,
         QKeyCombination(Qt::AltModifier, Qt::Key_Left)},
        {BrowserCommand::Forward,
         QKeyCombination(Qt::AltModifier, Qt::Key_Right)},
        {BrowserCommand::Reload,
         QKeyCombination(Qt::ControlModifier, Qt::Key_R)},
        {BrowserCommand::Stop,
         QKeyCombination(Qt::NoModifier, Qt::Key_Escape)},
        {BrowserCommand::Home, std::nullopt},
    };

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    for (const DisabledDispatch &entry : disabled) {
        QAction *const action = chrome.actionForCommand(entry.command);
        QVERIFY(action != nullptr);
        QVERIFY(!action->isEnabled());
        if (entry.chord.has_value()) {
            const std::optional<BrowserCommand> foreignCommand =
                browserCommandForKeyCombination(*entry.chord);
            QVERIFY(foreignCommand.has_value());
            QCOMPARE(*foreignCommand, entry.command);
            chrome.dispatchCommand(*foreignCommand);
        } else {
            chrome.dispatchCommand(entry.command);
        }
    }
    QCOMPARE(commands.count(), 0);
    QVERIFY(!address->hasSelectedText());

    QAction *const focusAction =
        chrome.actionForCommand(BrowserCommand::FocusAddress);
    QVERIFY(focusAction != nullptr);
    focusAction->setEnabled(false);
    chrome.dispatchCommand(*browserCommandForKeyCombination(
        QKeyCombination(Qt::ControlModifier, Qt::Key_L)));
    QCOMPARE(commands.count(), 0);
    QVERIFY(!address->hasFocus());
    QVERIFY(!address->hasSelectedText());
}

void BrowserChromeTest::tabMovesDoNotRequestRedundantActivation()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot first = tab(
        tabId(u'1'), BrowserTabKind::Host, QStringLiteral("First"),
        {QStringLiteral("qbrowser://first")}, 0);
    const BrowserTabSnapshot second = tab(
        tabId(u'2'), BrowserTabKind::App, QStringLiteral("Second"),
        {QStringLiteral("app://pilot/second")}, 0);
    const BrowserTabSnapshot third = tab(
        tabId(u'3'), BrowserTabKind::Web, QStringLiteral("Third"),
        {QStringLiteral("app://pilot/web/third")}, 0);
    QVERIFY(synchronizeChrome(chrome,
        {first, second, third},
        {presentation(),
         presentation(BrowserContentIdentity::SignedApplication),
         presentation(BrowserContentIdentity::RestrictedWeb)},
        first.id));

    QSignalSpy activations(&chrome, &BrowserChrome::tabActivationRequested);
    QSignalSpy moves(&chrome, &BrowserChrome::tabMoveRequested);
    chrome.tabBar()->moveTab(0, 2);
    QCOMPARE(moves.count(), 1);
    QCOMPARE(moves.takeFirst().at(0).toString(), first.id);
    QCOMPARE(activations.count(), 0);
    QCOMPARE(chrome.tabBar()
                 ->tabData(chrome.tabBar()->currentIndex())
                 .toString(),
             first.id);

    chrome.tabBar()->moveTab(0, 1);
    QCOMPARE(moves.count(), 1);
    QCOMPARE(moves.takeFirst().at(0).toString(), second.id);
    QCOMPARE(activations.count(), 0);

    chrome.tabBar()->setCurrentIndex(0);
    QCOMPARE(activations.count(), 1);
    QCOMPARE(activations.takeFirst().at(0).toString(), third.id);
}

void BrowserChromeTest::corruptTabDataIsReconciledExactly()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot first = tab(
        tabId(u'1'), BrowserTabKind::Host, QStringLiteral("First"),
        {QStringLiteral("qbrowser://first")}, 0);
    const BrowserTabSnapshot second = tab(
        tabId(u'2'), BrowserTabKind::App, QStringLiteral("Second"),
        {QStringLiteral("app://pilot/second")}, 0);
    const QVector<BrowserTabPresentation> presentations{
        presentation(),
        presentation(BrowserContentIdentity::SignedApplication)};
    QVERIFY(synchronizeChrome(chrome,
                              {first, second},
                              presentations,
                              first.id));

    QTabBar *const tabBar = chrome.tabBar();
    tabBar->setTabData(1, first.id);
    const int staleIndex = tabBar->addTab(QStringLiteral("stale"));
    tabBar->setTabData(staleIndex, tabId(u'f'));
    tabBar->addTab(QStringLiteral("empty"));
    QCOMPARE(tabBar->count(), 4);

    QVERIFY(synchronizeChrome(chrome,
                              {first, second},
                              presentations,
                              second.id));
    QCOMPARE(tabBar->count(), 2);
    QCOMPARE(tabBar->tabData(0).toString(), first.id);
    QCOMPARE(tabBar->tabData(1).toString(), second.id);
    QCOMPARE(tabBar->currentIndex(), 1);
}

void BrowserChromeTest::actionCapacityMatrixIsExact()
{
    BrowserChrome chrome;
    auto enabled = [&](BrowserCommand command) {
        QAction *const action = chrome.actionForCommand(command);
        Q_ASSERT(action != nullptr);
        return action->isEnabled();
    };

    QVERIFY(enabled(BrowserCommand::NewTab));
    QVERIFY(enabled(BrowserCommand::ReopenClosedTab));
    QVERIFY(enabled(BrowserCommand::FocusAddress));
    QVERIFY(!enabled(BrowserCommand::CloseTab));
    QVERIFY(!enabled(BrowserCommand::Reload));
    QVERIFY(!enabled(BrowserCommand::Home));

    const BrowserTabSnapshot one = tab(
        tabId(u'1'), BrowserTabKind::Host, QStringLiteral("One"),
        {QStringLiteral("qbrowser://one")}, 0);
    QVERIFY(synchronizeChrome(chrome, {one}, {presentation()}, one.id));
    QVERIFY(enabled(BrowserCommand::NewTab));
    QVERIFY(enabled(BrowserCommand::ReopenClosedTab));
    QVERIFY(enabled(BrowserCommand::CloseTab));
    QVERIFY(enabled(BrowserCommand::Reload));
    QVERIFY(enabled(BrowserCommand::Home));
    QVERIFY(!enabled(BrowserCommand::NextTab));

    BrowserTabModel emptyModel;
    QVERIFY(chrome.synchronizeTabs(emptyModel));
    QVERIFY(enabled(BrowserCommand::NewTab));
    QVERIFY(enabled(BrowserCommand::ReopenClosedTab));
    QVERIFY(!enabled(BrowserCommand::CloseTab));
    QVERIFY(!enabled(BrowserCommand::Reload));
    QVERIFY(!enabled(BrowserCommand::Home));
    QToolButton *const emptyReloadStop = toolButton(
        chrome, QStringLiteral("navigation-reload-stop"));
    QToolButton *const emptyHome = toolButton(
        chrome, QStringLiteral("navigation-home"));
    QLabel *const emptyIdentity = chrome.findChild<QLabel *>(
        QStringLiteral("navigation-content-identity"));
    QVERIFY(emptyReloadStop != nullptr);
    QVERIFY(emptyHome != nullptr);
    QVERIFY(emptyIdentity != nullptr);
    QVERIFY(emptyReloadStop->isHidden());
    QVERIFY(!emptyReloadStop->isEnabled());
    QVERIFY(emptyHome->isHidden());
    QVERIFY(!emptyHome->isEnabled());
    QVERIFY(emptyIdentity->isHidden());
    QVERIFY(emptyIdentity->text().isEmpty());
    QCOMPARE(chrome.navigationBar()->addressText(), QString());
    QCOMPARE(chrome.windowTitle(), QStringLiteral("Q-Browser"));

    QVector<BrowserTabSnapshot> tabs;
    QVector<BrowserTabPresentation> presentations;
    for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
        const QString id = QString::number(index, 16)
            .rightJustified(32, QLatin1Char('0'));
        tabs.append(tab(id,
                        BrowserTabKind::App,
                        QStringLiteral("Tab %1").arg(index + 1),
                        {QStringLiteral("app://pilot/%1").arg(index)},
                        0));
        presentations.append(presentation(
            BrowserContentIdentity::SignedApplication));
    }
    QVERIFY(synchronizeChrome(chrome, tabs, presentations, tabs.first().id));
    QVERIFY(!enabled(BrowserCommand::NewTab));
    QVERIFY(!enabled(BrowserCommand::ReopenClosedTab));
    QVERIFY(enabled(BrowserCommand::CloseTab));
    QVERIFY(enabled(BrowserCommand::NextTab));

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    chrome.dispatchCommand(*browserCommandForKeyCombination(
        QKeyCombination(Qt::ControlModifier, Qt::Key_T)));
    chrome.dispatchCommand(*browserCommandForKeyCombination(
        QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier,
                        Qt::Key_T)));
    QCOMPARE(commands.count(), 0);
}

void BrowserChromeTest::accessibleTreeHasNoDuplicateReloadStopActions()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot page = tab(
        tabId(u'1'), BrowserTabKind::Web, QStringLiteral("Help"),
        {QStringLiteral("app://pilot/web/help")}, 0);
    QVERIFY(synchronizeChrome(chrome,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb)},
        page.id));

    QToolButton *const reloadStop = toolButton(
        chrome, QStringLiteral("navigation-reload-stop"));
    QVERIFY(reloadStop != nullptr);
    QAccessibleInterface *const buttonInterface =
        accessibleInterface(reloadStop);
    QCOMPARE(buttonInterface->role(), QAccessible::Button);
    QCOMPARE(buttonInterface->text(QAccessible::Name),
             QStringLiteral("Reload"));
    QCOMPARE(buttonInterface->text(QAccessible::Description),
             QStringLiteral("Reload the active tab"));
    QCOMPARE(buttonInterface->childCount(), 0);
    QCOMPARE(reloadStop->actions().size(), 1);
    QCOMPARE(reloadStop->actions().first(),
             chrome.actionForCommand(BrowserCommand::Reload));
    QSet<QAccessible::Id> beforeNodeIds;
    QVERIFY(collectUniqueAccessibleNodeIds(accessibleInterface(&chrome),
                                           beforeNodeIds));

    QVERIFY(synchronizeChrome(chrome,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb, true, 61)},
        page.id));
    QCOMPARE(buttonInterface->role(), QAccessible::Button);
    QCOMPARE(buttonInterface->text(QAccessible::Name), QStringLiteral("Stop"));
    QCOMPARE(buttonInterface->text(QAccessible::Description),
             QStringLiteral("Stop loading the active tab (61%)"));
    QCOMPARE(buttonInterface->childCount(), 0);
    QCOMPARE(reloadStop->actions().size(), 1);
    QCOMPARE(reloadStop->actions().first(),
             chrome.actionForCommand(BrowserCommand::Stop));
    QSet<QAccessible::Id> afterNodeIds;
    QVERIFY(collectUniqueAccessibleNodeIds(accessibleInterface(&chrome),
                                           afterNodeIds));
    QCOMPARE(afterNodeIds.size(), beforeNodeIds.size());

    QVERIFY(synchronizeChrome(
        chrome,
        {page},
        {presentation(BrowserContentIdentity::RestrictedWeb)},
        page.id));
    QCOMPARE(buttonInterface->text(QAccessible::Name),
             QStringLiteral("Reload"));
    QCOMPARE(buttonInterface->text(QAccessible::Description),
             QStringLiteral("Reload the active tab"));
    QCOMPARE(reloadStop->actions().size(), 1);
    QCOMPARE(reloadStop->actions().first(),
             chrome.actionForCommand(BrowserCommand::Reload));
    QSet<QAccessible::Id> restoredNodeIds;
    QVERIFY(collectUniqueAccessibleNodeIds(accessibleInterface(&chrome),
                                           restoredNodeIds));
    QCOMPARE(restoredNodeIds.size(), beforeNodeIds.size());

    QAccessibleInterface *const tabList = accessibleInterface(chrome.tabBar());
    QCOMPARE(tabList->role(), QAccessible::PageTabList);
    QSet<QString> pageTabNames;
    int pageTabCount = 0;
    for (int index = 0; index < tabList->childCount(); ++index) {
        QAccessibleInterface *const child = tabList->child(index);
        if (child == nullptr || child->role() != QAccessible::PageTab) continue;
        ++pageTabCount;
        QVERIFY(!pageTabNames.contains(child->text(QAccessible::Name)));
        pageTabNames.insert(child->text(QAccessible::Name));
        QCOMPARE(tabList->indexOfChild(child), index);
    }
    QCOMPARE(pageTabCount, 1);
    QCOMPARE(pageTabNames.size(), pageTabCount);
}

QTEST_MAIN(BrowserChromeTest)

#include "tst_browser_chrome.moc"
