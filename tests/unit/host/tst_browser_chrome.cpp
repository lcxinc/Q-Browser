#include "BrowserChrome.h"
#include "BrowserCommand.h"
#include "NavigationBar.h"

#include <QAccessible>
#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QSignalSpy>
#include <QTabBar>
#include <QTest>
#include <QToolButton>
#include <QVector>

#include <optional>

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
}

class BrowserChromeTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
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
};

void BrowserChromeTest::initTestCase()
{
    qRegisterMetaType<BrowserCommand>();
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

    QVERIFY(chrome.synchronizeTabs(
        {first, second},
        {presentation(),
         presentation(BrowserContentIdentity::SignedApplication)},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), first.id);
    QCOMPARE(tabBar->tabData(1).toString(), second.id);

    QVERIFY(chrome.synchronizeTabs(
        {inserted, first, second},
        {presentation(BrowserContentIdentity::RestrictedWeb),
         presentation(),
         presentation(BrowserContentIdentity::SignedApplication)},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), inserted.id);
    QCOMPARE(tabBar->tabData(1).toString(), first.id);
    QCOMPARE(tabBar->tabData(2).toString(), second.id);

    QVERIFY(chrome.synchronizeTabs(
        {second, inserted, first},
        {presentation(BrowserContentIdentity::SignedApplication),
         presentation(BrowserContentIdentity::RestrictedWeb),
         presentation()},
        first.id));
    QCOMPARE(tabBar->tabData(0).toString(), second.id);
    QCOMPARE(tabBar->tabData(1).toString(), inserted.id);
    QCOMPARE(tabBar->tabData(2).toString(), first.id);

    QVERIFY(chrome.synchronizeTabs(
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

    QVERIFY(chrome.synchronizeTabs(
        {first, second, third},
        {presentation(),
         presentation(BrowserContentIdentity::SignedApplication),
         presentation(BrowserContentIdentity::RestrictedWeb)},
        second.id));
    QVERIFY(chrome.synchronizeTabs(
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
    QVERIFY(!chrome.synchronizeTabs(beforeTabs, {presentation()}, third.id));
    QVERIFY(!chrome.synchronizeTabs(beforeTabs, beforePresentations,
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
    QVERIFY(chrome.synchronizeTabs(
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
    QVERIFY(chrome.synchronizeTabs(
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
    QVERIFY(chrome.synchronizeTabs(
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

    QVERIFY(chrome.synchronizeTabs(
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
    QVERIFY(chrome.synchronizeTabs(tabs, presentations, tabs.at(4).id));
    chrome.resize(900, 160);
    chrome.show();
    chrome.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&chrome));
    chrome.setFocus(Qt::OtherFocusReason);

    QSignalSpy commands(&chrome, &BrowserChrome::commandRequested);
    QTest::keyClick(&chrome, static_cast<Qt::Key>(key), modifiers);

    QCOMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(), expected);
}

void BrowserChromeTest::focusAddressShortcutFocusesAndSelectsAll()
{
    BrowserChrome chrome;
    const BrowserTabSnapshot page = tab(
        tabId(u'6'), BrowserTabKind::Host, QStringLiteral("New tab"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    QVERIFY(chrome.synchronizeTabs({page}, {presentation()}, page.id));
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
    QVERIFY(chrome.synchronizeTabs(
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

    QVERIFY(chrome.synchronizeTabs(
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

QTEST_MAIN(BrowserChromeTest)

#include "tst_browser_chrome.moc"
