#include "BrowserTabModel.h"

#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <type_traits>

namespace
{
template <typename T>
concept HasPid = requires(T value) { value.pid; };
template <typename T>
concept HasHwnd = requires(T value) { value.hwnd; };
template <typename T>
concept HasNonce = requires(T value) { value.nonce; };
template <typename T>
concept HasGeneration = requires(T value) { value.generation; };
template <typename T>
concept HasRequest = requires(T value) { value.request; };
template <typename T>
concept HasCapability = requires(T value) { value.capability; };
template <typename T>
concept HasCookie = requires(T value) { value.cookie; };
template <typename T>
concept HasGrant = requires(T value) { value.grant; };

static_assert(std::is_copy_constructible_v<BrowserTabSnapshot>);
static_assert(!HasPid<BrowserTabSnapshot>);
static_assert(!HasHwnd<BrowserTabSnapshot>);
static_assert(!HasNonce<BrowserTabSnapshot>);
static_assert(!HasGeneration<BrowserTabSnapshot>);
static_assert(!HasRequest<BrowserTabSnapshot>);
static_assert(!HasCapability<BrowserTabSnapshot>);
static_assert(!HasCookie<BrowserTabSnapshot>);
static_assert(!HasGrant<BrowserTabSnapshot>);

QString validId(int value)
{
    return QString::number(value, 16).rightJustified(32, QLatin1Char('0'));
}

BrowserTabSnapshot restoredTab(int id,
                              BrowserTabKind kind,
                              const QString &title,
                              const QStringList &history,
                              int historyIndex)
{
    BrowserTabSnapshot snapshot;
    snapshot.id = validId(id);
    snapshot.kind = kind;
    snapshot.title = title;
    snapshot.history = history;
    snapshot.historyIndex = historyIndex;
    snapshot.address = history.at(historyIndex);
    return snapshot;
}

void clearSpies(QSignalSpy &inserted,
                QSignalSpy &removed,
                QSignalSpy &moved,
                QSignalSpy &changed,
                QSignalSpy &active,
                QSignalSpy &persistence)
{
    inserted.clear();
    removed.clear();
    moved.clear();
    changed.clear();
    active.clear();
    persistence.clear();
}
}

class BrowserTabModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void createActivateMoveAndCloseHaveExactSignals();
    void activeIndexTracksStructuralChangesExactly();
    void refusesSeventeenthTabWithoutChangingAnything();
    void stableIdsSurviveTabBarStyleReordering();
    void historiesAreIndependentAndSuppressCurrentDuplicates();
    void backForwardBranchingAndHistoryBoundAreExact();
    void recentlyClosedIsASixteenEntryLifo();
    void reopenUsesFreshIdentityAndOnlyRestoresDescriptorState();
    void titleIsBoundedSanitizedPlainText();
    void loneSurrogateTitlesAreRejectedAtomically();
    void identityLifecycleAndPresentationStayResourceFree();
    void accessiblePresentationUsesFixedHostText();
    void validatedRestoreIsAtomicDormantAndNonPersistent();
};

void BrowserTabModelTest::createActivateMoveAndCloseHaveExactSignals()
{
    BrowserTabModel model;
    QCOMPARE(model.count(), 0);
    QVERIFY(model.isEmpty());
    QCOMPARE(model.activeIndex(), -1);

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QStringList order;
    connect(&model, &BrowserTabModel::tabInserted, this,
            [&order](int index, const QString &id) {
                order.append(QStringLiteral("insert:%1:%2").arg(index).arg(id));
            });
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&order](int index, const QString &id) {
                order.append(QStringLiteral("remove:%1:%2").arg(index).arg(id));
            });
    connect(&model, &BrowserTabModel::tabMoved, this,
            [&order](int from, int to) {
                order.append(QStringLiteral("move:%1:%2").arg(from).arg(to));
            });
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&order](int from, int to) {
                order.append(QStringLiteral("active:%1:%2").arg(from).arg(to));
            });
    connect(&model, &BrowserTabModel::persistenceNeeded, this,
            [&order] { order.append(QStringLiteral("persist")); });

    const QString first = model.createTab(BrowserTabKind::Host,
                                          QStringLiteral("New tab"),
                                          QStringLiteral("qbrowser://newtab"));
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$"))
                .match(first).hasMatch());
    QCOMPARE(model.count(), 1);
    QVERIFY(!model.isEmpty());
    QCOMPARE(model.activeIndex(), 0);
    QCOMPARE(model.activeId(), first);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.at(0).at(0).toInt(), 0);
    QCOMPARE(inserted.at(0).at(1).toString(), first);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), -1);
    QCOMPARE(active.at(0).at(1).toInt(), 0);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("insert:0:%1").arg(first),
                                 QStringLiteral("active:-1:0"),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    const QString second = model.createTab(BrowserTabKind::App,
                                           QStringLiteral("Orders"),
                                           QStringLiteral("app://pilot/orders"),
                                           false);
    QVERIFY(!second.isEmpty());
    QCOMPARE(model.activeId(), first);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("insert:1:%1").arg(second),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    QVERIFY(model.activateTab(1));
    QCOMPARE(model.activeId(), second);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 0);
    QCOMPARE(active.at(0).at(1).toInt(), 1);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("active:0:1"),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    QVERIFY(model.activateTab(1));
    QVERIFY(order.isEmpty());
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);

    QVERIFY(model.moveTab(1, 0));
    QCOMPARE(model.activeIndex(), 0);
    QCOMPARE(model.activeId(), second);
    QCOMPARE(moved.count(), 1);
    QCOMPARE(active.count(), 1);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("move:1:0"),
                                 QStringLiteral("active:1:0"),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    QVERIFY(model.moveTab(0, 0));
    QVERIFY(order.isEmpty());

    QVERIFY(model.closeTab(0));
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.activeId(), first);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(0).toInt(), 0);
    QCOMPARE(removed.at(0).at(1).toString(), second);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 0);
    QCOMPARE(active.at(0).at(1).toInt(), 0);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("remove:0:%1").arg(second),
                                 QStringLiteral("active:0:0"),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    QVERIFY(model.closeTab(0));
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.activeIndex(), -1);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 0);
    QCOMPARE(active.at(0).at(1).toInt(), -1);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("remove:0:%1").arg(first),
                                 QStringLiteral("active:0:-1"),
                                 QStringLiteral("persist")}));

    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();
    QVERIFY(!model.activateTab(0));
    QVERIFY(!model.moveTab(0, 0));
    QVERIFY(!model.closeTab(0));
    QVERIFY(order.isEmpty());
}

void BrowserTabModelTest::refusesSeventeenthTabWithoutChangingAnything()
{
    BrowserTabModel model;
    for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
        QVERIFY(!model.createTab(BrowserTabKind::Host,
                                 QStringLiteral("Tab %1").arg(index),
                                 QStringLiteral("qbrowser://newtab"),
                                 false).isEmpty());
    }
    const QVector<BrowserTabSnapshot> before = model.snapshots();
    const int activeBefore = model.activeIndex();
    const int closedBefore = model.recentlyClosedCount();
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.createTab(BrowserTabKind::App,
                            QStringLiteral("Too many"),
                            QStringLiteral("app://pilot/orders")).isEmpty());

    QVERIFY(model.snapshots() == before);
    QCOMPARE(model.activeIndex(), activeBefore);
    QCOMPARE(model.recentlyClosedCount(), closedBefore);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::activeIndexTracksStructuralChangesExactly()
{
    BrowserTabModel closes;
    const QString a = closes.createTab(BrowserTabKind::Host,
                                       QStringLiteral("A"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString b = closes.createTab(BrowserTabKind::Host,
                                       QStringLiteral("B"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString c = closes.createTab(BrowserTabKind::Host,
                                       QStringLiteral("C"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString d = closes.createTab(BrowserTabKind::Host,
                                       QStringLiteral("D"),
                                       QStringLiteral("qbrowser://newtab"), false);
    QVERIFY(!a.isEmpty() && !b.isEmpty() && !c.isEmpty() && !d.isEmpty());
    QVERIFY(closes.activateTab(2));
    QSignalSpy closeActive(&closes, &BrowserTabModel::activeTabChanged);

    QVERIFY(closes.closeTab(0));
    QCOMPARE(closes.activeId(), c);
    QCOMPARE(closes.activeIndex(), 1);
    QCOMPARE(closeActive.count(), 1);
    QCOMPARE(closeActive.at(0).at(0).toInt(), 2);
    QCOMPARE(closeActive.at(0).at(1).toInt(), 1);

    closeActive.clear();
    QVERIFY(closes.closeTab(2));
    QCOMPARE(closes.activeId(), c);
    QCOMPARE(closes.activeIndex(), 1);
    QCOMPARE(closeActive.count(), 0);

    BrowserTabModel moves;
    const QString ma = moves.createTab(BrowserTabKind::Host,
                                       QStringLiteral("A"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString mb = moves.createTab(BrowserTabKind::Host,
                                       QStringLiteral("B"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString mc = moves.createTab(BrowserTabKind::Host,
                                       QStringLiteral("C"),
                                       QStringLiteral("qbrowser://newtab"), false);
    const QString md = moves.createTab(BrowserTabKind::Host,
                                       QStringLiteral("D"),
                                       QStringLiteral("qbrowser://newtab"), false);
    QVERIFY(!ma.isEmpty() && !mb.isEmpty() && !mc.isEmpty() && !md.isEmpty());
    QVERIFY(moves.activateTab(2));
    QSignalSpy moveActive(&moves, &BrowserTabModel::activeTabChanged);

    QVERIFY(moves.moveTab(0, 3));
    QCOMPARE(moves.activeId(), mc);
    QCOMPARE(moves.activeIndex(), 1);
    QCOMPARE(moveActive.count(), 1);
    QCOMPARE(moveActive.at(0).at(0).toInt(), 2);
    QCOMPARE(moveActive.at(0).at(1).toInt(), 1);

    moveActive.clear();
    QVERIFY(moves.moveTab(3, 0));
    QCOMPARE(moves.activeId(), mc);
    QCOMPARE(moves.activeIndex(), 2);
    QCOMPARE(moveActive.count(), 1);
    QCOMPARE(moveActive.at(0).at(0).toInt(), 1);
    QCOMPARE(moveActive.at(0).at(1).toInt(), 2);

    moveActive.clear();
    QVERIFY(moves.moveTab(0, 1));
    QCOMPARE(moves.activeId(), mc);
    QCOMPARE(moves.activeIndex(), 2);
    QCOMPARE(moveActive.count(), 0);

    BrowserTabModel closesLastActive;
    const QString first = closesLastActive.createTab(
        BrowserTabKind::Host, QStringLiteral("First"),
        QStringLiteral("qbrowser://newtab"), false);
    const QString middle = closesLastActive.createTab(
        BrowserTabKind::Host, QStringLiteral("Middle"),
        QStringLiteral("qbrowser://newtab"), false);
    const QString last = closesLastActive.createTab(
        BrowserTabKind::Host, QStringLiteral("Last"),
        QStringLiteral("qbrowser://newtab"), false);
    QVERIFY(!first.isEmpty() && !middle.isEmpty() && !last.isEmpty());
    QVERIFY(closesLastActive.activateTab(2));
    QSignalSpy lastActive(&closesLastActive,
                          &BrowserTabModel::activeTabChanged);

    QVERIFY(closesLastActive.closeTab(2));
    QCOMPARE(closesLastActive.activeId(), middle);
    QCOMPARE(closesLastActive.activeIndex(), 1);
    QCOMPARE(lastActive.count(), 1);
    QCOMPARE(lastActive.at(0).at(0).toInt(), 2);
    QCOMPARE(lastActive.at(0).at(1).toInt(), 1);
}

void BrowserTabModelTest::stableIdsSurviveTabBarStyleReordering()
{
    BrowserTabModel model;
    const QString first = model.createTab(BrowserTabKind::Host,
                                          QStringLiteral("First"),
                                          QStringLiteral("qbrowser://newtab"),
                                          false);
    const QString second = model.createTab(BrowserTabKind::App,
                                           QStringLiteral("Second"),
                                           QStringLiteral("app://pilot/orders"),
                                           false);
    const QString third = model.createTab(BrowserTabKind::Web,
                                          QStringLiteral("Third"),
                                          QStringLiteral("app://pilot/help"),
                                          false);

    QVERIFY(model.moveTab(0, 2));
    QCOMPARE(model.snapshotAt(0).id, second);
    QCOMPARE(model.snapshotAt(1).id, third);
    QCOMPARE(model.snapshotAt(2).id, first);
    QCOMPARE(model.indexOfId(first), 2);
    QCOMPARE(model.indexOfId(second), 0);
    QCOMPARE(model.indexOfId(third), 1);
    QCOMPARE(model.indexOfId(QStringLiteral("missing")), -1);
}

void BrowserTabModelTest::historiesAreIndependentAndSuppressCurrentDuplicates()
{
    BrowserTabModel model;
    const QString first = model.createTab(BrowserTabKind::App,
                                          QStringLiteral("First"),
                                          QStringLiteral("app://pilot/orders"));
    const QString second = model.createTab(BrowserTabKind::App,
                                           QStringLiteral("Second"),
                                           QStringLiteral("app://pilot/settings"));
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.navigateTab(first, QStringLiteral("app://pilot/orders/42")));
    QCOMPARE(model.snapshotAt(model.indexOfId(first)).history,
             QStringList({QStringLiteral("app://pilot/orders"),
                          QStringLiteral("app://pilot/orders/42")}));
    QCOMPARE(model.snapshotAt(model.indexOfId(second)).history,
             QStringList({QStringLiteral("app://pilot/settings")}));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toInt(), model.indexOfId(first));
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(first, QStringLiteral("app://pilot/orders/42")));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
    QCOMPARE(model.snapshotAt(model.indexOfId(first)).history.size(), 2);

    QVERIFY(!model.navigateTab(QStringLiteral("missing"),
                               QStringLiteral("app://pilot/orders")));
    QVERIFY(!model.navigateTab(first, QString()));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::backForwardBranchingAndHistoryBoundAreExact()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::App,
                                       QStringLiteral("Orders"),
                                       QStringLiteral("app://pilot/orders/0"));
    QVERIFY(model.navigateTab(id, QStringLiteral("app://pilot/orders/1")));
    QVERIFY(model.navigateTab(id, QStringLiteral("app://pilot/orders/2")));
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.canGoBack(id));
    QVERIFY(!model.canGoForward(id));
    QVERIFY(model.goBack(id));
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("app://pilot/orders/1"));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, QStringLiteral("app://pilot/orders/1")));
    QCOMPARE(model.snapshotAt(0).history,
             QStringList({QStringLiteral("app://pilot/orders/0"),
                          QStringLiteral("app://pilot/orders/1"),
                          QStringLiteral("app://pilot/orders/2")}));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QVERIFY(model.canGoForward(id));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);

    QVERIFY(model.goBack(id));
    QVERIFY(!model.goBack(id));
    QCOMPARE(model.snapshotAt(0).historyIndex, 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.goForward(id));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, QStringLiteral("app://pilot/orders/branch")));
    QCOMPARE(model.snapshotAt(0).history,
             QStringList({QStringLiteral("app://pilot/orders/0"),
                          QStringLiteral("app://pilot/orders/1"),
                          QStringLiteral("app://pilot/orders/branch")}));
    QCOMPARE(model.snapshotAt(0).historyIndex, 2);
    QVERIFY(!model.canGoForward(id));
    QVERIFY(!model.goForward(id));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    BrowserTabModel bounded;
    const QString boundedId = bounded.createTab(
        BrowserTabKind::App, QStringLiteral("Bounded"),
        QStringLiteral("app://pilot/orders/0"));
    for (int index = 1; index <= 300; ++index) {
        QVERIFY(bounded.navigateTab(
            boundedId, QStringLiteral("app://pilot/orders/%1").arg(index)));
    }
    const BrowserTabSnapshot boundedSnapshot = bounded.snapshotAt(0);
    QCOMPARE(boundedSnapshot.history.size(), BrowserTabModel::MaxHistoryEntries);
    QCOMPARE(boundedSnapshot.historyIndex,
             BrowserTabModel::MaxHistoryEntries - 1);
    QCOMPARE(boundedSnapshot.history.first(),
             QStringLiteral("app://pilot/orders/45"));
    QCOMPARE(boundedSnapshot.history.last(),
             QStringLiteral("app://pilot/orders/300"));
}

void BrowserTabModelTest::recentlyClosedIsASixteenEntryLifo()
{
    BrowserTabModel model;
    QSet<QString> oldIds;
    for (int index = 0; index <= BrowserTabModel::MaxRecentlyClosed; ++index) {
        const QString id = model.createTab(
            BrowserTabKind::App,
            QStringLiteral("Closed %1").arg(index),
            QStringLiteral("app://pilot/orders/%1").arg(index));
        QVERIFY(!id.isEmpty());
        oldIds.insert(id);
        QVERIFY(model.closeTab(model.indexOfId(id)));
    }
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.recentlyClosedCount(), BrowserTabModel::MaxRecentlyClosed);

    for (int expected = BrowserTabModel::MaxRecentlyClosed;
         expected >= 1; --expected) {
        const QString reopened = model.reopenMostRecentlyClosed();
        QVERIFY(!reopened.isEmpty());
        QVERIFY(!oldIds.contains(reopened));
        QCOMPARE(model.snapshotAt(model.indexOfId(reopened)).title,
                 QStringLiteral("Closed %1").arg(expected));
    }
    QCOMPARE(model.count(), BrowserTabModel::MaxOpenTabs);
    QCOMPARE(model.recentlyClosedCount(), 0);

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QVERIFY(model.reopenMostRecentlyClosed().isEmpty());
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::reopenUsesFreshIdentityAndOnlyRestoresDescriptorState()
{
    BrowserTabModel model;
    const QString oldId = model.createTab(BrowserTabKind::App,
                                          QStringLiteral("Order details"),
                                          QStringLiteral("app://pilot/orders"));
    QVERIFY(model.navigateTab(oldId, QStringLiteral("app://pilot/orders/42")));
    QVERIFY(model.navigateTab(oldId, QStringLiteral("app://pilot/orders/43")));
    QVERIFY(model.goBack(oldId));
    QVERIFY(model.setLifecycle(oldId, BrowserTabLifecycle::Active));
    QVERIFY(model.setLoadState(oldId, true, 73));
    QVERIFY(model.setVisualState(oldId, BrowserVisualState::Crashed));
    const BrowserTabSnapshot beforeClose = model.snapshotAt(0);

    QVERIFY(model.closeTab(0));
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QStringList order;
    connect(&model, &BrowserTabModel::tabInserted, this,
            [&order](int index, const QString &id) {
                order.append(QStringLiteral("insert:%1:%2").arg(index).arg(id));
            });
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&order](int from, int to) {
                order.append(QStringLiteral("active:%1:%2").arg(from).arg(to));
            });
    connect(&model, &BrowserTabModel::persistenceNeeded, this,
            [&order] { order.append(QStringLiteral("persist")); });
    const QString reopened = model.reopenMostRecentlyClosed();
    QVERIFY(!reopened.isEmpty());
    QVERIFY(reopened != oldId);
    const BrowserTabSnapshot restored = model.snapshotAt(0);
    QCOMPARE(restored.kind, beforeClose.kind);
    QCOMPARE(restored.title, beforeClose.title);
    QCOMPARE(restored.address, beforeClose.address);
    QCOMPARE(restored.history, beforeClose.history);
    QCOMPARE(restored.historyIndex, beforeClose.historyIndex);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    const BrowserTabPresentation presentation = model.presentationAt(0);
    QCOMPARE(presentation.loading, false);
    QCOMPARE(presentation.progress, 0);
    QCOMPARE(presentation.contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(presentation.visualState, BrowserVisualState::Normal);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.at(0).at(0).toInt(), 0);
    QCOMPARE(inserted.at(0).at(1).toString(), reopened);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), -1);
    QCOMPARE(active.at(0).at(1).toInt(), 0);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("insert:0:%1").arg(reopened),
                                 QStringLiteral("active:-1:0"),
                                 QStringLiteral("persist")}));
}

void BrowserTabModelTest::titleIsBoundedSanitizedPlainText()
{
    BrowserTabModel model;
    const QString raw = QStringLiteral("<b>Hello</b>\u0001\n\u202e\u2066 world");
    const QString id = model.createTab(BrowserTabKind::Host, raw,
                                       QStringLiteral("qbrowser://newtab"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(model.snapshotAt(0).title,
             QStringLiteral("<b>Hello</b> world"));

    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    const QString splitsAtBoundary = QString(255, QLatin1Char('a')) + emoji
        + QStringLiteral("tail");
    QVERIFY(model.setTitle(id, splitsAtBoundary));
    QCOMPARE(model.snapshotAt(0).title, QString(255, QLatin1Char('a')));
    QCOMPARE(model.snapshotAt(0).title.size(), 255);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    const QString keepsPair = QString(254, QLatin1Char('b')) + emoji
        + QStringLiteral("tail");
    QVERIFY(model.setTitle(id, keepsPair));
    QCOMPARE(model.snapshotAt(0).title,
             QString(254, QLatin1Char('b')) + emoji);
    QCOMPARE(model.snapshotAt(0).title.size(),
             BrowserTabModel::MaxTitleCodeUnits);
    QVERIFY(!model.snapshotAt(0).title.back().isHighSurrogate());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.setTitle(id, model.snapshotAt(0).title));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::loneSurrogateTitlesAreRejectedAtomically()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::Host,
                                       QStringLiteral("Safe"),
                                       QStringLiteral("qbrowser://newtab"));
    const BrowserTabSnapshot before = model.snapshotAt(0);
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    const QString loneHigh(1, QChar(0xd800));
    const QString loneLow(1, QChar(0xdc00));

    QVERIFY(!model.setTitle(id, loneHigh));
    QVERIFY(!model.setTitle(id, loneLow));
    QVERIFY(model.snapshotAt(0) == before);
    QVERIFY(model.createTab(BrowserTabKind::Host, loneHigh,
                            QStringLiteral("qbrowser://newtab")).isEmpty());
    QCOMPARE(model.count(), 1);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::identityLifecycleAndPresentationStayResourceFree()
{
    BrowserTabModel model;
    const QString host = model.createTab(BrowserTabKind::Host,
                                         QStringLiteral("Host"),
                                         QStringLiteral("qbrowser://newtab"));
    const QString app = model.createTab(BrowserTabKind::App,
                                        QStringLiteral("App"),
                                        QStringLiteral("app://pilot/orders"));
    const QString web = model.createTab(BrowserTabKind::Web,
                                        QStringLiteral("Web"),
                                        QStringLiteral("app://pilot/help"));
    const QString error = model.createTab(BrowserTabKind::TrustedError,
                                          QStringLiteral("Error"),
                                          QStringLiteral("qbrowser://newtab"));
    QCOMPARE(model.presentationAt(model.indexOfId(host)).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(model.presentationAt(model.indexOfId(app)).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(model.presentationAt(model.indexOfId(web)).contentIdentity,
             BrowserContentIdentity::RestrictedWeb);
    QCOMPARE(model.presentationAt(model.indexOfId(error)).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(model.presentationAt(model.indexOfId(error)).visualState,
             BrowserVisualState::TrustedError);

    const QVector<BrowserTabSnapshot> persistentBefore = model.snapshots();
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QVERIFY(model.setLifecycle(app, BrowserTabLifecycle::Starting));
    QVERIFY(model.setLifecycle(app, BrowserTabLifecycle::Loading));
    QVERIFY(model.setLoadState(app, true, -50));
    QCOMPARE(model.presentationAt(model.indexOfId(app)).progress, 0);
    QVERIFY(model.setLoadState(app, true, 150));
    QCOMPARE(model.presentationAt(model.indexOfId(app)).progress, 100);
    QVERIFY(model.setVisualState(app, BrowserVisualState::Recovering));
    QVERIFY(model.setVisualState(app, BrowserVisualState::Crashed));
    QCOMPARE(model.lifecycleAt(model.indexOfId(app)),
             BrowserTabLifecycle::Loading);
    QCOMPARE(model.presentationAt(model.indexOfId(app)).visualState,
             BrowserVisualState::Crashed);
    QCOMPARE(changed.count(), 6);
    QCOMPARE(persistence.count(), 0);
    QVERIFY(model.snapshots() == persistentBefore);

    changed.clear();
    QVERIFY(model.setLifecycle(app, BrowserTabLifecycle::Loading));
    QVERIFY(model.setLoadState(app, true, 100));
    QVERIFY(model.setVisualState(app, BrowserVisualState::Crashed));
    QCOMPARE(changed.count(), 0);

    QVERIFY(model.setTitle(app, QStringLiteral("<i>Untrusted label</i>")));
    QCOMPARE(model.presentationAt(model.indexOfId(app)).contentIdentity,
             BrowserContentIdentity::SignedApplication);
}

void BrowserTabModelTest::accessiblePresentationUsesFixedHostText()
{
    BrowserTabModel model;
    const QString host = model.createTab(BrowserTabKind::Host,
                                         QStringLiteral("<b>New tab</b>"),
                                         QStringLiteral("qbrowser://newtab"));
    const QString app = model.createTab(BrowserTabKind::App,
                                        QStringLiteral("Orders"),
                                        QStringLiteral("app://pilot/orders"));
    const QString web = model.createTab(BrowserTabKind::Web,
                                        QStringLiteral("Help"),
                                        QStringLiteral("app://pilot/help"));
    const QString error = model.createTab(BrowserTabKind::TrustedError,
                                          QStringLiteral("Unavailable"),
                                          QStringLiteral("qbrowser://newtab"));

    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(host)).name,
             QStringLiteral("<b>New tab</b>"));
    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(host)).description,
             QStringLiteral("Q-Browser, Ready"));
    QVERIFY(model.setLoadState(app, true, 42));
    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(app)).description,
             QStringLiteral("Signed application, Loading 42%"));
    QVERIFY(model.setVisualState(app, BrowserVisualState::Recovering));
    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(app)).description,
             QStringLiteral("Signed application, Recovering"));
    QVERIFY(model.setVisualState(web, BrowserVisualState::Crashed));
    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(web)).description,
             QStringLiteral("Restricted web, Crashed"));
    QCOMPARE(model.accessiblePresentationAt(model.indexOfId(error)).description,
             QStringLiteral("Q-Browser, Error"));
}

void BrowserTabModelTest::validatedRestoreIsAtomicDormantAndNonPersistent()
{
    BrowserTabModel model;
    const QString stale = model.createTab(BrowserTabKind::Host,
                                          QStringLiteral("Stale closed"),
                                          QStringLiteral("qbrowser://newtab"));
    QVERIFY(model.closeTab(model.indexOfId(stale)));
    const QString old = model.createTab(BrowserTabKind::Host,
                                        QStringLiteral("Old"),
                                        QStringLiteral("qbrowser://newtab"));
    QVERIFY(model.setLifecycle(old, BrowserTabLifecycle::Active));
    QVERIFY(model.setLoadState(old, true, 90));

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QStringList order;
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&order](int index, const QString &id) {
                order.append(QStringLiteral("remove:%1:%2").arg(index).arg(id));
            });
    connect(&model, &BrowserTabModel::tabInserted, this,
            [&order](int index, const QString &id) {
                order.append(QStringLiteral("insert:%1:%2").arg(index).arg(id));
            });
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&order](int from, int to) {
                order.append(QStringLiteral("active:%1:%2").arg(from).arg(to));
            });

    const BrowserTabSnapshot app = restoredTab(
        10, BrowserTabKind::App, QStringLiteral("Restored app"),
        {QStringLiteral("app://pilot/orders"),
         QStringLiteral("app://pilot/orders/42")}, 1);
    const BrowserTabSnapshot host = restoredTab(
        11, BrowserTabKind::Host, QStringLiteral("Restored host"),
        {QStringLiteral("qbrowser://newtab")}, 0);
    QVERIFY(model.replaceFromValidatedSnapshot({app, host}, 1));
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(model.recentlyClosedCount(), 0);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.lifecycleAt(1), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.presentationAt(0).loading, false);
    QCOMPARE(model.presentationAt(0).progress, 0);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(model.presentationAt(0).visualState, BrowserVisualState::Normal);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(persistence.count(), 0);
    QCOMPARE(order, QStringList({QStringLiteral("remove:0:%1").arg(old),
                                 QStringLiteral("insert:0:%1").arg(app.id),
                                 QStringLiteral("insert:1:%1").arg(host.id),
                                 QStringLiteral("active:0:1")}));

    const QVector<BrowserTabSnapshot> before = model.snapshots();
    const int activeBefore = model.activeIndex();
    clearSpies(inserted, removed, moved, changed, active, persistence);
    order.clear();

    QVector<BrowserTabSnapshot> tooMany;
    for (int index = 0; index <= BrowserTabModel::MaxOpenTabs; ++index) {
        tooMany.append(restoredTab(
            100 + index, BrowserTabKind::Host,
            QStringLiteral("Too many %1").arg(index),
            {QStringLiteral("qbrowser://newtab")}, 0));
    }
    QVERIFY(!model.replaceFromValidatedSnapshot(tooMany, 0));
    QVERIFY(!model.replaceFromValidatedSnapshot({app, app}, 0));
    QVERIFY(!model.replaceFromValidatedSnapshot({app, host}, 2));
    BrowserTabSnapshot invalidId = app;
    invalidId.id = QStringLiteral("ABC");
    QVERIFY(!model.replaceFromValidatedSnapshot({invalidId}, 0));
    BrowserTabSnapshot tooMuchHistory = app;
    tooMuchHistory.history.clear();
    for (int index = 0; index <= BrowserTabModel::MaxHistoryEntries; ++index) {
        tooMuchHistory.history.append(
            QStringLiteral("app://pilot/orders/%1").arg(index));
    }
    tooMuchHistory.historyIndex = tooMuchHistory.history.size() - 1;
    tooMuchHistory.address = tooMuchHistory.history.last();
    QVERIFY(!model.replaceFromValidatedSnapshot({tooMuchHistory}, 0));
    QVERIFY(model.snapshots() == before);
    QCOMPARE(model.activeIndex(), activeBefore);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
    QVERIFY(order.isEmpty());

    QVERIFY(model.replaceFromValidatedSnapshot({}, -1));
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.activeIndex(), -1);
    QCOMPARE(removed.count(), 2);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 1);
    QCOMPARE(active.at(0).at(1).toInt(), -1);
    QCOMPARE(persistence.count(), 0);

    inserted.clear();
    removed.clear();
    changed.clear();
    active.clear();
    order.clear();
    QVERIFY(model.replaceFromValidatedSnapshot({}, -1));
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
    QVERIFY(order.isEmpty());
}

QTEST_APPLESS_MAIN(BrowserTabModelTest)

#include "tst_browser_tab_model.moc"
