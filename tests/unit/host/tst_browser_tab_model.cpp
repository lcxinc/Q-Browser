#include "BrowserTabModel.h"

#include <QRegularExpression>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <optional>
#include <memory>
#include <type_traits>
#include <utility>

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
template <typename T>
concept HasContentIdentitySetter = requires(
    T value, BrowserContentIdentity identity) {
    value.setContentIdentity(identity);
};

static_assert(std::is_copy_constructible_v<BrowserTabSnapshot>);
static_assert(std::is_same_v<
              decltype(std::declval<const BrowserTabModel &>().snapshotAt(0)),
              BrowserTabSnapshot>);
static_assert(!HasPid<BrowserTabSnapshot>);
static_assert(!HasHwnd<BrowserTabSnapshot>);
static_assert(!HasNonce<BrowserTabSnapshot>);
static_assert(!HasGeneration<BrowserTabSnapshot>);
static_assert(!HasRequest<BrowserTabSnapshot>);
static_assert(!HasCapability<BrowserTabSnapshot>);
static_assert(!HasCookie<BrowserTabSnapshot>);
static_assert(!HasGrant<BrowserTabSnapshot>);
static_assert(!HasContentIdentitySetter<BrowserTabModel>);

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
    void snapshotAtReturnsAnOwnedValueAcrossStructuralChanges();
    void stableIdsSurviveTabBarStyleReordering();
    void historiesAreIndependentAndSuppressCurrentDuplicates();
    void validatedRouteNavigationChangesKindAndIdentity();
    void backAndForwardCommitValidatedTargetKindAtomically();
    void invalidValidatedKindOperationsAreAtomicAndSilent();
    void backForwardBranchingAndHistoryBoundAreExact();
    void recentlyClosedIsASixteenEntryLifo();
    void reopenUsesFreshIdentityAndOnlyRestoresDescriptorState();
    void titleIsBoundedSanitizedPlainText();
    void titleSanitizerValidatesTheWholeLargeInput();
    void loneSurrogateTitlesAreRejectedAtomically();
    void identityLifecycleAndPresentationStayResourceFree();
    void accessiblePresentationUsesFixedHostText();
    void directSignalsRejectReentrantMutators();
    void tabChangedObserversKeepAValidIndexDuringNotification();
    void restorePublishesSelfConsistentGranularChanges();
    void restorePublishesCompletionWhenActiveIdentityIsUnchanged();
    void restoreToEmptyIsInactiveAndNoOpRestoreIsSilent();
    void restoreRejectsNestedMutations();
    void restoreOwnsItsInputBeforeEmittingSignals();
    void restoreSignalCanDestroyModel();
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

void BrowserTabModelTest::snapshotAtReturnsAnOwnedValueAcrossStructuralChanges()
{
    BrowserTabModel model;
    const QString originalId = model.createTab(
        BrowserTabKind::Host, QStringLiteral("Owned snapshot"),
        QStringLiteral("qbrowser://owned-snapshot"));
    QVERIFY(!originalId.isEmpty());
    const BrowserTabSnapshot &ownedSnapshot = model.snapshotAt(0);
    const BrowserTabSnapshot expected = ownedSnapshot;

    QVERIFY(model.closeTab(0));
    for (int index = 0; index < BrowserTabModel::MaxOpenTabs; ++index) {
        QVERIFY(!model.createTab(
                     BrowserTabKind::Host,
                     QStringLiteral("Replacement %1").arg(index),
                     QStringLiteral("qbrowser://replacement/%1").arg(index),
                     false)
                     .isEmpty());
    }

    QVERIFY(ownedSnapshot == expected);
    QCOMPARE(ownedSnapshot.id, originalId);
    QCOMPARE(ownedSnapshot.title, QStringLiteral("Owned snapshot"));
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

    QVERIFY(model.navigateTab(first, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/42")));
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
    QVERIFY(model.navigateTab(first, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/42")));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
    QCOMPARE(model.snapshotAt(model.indexOfId(first)).history.size(), 2);

    QVERIFY(!model.navigateTab(QStringLiteral("missing"), BrowserTabKind::App,
                               QStringLiteral("app://pilot/orders")));
    QVERIFY(!model.navigateTab(first, BrowserTabKind::App, QString()));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::backForwardBranchingAndHistoryBoundAreExact()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::App,
                                       QStringLiteral("Orders"),
                                       QStringLiteral("app://pilot/orders/0"));
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/1")));
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/2")));
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.canGoBack(id));
    QVERIFY(!model.canGoForward(id));
    QVERIFY(model.goBack(id, BrowserTabKind::App));
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("app://pilot/orders/1"));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/1")));
    QCOMPARE(model.snapshotAt(0).history,
             QStringList({QStringLiteral("app://pilot/orders/0"),
                          QStringLiteral("app://pilot/orders/1"),
                          QStringLiteral("app://pilot/orders/2")}));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QVERIFY(model.canGoForward(id));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);

    QVERIFY(model.goBack(id, BrowserTabKind::App));
    QVERIFY(!model.goBack(id, BrowserTabKind::App));
    QCOMPARE(model.snapshotAt(0).historyIndex, 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.goForward(id, BrowserTabKind::App));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/branch")));
    QCOMPARE(model.snapshotAt(0).history,
             QStringList({QStringLiteral("app://pilot/orders/0"),
                          QStringLiteral("app://pilot/orders/1"),
                          QStringLiteral("app://pilot/orders/branch")}));
    QCOMPARE(model.snapshotAt(0).historyIndex, 2);
    QVERIFY(!model.canGoForward(id));
    QVERIFY(!model.goForward(id, BrowserTabKind::App));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    BrowserTabModel bounded;
    const QString boundedId = bounded.createTab(
        BrowserTabKind::App, QStringLiteral("Bounded"),
        QStringLiteral("app://pilot/orders/0"));
    for (int index = 1; index <= 300; ++index) {
        QVERIFY(bounded.navigateTab(
            boundedId, BrowserTabKind::App,
            QStringLiteral("app://pilot/orders/%1").arg(index)));
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

void BrowserTabModelTest::validatedRouteNavigationChangesKindAndIdentity()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::Host,
                                       QStringLiteral("New tab"),
                                       QStringLiteral("qbrowser://newtab"));

    // The caller has already resolved this logical address to the Web engine.
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.setLifecycle(id, BrowserTabLifecycle::Active));
    QVERIFY(model.setLoadState(id, true, 67));
    QVERIFY(model.setVisualState(id, BrowserVisualState::Crashed));
    changed.clear();

    QVERIFY(model.navigateTab(id, BrowserTabKind::Web,
                              QStringLiteral("app://pilot/help")));

    QCOMPARE(model.snapshotAt(0).id, id);
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::Web);
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("app://pilot/help"));
    QCOMPARE(model.snapshotAt(0).history,
             QStringList({QStringLiteral("qbrowser://newtab"),
                          QStringLiteral("app://pilot/help")}));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::RestrictedWeb);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.presentationAt(0).loading, false);
    QCOMPARE(model.presentationAt(0).progress, 0);
    QCOMPARE(model.presentationAt(0).visualState, BrowserVisualState::Normal);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toInt(), 0);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.setLifecycle(id, BrowserTabLifecycle::Loading));
    QVERIFY(model.setLoadState(id, true, 35));
    QVERIFY(model.setVisualState(id, BrowserVisualState::Recovering));
    changed.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders")));
    QCOMPARE(model.snapshotAt(0).id, id);
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::App);
    QCOMPARE(model.snapshotAt(0).address,
             QStringLiteral("app://pilot/orders"));
    QCOMPARE(model.snapshotAt(0).historyIndex, 2);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.presentationAt(0).loading, false);
    QCOMPARE(model.presentationAt(0).progress, 0);
    QCOMPARE(model.presentationAt(0).visualState, BrowserVisualState::Normal);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    const int historySize = static_cast<int>(model.snapshotAt(0).history.size());
    QVERIFY(model.setLifecycle(id, BrowserTabLifecycle::Active));
    QVERIFY(model.setLoadState(id, true, 81));
    QVERIFY(model.setVisualState(id, BrowserVisualState::Crashed));
    changed.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::TrustedError,
                              QStringLiteral("app://pilot/orders")));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::TrustedError);
    QCOMPARE(model.snapshotAt(0).history.size(), historySize);
    QCOMPARE(model.snapshotAt(0).historyIndex, 2);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.presentationAt(0).loading, false);
    QCOMPARE(model.presentationAt(0).progress, 0);
    QCOMPARE(model.presentationAt(0).visualState,
             BrowserVisualState::TrustedError);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders")));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::App);
    QCOMPARE(model.snapshotAt(0).history.size(), historySize);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(model.presentationAt(0).visualState, BrowserVisualState::Normal);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders")));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
}

void BrowserTabModelTest::backAndForwardCommitValidatedTargetKindAtomically()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::Host,
                                       QStringLiteral("Routes"),
                                       QStringLiteral("qbrowser://newtab"));
    QVERIFY(model.navigateTab(id, BrowserTabKind::Web,
                              QStringLiteral("app://pilot/help")));
    QVERIFY(model.navigateTab(id, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders")));
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.setLifecycle(id, BrowserTabLifecycle::Active));
    QVERIFY(model.setLoadState(id, true, 91));
    QVERIFY(model.setVisualState(id, BrowserVisualState::Crashed));
    changed.clear();
    QVERIFY(model.goBack(id, BrowserTabKind::Web));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("app://pilot/help"));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::Web);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::RestrictedWeb);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.presentationAt(0).loading, false);
    QCOMPARE(model.presentationAt(0).progress, 0);
    QCOMPARE(model.presentationAt(0).visualState, BrowserVisualState::Normal);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.goBack(id, BrowserTabKind::Host));
    QCOMPARE(model.snapshotAt(0).historyIndex, 0);
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("qbrowser://newtab"));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::Host);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::QBrowser);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.goForward(id, BrowserTabKind::Web));
    QCOMPARE(model.snapshotAt(0).historyIndex, 1);
    QCOMPARE(model.snapshotAt(0).address, QStringLiteral("app://pilot/help"));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::Web);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::RestrictedWeb);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    changed.clear();
    persistence.clear();
    QVERIFY(model.goForward(id, BrowserTabKind::App));
    QCOMPARE(model.snapshotAt(0).historyIndex, 2);
    QCOMPARE(model.snapshotAt(0).address,
             QStringLiteral("app://pilot/orders"));
    QCOMPARE(model.snapshotAt(0).kind, BrowserTabKind::App);
    QCOMPARE(model.presentationAt(0).contentIdentity,
             BrowserContentIdentity::SignedApplication);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
}

void BrowserTabModelTest::invalidValidatedKindOperationsAreAtomicAndSilent()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::Host,
                                       QStringLiteral("Routes"),
                                       QStringLiteral("qbrowser://newtab"));
    QVERIFY(model.navigateTab(id, BrowserTabKind::Web,
                              QStringLiteral("app://pilot/help")));
    const auto invalidKind = static_cast<BrowserTabKind>(99);
    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    const BrowserTabSnapshot before = model.snapshotAt(0);
    const BrowserTabLifecycle lifecycleBefore = model.lifecycleAt(0);
    const BrowserTabPresentation presentationBefore = model.presentationAt(0);

    QVERIFY(!model.navigateTab(id, invalidKind,
                               QStringLiteral("app://pilot/orders")));
    QVERIFY(!model.navigateTab(QStringLiteral("missing"), BrowserTabKind::App,
                               QStringLiteral("app://pilot/orders")));
    QVERIFY(!model.navigateTab(id, BrowserTabKind::App, QString()));
    QVERIFY(!model.goBack(id, invalidKind));
    QVERIFY(!model.goBack(QStringLiteral("missing"), BrowserTabKind::Host));
    QVERIFY(!model.goForward(QStringLiteral("missing"), BrowserTabKind::Web));
    QVERIFY(model.snapshotAt(0) == before);
    QCOMPARE(model.lifecycleAt(0), lifecycleBefore);
    QVERIFY(model.presentationAt(0) == presentationBefore);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);

    QVERIFY(model.goBack(id, BrowserTabKind::Host));
    changed.clear();
    persistence.clear();
    const BrowserTabSnapshot atStart = model.snapshotAt(0);
    const BrowserTabPresentation startPresentation = model.presentationAt(0);
    QVERIFY(!model.goBack(id, BrowserTabKind::App));
    QVERIFY(!model.goForward(id, invalidKind));

    QVERIFY(model.snapshotAt(0) == atStart);
    QVERIFY(model.presentationAt(0) == startPresentation);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);

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

    const QString retainedClosedId = model.snapshotAt(0).id;
    QVERIFY(model.closeTab(0));
    QCOMPARE(model.count(), BrowserTabModel::MaxOpenTabs - 1);
    QCOMPARE(model.recentlyClosedCount(), 1);
    const QString fillId = model.createTab(
        BrowserTabKind::Host, QStringLiteral("Fill to capacity"),
        QStringLiteral("qbrowser://fill-to-capacity"), false);
    QVERIFY(!fillId.isEmpty());
    QCOMPARE(model.count(), BrowserTabModel::MaxOpenTabs);
    QCOMPARE(model.recentlyClosedCount(), 1);
    const QVector<BrowserTabSnapshot> beforeFailedReopen = model.snapshots();
    const int activeBeforeFailedReopen = model.activeIndex();

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QVERIFY(model.reopenMostRecentlyClosed().isEmpty());
    QVERIFY(model.reopenMostRecentlyClosed().isEmpty());
    QCOMPARE(model.count(), BrowserTabModel::MaxOpenTabs);
    QVERIFY(model.snapshots() == beforeFailedReopen);
    QCOMPARE(model.activeIndex(), activeBeforeFailedReopen);
    QCOMPARE(model.recentlyClosedCount(), 1);
    QVERIFY(model.indexOfId(retainedClosedId) == -1);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::reopenUsesFreshIdentityAndOnlyRestoresDescriptorState()
{
    BrowserTabModel model;
    const QString oldId = model.createTab(BrowserTabKind::App,
                                          QStringLiteral("Order details"),
                                          QStringLiteral("app://pilot/orders"));
    QVERIFY(model.navigateTab(oldId, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/42")));
    QVERIFY(model.navigateTab(oldId, BrowserTabKind::App,
                              QStringLiteral("app://pilot/orders/43")));
    QVERIFY(model.goBack(oldId, BrowserTabKind::App));
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
    const QString raw = QStringLiteral("<b>A&B</b>\u0001\n\u202e\u2066");
    const std::optional<QString> canonical = BrowserTabModel::canonicalTitle(raw);
    QVERIFY(canonical.has_value());
    QCOMPARE(*canonical, QStringLiteral("<b>A&B</b>"));
    const QString id = model.createTab(BrowserTabKind::Host, raw,
                                       QStringLiteral("qbrowser://newtab"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(model.snapshotAt(0).title, *canonical);

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

void BrowserTabModelTest::titleSanitizerValidatesTheWholeLargeInput()
{
    BrowserTabModel model;
    const QString id = model.createTab(BrowserTabKind::Host,
                                       QStringLiteral("Safe"),
                                       QStringLiteral("qbrowser://newtab"));
    QVERIFY(!id.isEmpty());
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    const QString maximumInput(BrowserTabModel::MaxRawTitleCodeUnits,
                               QLatin1Char('x'));
    const std::optional<QString> maximumCanonical =
        BrowserTabModel::canonicalTitle(maximumInput);
    QVERIFY(maximumCanonical.has_value());
    QCOMPARE(maximumCanonical->size(), BrowserTabModel::MaxTitleCodeUnits);
    QVERIFY(model.setTitle(id, maximumInput));
    QCOMPARE(model.snapshotAt(0).title,
             QString(BrowserTabModel::MaxTitleCodeUnits,
                     QLatin1Char('x')));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(persistence.count(), 1);

    const BrowserTabSnapshot beforeInvalid = model.snapshotAt(0);
    changed.clear();
    persistence.clear();
    QString malformedAtBound(BrowserTabModel::MaxRawTitleCodeUnits,
                             QLatin1Char('y'));
    malformedAtBound[malformedAtBound.size() - 1] = QChar(0xd800);
    QVERIFY(!BrowserTabModel::canonicalTitle(malformedAtBound).has_value());
    QVERIFY(!model.setTitle(id, malformedAtBound));
    QVERIFY(model.snapshotAt(0) == beforeInvalid);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(persistence.count(), 0);

    const QString oversized(BrowserTabModel::MaxRawTitleCodeUnits + 1,
                            QLatin1Char('z'));
    QVERIFY(!BrowserTabModel::canonicalTitle(oversized).has_value());
    QVERIFY(!model.setTitle(id, oversized));
    QVERIFY(model.snapshotAt(0) == beforeInvalid);
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

void BrowserTabModelTest::directSignalsRejectReentrantMutators()
{
    enum SignalCase
    {
        Inserted,
        Moved,
        Removed,
        Changed,
        ActiveChanged,
        Persistence,
        SignalCaseCount,
    };

    for (int signalCase = Inserted; signalCase < SignalCaseCount;
         ++signalCase) {
        BrowserTabModel model;
        const QString closed = model.createTab(
            BrowserTabKind::Host, QStringLiteral("Closed"),
            QStringLiteral("qbrowser://closed"));
        QVERIFY(!closed.isEmpty());
        QVERIFY(model.closeTab(0));

        const QString anchor = model.createTab(
            BrowserTabKind::Host, QStringLiteral("Anchor"),
            QStringLiteral("qbrowser://anchor"));
        QVERIFY(!anchor.isEmpty());
        QVERIFY(model.navigateTab(anchor, BrowserTabKind::Web,
                                  QStringLiteral("app://pilot/help")));
        QVERIFY(model.navigateTab(anchor, BrowserTabKind::App,
                                  QStringLiteral("app://pilot/orders")));
        QVERIFY(model.goBack(anchor, BrowserTabKind::Web));
        const QString other = model.createTab(
            BrowserTabKind::App, QStringLiteral("Other"),
            QStringLiteral("app://pilot/other"), false);
        const QString victim = model.createTab(
            BrowserTabKind::Web, QStringLiteral("Victim"),
            QStringLiteral("https://example.test/victim"), false);
        QVERIFY(!other.isEmpty());
        QVERIFY(!victim.isEmpty());
        QCOMPARE(model.activeId(), anchor);
        QVERIFY(model.canGoBack(anchor));
        QVERIFY(model.canGoForward(anchor));
        QCOMPARE(model.recentlyClosedCount(), 1);

        QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
        QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
        QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
        QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
        QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
        QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
        QStringList order;
        connect(&model, &BrowserTabModel::tabInserted, this,
                [&order](int index, const QString &id) {
                    order.append(
                        QStringLiteral("insert:%1:%2").arg(index).arg(id));
                });
        connect(&model, &BrowserTabModel::tabRemoved, this,
                [&order](int index, const QString &id) {
                    order.append(
                        QStringLiteral("remove:%1:%2").arg(index).arg(id));
                });
        connect(&model, &BrowserTabModel::tabMoved, this,
                [&order](int from, int to) {
                    order.append(
                        QStringLiteral("move:%1:%2").arg(from).arg(to));
                });
        connect(&model, &BrowserTabModel::tabChanged, this,
                [&order](int index) {
                    order.append(QStringLiteral("change:%1").arg(index));
                });
        connect(&model, &BrowserTabModel::activeTabChanged, this,
                [&order](int from, int to) {
                    order.append(
                        QStringLiteral("active:%1:%2").arg(from).arg(to));
                });
        connect(&model, &BrowserTabModel::persistenceNeeded, this,
                [&order] { order.append(QStringLiteral("persist")); });

        bool probing = false;
        bool allRejected = true;
        bool readOnlyQueriesStayedCoherent = true;
        int probeCalls = 0;
        const auto probe = [&] {
            if (probing) return;
            QScopedValueRollback<bool> probingGuard(probing, true);
            ++probeCalls;

            const QVector<BrowserTabSnapshot> before = model.snapshots();
            const int activeBefore = model.activeIndex();
            const int closedBefore = model.recentlyClosedCount();
            const int anchorIndex = model.indexOfId(anchor);
            readOnlyQueriesStayedCoherent =
                readOnlyQueriesStayedCoherent && !model.isEmpty()
                && model.count() == before.size() && anchorIndex >= 0
                && model.snapshotAt(anchorIndex).id == anchor
                && model.activeIndex() >= 0
                && model.activeIndex() < model.count()
                && model.snapshotAt(model.activeIndex()).id == model.activeId();
            if (!readOnlyQueriesStayedCoherent) return;
            (void)model.lifecycleAt(anchorIndex);
            (void)model.presentationAt(anchorIndex);
            (void)model.accessiblePresentationAt(anchorIndex);
            (void)model.canGoBack(anchor);
            (void)model.canGoForward(anchor);

            const auto reject = [&allRejected](bool accepted) {
                if (!accepted) return true;
                allRejected = false;
                return false;
            };
            const QString reentrantCreated = model.createTab(
                BrowserTabKind::Host, QStringLiteral("Reentrant create"),
                QStringLiteral("qbrowser://reentrant"));
            if (!reject(!reentrantCreated.isEmpty())) return;
            const QString activationTarget = model.activeId() == anchor
                ? other
                : anchor;
            if (!reject(model.activateTab(model.indexOfId(activationTarget))))
                return;
            if (!reject(model.moveTab(model.indexOfId(anchor),
                                      model.indexOfId(other))))
                return;
            if (!reject(model.closeTab(model.indexOfId(other)))) return;
            if (!reject(!model.reopenMostRecentlyClosed().isEmpty())) return;
            if (!reject(model.navigateTab(
                    anchor, BrowserTabKind::App,
                    QStringLiteral("app://pilot/reentrant"))))
                return;
            if (!reject(model.goBack(anchor, BrowserTabKind::Host))) return;
            if (!reject(model.goForward(anchor, BrowserTabKind::App))) return;
            if (!reject(model.setTitle(anchor,
                                       QStringLiteral("Reentrant title"))))
                return;
            if (!reject(model.setLifecycle(anchor,
                                           BrowserTabLifecycle::Starting)))
                return;
            if (!reject(model.setLoadState(anchor, true, 33))) return;
            if (!reject(model.setVisualState(
                    anchor, BrowserVisualState::Recovering)))
                return;
            if (!reject(model.replaceFromValidatedSnapshot({}, -1))) return;

            readOnlyQueriesStayedCoherent =
                readOnlyQueriesStayedCoherent && model.snapshots() == before
                && model.activeIndex() == activeBefore
                && model.recentlyClosedCount() == closedBefore;
        };

        switch (signalCase) {
        case Inserted:
            connect(&model, &BrowserTabModel::tabInserted, this,
                    [probe](int, const QString &) { probe(); },
                    Qt::DirectConnection);
            break;
        case Moved:
            connect(&model, &BrowserTabModel::tabMoved, this,
                    [probe](int, int) { probe(); }, Qt::DirectConnection);
            break;
        case Removed:
            connect(&model, &BrowserTabModel::tabRemoved, this,
                    [probe](int, const QString &) { probe(); },
                    Qt::DirectConnection);
            break;
        case Changed:
            connect(&model, &BrowserTabModel::tabChanged, this,
                    [probe](int) { probe(); }, Qt::DirectConnection);
            break;
        case ActiveChanged:
            connect(&model, &BrowserTabModel::activeTabChanged, this,
                    [probe](int, int) { probe(); }, Qt::DirectConnection);
            break;
        case Persistence:
            connect(&model, &BrowserTabModel::persistenceNeeded, this, probe,
                    Qt::DirectConnection);
            break;
        default:
            QFAIL("Unexpected signal case");
        }

        QStringList expectedOrder;
        switch (signalCase) {
        case Inserted: {
            const QString outerId = model.createTab(
                BrowserTabKind::Host, QStringLiteral("Outer insert"),
                QStringLiteral("qbrowser://outer"), false);
            QVERIFY(!outerId.isEmpty());
            QVERIFY(model.indexOfId(outerId) >= 0);
            QCOMPARE(model.activeId(), anchor);
            expectedOrder = {
                QStringLiteral("insert:3:%1").arg(outerId),
                QStringLiteral("persist"),
            };
            break;
        }
        case Moved:
            QVERIFY(model.moveTab(1, 2));
            QCOMPARE(model.snapshotAt(0).id, anchor);
            QCOMPARE(model.snapshotAt(1).id, victim);
            QCOMPARE(model.snapshotAt(2).id, other);
            QCOMPARE(model.activeId(), anchor);
            expectedOrder = {QStringLiteral("move:1:2"),
                             QStringLiteral("persist")};
            break;
        case Removed:
            QVERIFY(model.closeTab(2));
            QCOMPARE(model.count(), 2);
            QCOMPARE(model.indexOfId(victim), -1);
            QCOMPARE(model.activeId(), anchor);
            expectedOrder = {
                QStringLiteral("remove:2:%1").arg(victim),
                QStringLiteral("persist"),
            };
            break;
        case Changed:
            QVERIFY(model.setTitle(anchor, QStringLiteral("Outer title")));
            QCOMPARE(model.snapshotAt(model.indexOfId(anchor)).title,
                     QStringLiteral("Outer title"));
            expectedOrder = {QStringLiteral("change:0"),
                             QStringLiteral("persist")};
            break;
        case ActiveChanged:
            QVERIFY(model.activateTab(1));
            QCOMPARE(model.activeId(), other);
            expectedOrder = {QStringLiteral("active:0:1"),
                             QStringLiteral("persist")};
            break;
        case Persistence:
            QVERIFY(model.navigateTab(anchor, BrowserTabKind::Host,
                                      QStringLiteral("qbrowser://settings")));
            QCOMPARE(model.snapshotAt(model.indexOfId(anchor)).address,
                     QStringLiteral("qbrowser://settings"));
            expectedOrder = {QStringLiteral("change:0"),
                             QStringLiteral("persist")};
            break;
        default:
            QFAIL("Unexpected signal case");
        }

        QVERIFY2(probeCalls > 0, "The selected direct signal was not emitted");
        QVERIFY2(allRejected,
                 "A direct signal slot was allowed to mutate the model");
        QVERIFY(readOnlyQueriesStayedCoherent);
        QCOMPARE(inserted.count(), signalCase == Inserted ? 1 : 0);
        QCOMPARE(removed.count(), signalCase == Removed ? 1 : 0);
        QCOMPARE(moved.count(), signalCase == Moved ? 1 : 0);
        QCOMPARE(changed.count(),
                 signalCase == Changed || signalCase == Persistence ? 1 : 0);
        QCOMPARE(active.count(), signalCase == ActiveChanged ? 1 : 0);
        QCOMPARE(persistence.count(), 1);
        QCOMPARE(order, expectedOrder);
    }
}

void BrowserTabModelTest::tabChangedObserversKeepAValidIndexDuringNotification()
{
    BrowserTabModel model;
    const QString first = model.createTab(BrowserTabKind::Host,
                                          QStringLiteral("First"),
                                          QStringLiteral("qbrowser://first"));
    const QString second = model.createTab(BrowserTabKind::App,
                                           QStringLiteral("Second"),
                                           QStringLiteral("app://second"),
                                           false);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    bool closeWasRejected = false;
    bool laterObserverSawOriginalTab = false;
    int laterObserverCalls = 0;
    connect(&model, &BrowserTabModel::tabChanged, this,
            [&](int index) { closeWasRejected = !model.closeTab(index); },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::tabChanged, this,
            [&](int index) {
                ++laterObserverCalls;
                laterObserverSawOriginalTab = index >= 0
                    && index < model.count()
                    && model.snapshotAt(index).id == first;
            },
            Qt::DirectConnection);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);

    QVERIFY(model.setTitle(first, QStringLiteral("Updated first")));
    QVERIFY(closeWasRejected);
    QCOMPARE(laterObserverCalls, 1);
    QVERIFY(laterObserverSawOriginalTab);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.snapshotAt(0).id, first);
    QCOMPARE(model.snapshotAt(0).title, QStringLiteral("Updated first"));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 1);
}

void BrowserTabModelTest::restorePublishesSelfConsistentGranularChanges()
{
    BrowserTabModel model;
    const QString oldFirst = model.createTab(
        BrowserTabKind::Host, QStringLiteral("Old first"),
        QStringLiteral("qbrowser://old-first"));
    const QString oldSecond = model.createTab(
        BrowserTabKind::App, QStringLiteral("Old second"),
        QStringLiteral("app://old-second"), false);
    const QString oldThird = model.createTab(
        BrowserTabKind::Web, QStringLiteral("Old third"),
        QStringLiteral("https://example.test/old-third"), false);
    QVERIFY(model.activateTab(1));

    const BrowserTabSnapshot newFirst = restoredTab(
        201, BrowserTabKind::App, QStringLiteral("New first"),
        {QStringLiteral("app://new-first")}, 0);
    const BrowserTabSnapshot newSecond = restoredTab(
        202, BrowserTabKind::Host, QStringLiteral("New second"),
        {QStringLiteral("qbrowser://new-second")}, 0);
    const QVector<BrowserTabSnapshot> restored{newFirst, newSecond};

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    QStringList observations;
    bool everyObservationWasCoherent = true;
    bool finalActiveWasPublishedAtomically = false;
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&](int index, const QString &id) {
                const int visibleCount = model.count();
                everyObservationWasCoherent = everyObservationWasCoherent
                    && visibleCount == index && model.indexOfId(id) == -1
                    && model.activeIndex() == -1
                    && model.activeId().isEmpty();
                observations.append(
                    QStringLiteral("remove:%1:%2:%3")
                        .arg(index)
                        .arg(id)
                        .arg(visibleCount));
            },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::tabInserted, this,
            [&](int index, const QString &id) {
                const int visibleCount = model.count();
                const bool indexIsValid = index >= 0 && index < visibleCount;
                const QString visibleId = indexIsValid
                    ? model.snapshotAt(index).id
                    : QStringLiteral("<invalid>");
                everyObservationWasCoherent = everyObservationWasCoherent
                    && visibleCount == index + 1 && indexIsValid
                    && visibleId == id && model.activeIndex() == -1
                    && model.activeId().isEmpty();
                observations.append(
                    QStringLiteral("insert:%1:%2:%3:%4")
                        .arg(index)
                        .arg(id)
                        .arg(visibleCount)
                        .arg(visibleId));
            },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&](int from, int to) {
                finalActiveWasPublishedAtomically = from == 1 && to == 1
                    && model.activeIndex() == 1
                    && model.activeId() == newSecond.id;
                observations.append(
                    QStringLiteral("active:%1:%2").arg(from).arg(to));
            },
            Qt::DirectConnection);

    QVERIFY(model.replaceFromValidatedSnapshot(restored, 1));
    QVERIFY(everyObservationWasCoherent);
    QVERIFY(finalActiveWasPublishedAtomically);
    QVERIFY(model.snapshots() == restored);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(model.activeId(), newSecond.id);
    QCOMPARE(model.lifecycleAt(0), BrowserTabLifecycle::Dormant);
    QCOMPARE(model.lifecycleAt(1), BrowserTabLifecycle::Dormant);
    QCOMPARE(removed.count(), 3);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 1);
    QCOMPARE(active.at(0).at(1).toInt(), 1);
    QCOMPARE(persistence.count(), 0);
    QCOMPARE(observations,
             QStringList({
                 QStringLiteral("remove:2:%1:2").arg(oldThird),
                 QStringLiteral("remove:1:%1:1").arg(oldSecond),
                 QStringLiteral("remove:0:%1:0").arg(oldFirst),
                 QStringLiteral("insert:0:%1:1:%1").arg(newFirst.id),
                 QStringLiteral("insert:1:%1:2:%1").arg(newSecond.id),
                 QStringLiteral("active:1:1"),
              }));
}

void BrowserTabModelTest::restorePublishesCompletionWhenActiveIdentityIsUnchanged()
{
    BrowserTabModel model;
    const QString first = model.createTab(
        BrowserTabKind::Host, QStringLiteral("First"),
        QStringLiteral("qbrowser://first"));
    const QString activeId = model.createTab(
        BrowserTabKind::App, QStringLiteral("Active"),
        QStringLiteral("app://active"), false);
    const QString third = model.createTab(
        BrowserTabKind::Web, QStringLiteral("Third"),
        QStringLiteral("https://example.test/third"), false);
    QVERIFY(!first.isEmpty());
    QVERIFY(!activeId.isEmpty());
    QVERIFY(!third.isEmpty());
    QVERIFY(model.activateTab(1));

    QVector<BrowserTabSnapshot> restored = model.snapshots();
    restored[0].title = QStringLiteral("Updated first descriptor");
    QVERIFY(restored.at(1).id == activeId);

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    bool everyGranularObserverSawNoActiveTab = true;
    bool completionObserverSawFinalActiveTab = false;
    const auto observeNoActiveTab = [&] {
        everyGranularObserverSawNoActiveTab =
            everyGranularObserverSawNoActiveTab
            && model.activeIndex() == -1 && model.activeId().isEmpty();
    };
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [observeNoActiveTab](int, const QString &) {
                observeNoActiveTab();
            },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::tabInserted, this,
            [observeNoActiveTab](int, const QString &) {
                observeNoActiveTab();
            },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&](int from, int to) {
                completionObserverSawFinalActiveTab = from == 1 && to == 1
                    && model.activeIndex() == 1
                    && model.activeId() == activeId;
            },
            Qt::DirectConnection);

    QVERIFY(model.replaceFromValidatedSnapshot(restored, 1));
    QVERIFY(model.snapshots() == restored);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(model.activeId(), activeId);
    for (int index = 0; index < model.count(); ++index) {
        QCOMPARE(model.lifecycleAt(index), BrowserTabLifecycle::Dormant);
    }
    QCOMPARE(removed.count(), 3);
    QCOMPARE(inserted.count(), 3);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 1);
    QCOMPARE(active.at(0).at(1).toInt(), 1);
    QCOMPARE(persistence.count(), 0);
    QVERIFY(everyGranularObserverSawNoActiveTab);
    QVERIFY(completionObserverSawFinalActiveTab);
}

void BrowserTabModelTest::restoreToEmptyIsInactiveAndNoOpRestoreIsSilent()
{
    BrowserTabModel model;
    const QString first = model.createTab(
        BrowserTabKind::Host, QStringLiteral("First"),
        QStringLiteral("qbrowser://first"));
    const QString second = model.createTab(
        BrowserTabKind::App, QStringLiteral("Second"),
        QStringLiteral("app://second"), false);
    const QString third = model.createTab(
        BrowserTabKind::Web, QStringLiteral("Third"),
        QStringLiteral("https://example.test/third"), false);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(!third.isEmpty());
    QVERIFY(model.activateTab(1));

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    bool everyRemovalObserverSawNoActiveTab = true;
    bool completionObserverSawEmptyResult = false;
    int removalObserverCalls = 0;
    int completionObserverCalls = 0;
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&](int, const QString &) {
                ++removalObserverCalls;
                everyRemovalObserverSawNoActiveTab =
                    everyRemovalObserverSawNoActiveTab
                    && model.activeIndex() == -1
                    && model.activeId().isEmpty();
            },
            Qt::DirectConnection);
    connect(&model, &BrowserTabModel::activeTabChanged, this,
            [&](int from, int to) {
                ++completionObserverCalls;
                completionObserverSawEmptyResult = from == 1 && to == -1
                    && model.activeIndex() == -1
                    && model.activeId().isEmpty();
            },
            Qt::DirectConnection);

    QVERIFY(model.replaceFromValidatedSnapshot({}, -1));
    QVERIFY(everyRemovalObserverSawNoActiveTab);
    QVERIFY(completionObserverSawEmptyResult);
    QCOMPARE(removalObserverCalls, 3);
    QCOMPARE(completionObserverCalls, 1);
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.activeIndex(), -1);
    QVERIFY(model.activeId().isEmpty());
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 3);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(active.at(0).at(0).toInt(), 1);
    QCOMPARE(active.at(0).at(1).toInt(), -1);
    QCOMPARE(persistence.count(), 0);

    clearSpies(inserted, removed, moved, changed, active, persistence);
    removalObserverCalls = 0;
    completionObserverCalls = 0;
    QVERIFY(model.replaceFromValidatedSnapshot({}, -1));
    QCOMPARE(removalObserverCalls, 0);
    QCOMPARE(completionObserverCalls, 0);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 0);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::restoreRejectsNestedMutations()
{
    BrowserTabModel model;
    const QString oldFirst = model.createTab(
        BrowserTabKind::Host, QStringLiteral("Old first"),
        QStringLiteral("qbrowser://old-first"));
    const QString oldSecond = model.createTab(
        BrowserTabKind::App, QStringLiteral("Old second"),
        QStringLiteral("app://old-second"), false);
    const QString oldThird = model.createTab(
        BrowserTabKind::Web, QStringLiteral("Old third"),
        QStringLiteral("https://example.test/old-third"), false);
    QVERIFY(!oldFirst.isEmpty());
    QVERIFY(!oldSecond.isEmpty());
    QVERIFY(!oldThird.isEmpty());

    const QVector<BrowserTabSnapshot> restored{
        restoredTab(211, BrowserTabKind::App, QStringLiteral("Restored one"),
                    {QStringLiteral("app://restored-one")}, 0),
        restoredTab(212, BrowserTabKind::Host, QStringLiteral("Restored two"),
                    {QStringLiteral("qbrowser://restored-two")}, 0),
    };
    const QVector<BrowserTabSnapshot> nested{
        restoredTab(213, BrowserTabKind::Web, QStringLiteral("Nested"),
                    {QStringLiteral("https://example.test/nested")}, 0),
    };

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    QSignalSpy removed(&model, &BrowserTabModel::tabRemoved);
    QSignalSpy moved(&model, &BrowserTabModel::tabMoved);
    QSignalSpy changed(&model, &BrowserTabModel::tabChanged);
    QSignalSpy active(&model, &BrowserTabModel::activeTabChanged);
    QSignalSpy persistence(&model, &BrowserTabModel::persistenceNeeded);
    bool attempted = false;
    bool nestedCloseWasAccepted = false;
    bool nestedRestoreWasAccepted = false;
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&](int, const QString &) {
                if (attempted) return;
                attempted = true;
                nestedCloseWasAccepted = model.closeTab(0);
                nestedRestoreWasAccepted =
                    model.replaceFromValidatedSnapshot(nested, 0);
            },
            Qt::DirectConnection);

    QVERIFY(model.replaceFromValidatedSnapshot(restored, 1));
    QVERIFY(attempted);
    QVERIFY(!nestedCloseWasAccepted);
    QVERIFY(!nestedRestoreWasAccepted);
    QVERIFY(model.snapshots() == restored);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(removed.count(), 3);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(active.count(), 1);
    QCOMPARE(persistence.count(), 0);
}

void BrowserTabModelTest::restoreOwnsItsInputBeforeEmittingSignals()
{
    BrowserTabModel model;
    const QString oldFirst = model.createTab(
        BrowserTabKind::Host, QStringLiteral("Old first"),
        QStringLiteral("qbrowser://old-first"));
    const QString oldSecond = model.createTab(
        BrowserTabKind::App, QStringLiteral("Old second"),
        QStringLiteral("app://old-second"), false);
    QVERIFY(!oldFirst.isEmpty());
    QVERIFY(!oldSecond.isEmpty());

    QVector<BrowserTabSnapshot> input{
        restoredTab(221, BrowserTabKind::App, QStringLiteral("Owned one"),
                    {QStringLiteral("app://owned-one")}, 0),
        restoredTab(222, BrowserTabKind::Host, QStringLiteral("Owned two"),
                    {QStringLiteral("qbrowser://owned-two")}, 0),
    };
    const QVector<BrowserTabSnapshot> expected = input;
    const BrowserTabSnapshot aliasReplacementOne = restoredTab(
        223, BrowserTabKind::Web, QStringLiteral("Alias one"),
        {QStringLiteral("https://example.test/alias-one")}, 0);
    const BrowserTabSnapshot aliasReplacementTwo = restoredTab(
        224, BrowserTabKind::Web, QStringLiteral("Alias two"),
        {QStringLiteral("https://example.test/alias-two")}, 0);

    QSignalSpy inserted(&model, &BrowserTabModel::tabInserted);
    bool aliasWasMutated = false;
    connect(&model, &BrowserTabModel::tabRemoved, this,
            [&](int, const QString &) {
                if (aliasWasMutated) return;
                aliasWasMutated = true;
                input.clear();
                input.append(aliasReplacementOne);
                input.append(aliasReplacementTwo);
            },
            Qt::DirectConnection);

    QVERIFY(model.replaceFromValidatedSnapshot(input, 1));
    QVERIFY(aliasWasMutated);
    QVERIFY(input != expected);
    QVERIFY(model.snapshots() == expected);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(inserted.at(0).at(0).toInt(), 0);
    QCOMPARE(inserted.at(0).at(1).toString(), expected.at(0).id);
    QCOMPARE(inserted.at(1).at(0).toInt(), 1);
    QCOMPARE(inserted.at(1).at(1).toString(), expected.at(1).id);
}

void BrowserTabModelTest::restoreSignalCanDestroyModel()
{
    auto model = std::make_unique<BrowserTabModel>();
    QPointer<BrowserTabModel> guard(model.get());
    const QVector<BrowserTabSnapshot> restored{
        restoredTab(231, BrowserTabKind::Host, QStringLiteral("First"),
                    {QStringLiteral("qbrowser://newtab")}, 0),
        restoredTab(232, BrowserTabKind::Host, QStringLiteral("Second"),
                    {QStringLiteral("qbrowser://newtab")}, 0),
    };
    bool destroyed = false;
    connect(model.get(), &BrowserTabModel::tabInserted, this,
            [&](int, const QString &) {
                if (destroyed) return;
                destroyed = true;
                model.reset();
            },
            Qt::DirectConnection);

    const bool applied = model->replaceFromValidatedSnapshot(restored, 0);
    QVERIFY(!applied);
    QVERIFY(destroyed);
    QVERIFY(guard.isNull());
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
