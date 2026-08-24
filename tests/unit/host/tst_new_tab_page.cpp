#include "NewTabPage.h"

#include "BrowserAddress.h"
#include "BrowserTabModel.h"
#include "PilotRoutes.h"
#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

#include <array>
#include <optional>

namespace {

struct ExpectedEntry final
{
    QString objectName;
    QString title;
    QString address;
};

const std::array<ExpectedEntry, 7> &fixedEntries()
{
    static const std::array<ExpectedEntry, 7> entries{{
        {QStringLiteral("new-tab-pilot-login"),
         QStringLiteral("Login"),
         QStringLiteral("app://pilot/login")},
        {QStringLiteral("new-tab-pilot-dashboard"),
         QStringLiteral("Dashboard"),
         QStringLiteral("app://pilot/dashboard")},
        {QStringLiteral("new-tab-pilot-orders"),
         QStringLiteral("Orders"),
         QStringLiteral("app://pilot/orders")},
        {QStringLiteral("new-tab-pilot-customers"),
         QStringLiteral("Customers"),
         QStringLiteral("app://pilot/customers")},
        {QStringLiteral("new-tab-pilot-files"),
         QStringLiteral("Files"),
         QStringLiteral("app://pilot/files")},
        {QStringLiteral("new-tab-pilot-settings"),
         QStringLiteral("Settings"),
         QStringLiteral("app://pilot/settings")},
        {QStringLiteral("new-tab-pilot-help"),
         QStringLiteral("Help"),
         QStringLiteral("app://pilot/web/help")},
    }};
    return entries;
}

QAbstractButton *button(NewTabPage &page, const QString &objectName)
{
    return page.findChild<QAbstractButton *>(objectName);
}

QList<QAbstractButton *> recentButtons(NewTabPage &page)
{
    QList<QAbstractButton *> result;
    const QList<QAbstractButton *> allButtons = page.findChildren<QAbstractButton *>();
    for (QAbstractButton *candidate : allButtons) {
        if (candidate->objectName().startsWith(QStringLiteral("new-tab-recent-"))) {
            result.append(candidate);
        }
    }
    return result;
}

QWidget *nextFocusable(QWidget *widget)
{
    QWidget *candidate = widget->nextInFocusChain();
    while (candidate != widget && candidate->focusPolicy() == Qt::NoFocus) {
        candidate = candidate->nextInFocusChain();
    }
    return candidate;
}

#ifdef Q_OS_WIN
int directChildProcessCount(const QString &imageName)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return -1;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32ParentProcessID == GetCurrentProcessId()
                && QString::fromWCharArray(entry.szExeFile).compare(
                       imageName, Qt::CaseInsensitive)
                       == 0) {
                ++count;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return count;
}
#endif

} // namespace

class NewTabPageTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesFixedPilotEntries();
    void recentRoutesPreserveOrderAndFirstDuplicate();
    void recentRoutesAreBoundedAndReplacePriorState();
    void recentCandidateInspectionAndRawTitlesAreBounded();
    void excludesExternalHostAndNonCanonicalAddresses();
    void sanitizesAndBoundsUntrustedTitlesAsOrdinaryText();
    void rendersAmpersandsLiterallyWithoutMnemonics();
    void providesStableNamesPlainLabelsAndFocusOrder();
    void destroyedButtonReentryIsSafe();
    void synchronousCleanupReentryIsCoalesced();
    void synchronousActivationRefreshKeepsTheSenderAlive();
    void populatedEmptyRepopulatedStateIsDeterministic();
    void activatesFocusedEntriesFromTheKeyboard();
    void constructionCreatesNoWebOrWorkerResources();
};

void NewTabPageTest::exposesFixedPilotEntries()
{
    NewTabPage page;
    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    const std::optional<RouteRegistry> registry = createPilotRouteRegistry(
        QUrl(QStringLiteral("http://127.0.0.1:4180/")));
    QVERIFY(registry.has_value());

    for (const ExpectedEntry &entry : fixedEntries()) {
        const BrowserAddress parsed = BrowserAddress::parse(entry.address);
        QVERIFY2(parsed.isValid(), qPrintable(entry.address));
        QVERIFY2(registry->match(parsed.appPath()).isValid(),
                 qPrintable(parsed.appPath()));

        QAbstractButton *const routeButton = button(page, entry.objectName);
        QVERIFY2(routeButton != nullptr, qPrintable(entry.objectName));
        QCOMPARE(routeButton->text(), entry.title);
        QCOMPARE(routeButton->accessibleName(),
                 QStringLiteral("Open %1").arg(entry.title));

        routeButton->click();
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.takeFirst().at(0).toString(), entry.address);
    }
    QCOMPARE(recentButtons(page).size(), 0);
}

void NewTabPageTest::recentCandidateInspectionAndRawTitlesAreBounded()
{
    NewTabPage page;
    constexpr int maximumInspectedCandidates = 64;

    QVector<NewTabEntry> invalidFirst;
    const QString boundedTitle(BrowserTabModel::MaxRawTitleCodeUnits,
                               QLatin1Char('x'));
    for (int index = 0; index < maximumInspectedCandidates; ++index) {
        invalidFirst.append({boundedTitle,
                             QStringLiteral("https://example.test/%1")
                                 .arg(index)});
    }
    invalidFirst.append({QStringLiteral("Not inspected"),
                         QStringLiteral("app://pilot/dashboard")});
    page.setRecentRoutes(invalidFirst);
    QCOMPARE(recentButtons(page).size(), 0);

    QVector<NewTabEntry> duplicateFirst{
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
    };
    for (int index = 1; index < maximumInspectedCandidates; ++index) {
        duplicateFirst.append({boundedTitle,
                               QStringLiteral("app://pilot/orders")});
    }
    duplicateFirst.append({QStringLiteral("Also not inspected"),
                           QStringLiteral("app://pilot/dashboard")});
    page.setRecentRoutes(duplicateFirst);
    QCOMPARE(recentButtons(page).size(), 1);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->text(),
             QStringLiteral("Orders"));

    QString malformedAtBound(BrowserTabModel::MaxRawTitleCodeUnits,
                             QLatin1Char('m'));
    malformedAtBound[malformedAtBound.size() - 1] = QChar(0xd800);
    page.setRecentRoutes({
        {QString(BrowserTabModel::MaxRawTitleCodeUnits + 1, QLatin1Char('o')),
         QStringLiteral("app://pilot/orders")},
        {QStringLiteral("Duplicate valid title"),
         QStringLiteral("app://pilot/orders")},
        {malformedAtBound, QStringLiteral("app://pilot/dashboard")},
        {QStringLiteral("Another duplicate valid title"),
         QStringLiteral("app://pilot/dashboard")},
        {boundedTitle, QStringLiteral("app://pilot/customers")},
    });
    QCOMPARE(recentButtons(page).size(), 1);
    QAbstractButton *const accepted = button(
        page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(accepted != nullptr);
    QCOMPARE(accepted->text(),
             QString(BrowserTabModel::MaxTitleCodeUnits, QLatin1Char('x')));
}

void NewTabPageTest::recentRoutesPreserveOrderAndFirstDuplicate()
{
    NewTabPage page;
    const QVector<NewTabEntry> routes{
        {QStringLiteral("Orders first"), QStringLiteral("app://pilot/orders")},
        {QStringLiteral("Dashboard"), QStringLiteral("app://pilot/dashboard")},
        {QStringLiteral("Orders duplicate"), QStringLiteral("app://pilot/orders")},
        {QStringLiteral("Customer"), QStringLiteral("app://pilot/customers/CUS-001")},
    };

    page.setRecentRoutes(routes);

    const std::array<ExpectedEntry, 3> expected{{
        {QStringLiteral("new-tab-recent-0"),
         QStringLiteral("Orders first"),
         QStringLiteral("app://pilot/orders")},
        {QStringLiteral("new-tab-recent-1"),
         QStringLiteral("Dashboard"),
         QStringLiteral("app://pilot/dashboard")},
        {QStringLiteral("new-tab-recent-2"),
         QStringLiteral("Customer"),
         QStringLiteral("app://pilot/customers/CUS-001")},
    }};
    QCOMPARE(recentButtons(page).size(), static_cast<qsizetype>(expected.size()));

    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    for (const ExpectedEntry &entry : expected) {
        QAbstractButton *const routeButton = button(page, entry.objectName);
        QVERIFY2(routeButton != nullptr, qPrintable(entry.objectName));
        QCOMPARE(routeButton->text(), entry.title);
        QCOMPARE(routeButton->accessibleName(),
                 QStringLiteral("Open recent route: %1").arg(entry.title));
        routeButton->click();
        QCOMPARE(activated.takeFirst().at(0).toString(), entry.address);
    }
}

void NewTabPageTest::recentRoutesAreBoundedAndReplacePriorState()
{
    NewTabPage page;
    QVector<NewTabEntry> routes;
    for (int index = 0; index < 20; ++index) {
        routes.append({QStringLiteral("Order %1").arg(index),
                       QStringLiteral("app://pilot/orders/%1").arg(index)});
    }

    page.setRecentRoutes(routes);

    QCOMPARE(recentButtons(page).size(), 16);
    QVERIFY(button(page, QStringLiteral("new-tab-recent-15")) != nullptr);
    QVERIFY(button(page, QStringLiteral("new-tab-recent-16")) == nullptr);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-15"))->text(),
             QStringLiteral("Order 15"));

    page.setRecentRoutes({
        {QStringLiteral("Files"), QStringLiteral("app://pilot/files")},
    });

    QCOMPARE(recentButtons(page).size(), 1);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->text(),
             QStringLiteral("Files"));
    QVERIFY(button(page, QStringLiteral("new-tab-recent-1")) == nullptr);
    QVERIFY(page.findChildren<QAbstractButton *>().size()
            <= static_cast<qsizetype>(fixedEntries().size()) + 16);
}

void NewTabPageTest::excludesExternalHostAndNonCanonicalAddresses()
{
    NewTabPage page;
    const QString overlong = QStringLiteral("app://pilot/orders/")
        + QString(2048, QLatin1Char('x'));
    page.setRecentRoutes({
        {QStringLiteral("HTTPS"), QStringLiteral("https://example.test/")},
        {QStringLiteral("File"), QStringLiteral("file:///C:/secret.txt")},
        {QStringLiteral("Data"), QStringLiteral("data:text/plain,hello")},
        {QStringLiteral("Script"), QStringLiteral("javascript:alert(1)")},
        {QStringLiteral("Unknown scheme"), QStringLiteral("custom://pilot/orders")},
        {QStringLiteral("Host page"), QStringLiteral("qbrowser://newtab")},
        {QStringLiteral("Wrong authority"), QStringLiteral("app://other/orders")},
        {QStringLiteral("Scheme case"), QStringLiteral("APP://pilot/orders")},
        {QStringLiteral("Encoded unreserved"), QStringLiteral("app://pilot/%6Frders")},
        {QStringLiteral("Traversal"), QStringLiteral("app://pilot/orders/../settings")},
        {QStringLiteral("Too long"), overlong},
        {QStringLiteral("Valid"), QStringLiteral("app://pilot/dashboard")},
    });

    QCOMPARE(recentButtons(page).size(), 1);
    QAbstractButton *const valid = button(page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(valid != nullptr);
    QCOMPARE(valid->text(), QStringLiteral("Valid"));

    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    valid->click();
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.takeFirst().at(0).toString(),
             QStringLiteral("app://pilot/dashboard"));
}

void NewTabPageTest::sanitizesAndBoundsUntrustedTitlesAsOrdinaryText()
{
    NewTabPage page;
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    QString loneSurrogate = QStringLiteral("invalid");
    loneSurrogate.append(QChar(0xd800));
    page.setRecentRoutes({
        {QStringLiteral("<b>A&B</b>\u0001\n\u202e\u2066"),
         QStringLiteral("app://pilot/orders")},
        {QString(255, QLatin1Char('a')) + emoji + QStringLiteral("tail"),
         QStringLiteral("app://pilot/dashboard")},
        {QString(254, QLatin1Char('b')) + emoji + QStringLiteral("tail"),
         QStringLiteral("app://pilot/customers")},
        {QStringLiteral("\u0001\n\u202e"), QStringLiteral("app://pilot/files")},
        {loneSurrogate, QStringLiteral("app://pilot/settings")},
    });

    QCOMPARE(recentButtons(page).size(), 4);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->text(),
             QStringLiteral("<b>A&&B</b>"));
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->accessibleName(),
             QStringLiteral("Open recent route: <b>A&B</b>"));
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-1"))->text(),
             QString(255, QLatin1Char('a')));
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-2"))->text(),
             QString(254, QLatin1Char('b')) + emoji);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-2"))->text().size(), 256);
    QVERIFY(!button(page, QStringLiteral("new-tab-recent-2"))->text().back()
                 .isHighSurrogate());
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-3"))->text(),
             QStringLiteral("Untitled route"));
}

void NewTabPageTest::rendersAmpersandsLiterallyWithoutMnemonics()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("R&D"), QStringLiteral("app://pilot/orders/1")},
        {QStringLiteral("A&&B"), QStringLiteral("app://pilot/orders/2")},
        {QStringLiteral("Trail&"), QStringLiteral("app://pilot/orders/3")},
        {QStringLiteral("&A&B&&C&"), QStringLiteral("app://pilot/orders/4")},
    });

    const struct AmpersandCase final {
        const char *objectName;
        const char *title;
        const char *buttonText;
    } cases[] = {
        {"new-tab-recent-0", "R&D", "R&&D"},
        {"new-tab-recent-1", "A&&B", "A&&&&B"},
        {"new-tab-recent-2", "Trail&", "Trail&&"},
        {"new-tab-recent-3", "&A&B&&C&", "&&A&&B&&&&C&&"},
    };
    for (const AmpersandCase &entry : cases) {
        QAbstractButton *const routeButton = button(
            page, QString::fromLatin1(entry.objectName));
        QVERIFY2(routeButton != nullptr, entry.objectName);
        QCOMPARE(routeButton->text(), QString::fromLatin1(entry.buttonText));
        QCOMPARE(routeButton->accessibleName(),
                 QStringLiteral("Open recent route: %1")
                     .arg(QString::fromLatin1(entry.title)));
        QVERIFY(routeButton->shortcut().isEmpty());
    }

    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    QTest::keyClick(&page, Qt::Key_D, Qt::AltModifier);
    QCOMPARE(activated.count(), 0);
}

void NewTabPageTest::providesStableNamesPlainLabelsAndFocusOrder()
{
    NewTabPage page;
    QCOMPARE(page.objectName(), QStringLiteral("new-tab-page"));
    QCOMPARE(page.accessibleName(), QStringLiteral("Q-Browser New Tab"));

    const struct ExpectedLabel final {
        const char *objectName;
        const char *text;
        const char *accessibleName;
    } labels[] = {
        {"new-tab-heading", "Q-Browser", "Q-Browser New Tab"},
        {"new-tab-pilot-heading", "Pilot", "Pilot routes"},
        {"new-tab-recent-heading", "Recent", "Recent routes"},
    };
    for (const ExpectedLabel &expected : labels) {
        QLabel *const label = page.findChild<QLabel *>(
            QString::fromLatin1(expected.objectName));
        QVERIFY2(label != nullptr, expected.objectName);
        QCOMPARE(label->text(), QString::fromLatin1(expected.text));
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QCOMPARE(label->accessibleName(),
                 QString::fromLatin1(expected.accessibleName));
    }

    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
        {QStringLiteral("Help"), QStringLiteral("app://pilot/web/help")},
    });

    QList<QAbstractButton *> focusOrder;
    for (const ExpectedEntry &entry : fixedEntries()) {
        QAbstractButton *const routeButton = button(page, entry.objectName);
        QVERIFY(routeButton != nullptr);
        QCOMPARE(routeButton->focusPolicy(), Qt::StrongFocus);
        focusOrder.append(routeButton);
    }
    focusOrder.append(button(page, QStringLiteral("new-tab-recent-0")));
    focusOrder.append(button(page, QStringLiteral("new-tab-recent-1")));
    for (qsizetype index = 0; index + 1 < focusOrder.size(); ++index) {
        QCOMPARE(nextFocusable(focusOrder.at(index)), focusOrder.at(index + 1));
    }
}

void NewTabPageTest::destroyedButtonReentryIsSafe()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
    });
    QPointer<QAbstractButton> oldButton = button(
        page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(!oldButton.isNull());

    int destroyedCallbacks = 0;
    connect(oldButton.data(), &QObject::destroyed, &page,
            [&page, &destroyedCallbacks] {
                ++destroyedCallbacks;
                page.setRecentRoutes({
                    {QStringLiteral("Files"),
                     QStringLiteral("app://pilot/files")},
                });
            });

    delete oldButton.data();

    QVERIFY(oldButton.isNull());
    QCOMPARE(destroyedCallbacks, 1);
    QCOMPARE(recentButtons(page).size(), 1);
    QAbstractButton *const replacement = button(
        page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(replacement != nullptr);
    QCOMPARE(replacement->text(), QStringLiteral("Files"));
}

void NewTabPageTest::synchronousCleanupReentryIsCoalesced()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
    });
    QAbstractButton *const oldButton = button(
        page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(oldButton != nullptr);

    bool supersededRequested = false;
    connect(oldButton, &QObject::objectNameChanged, &page,
            [&page, &supersededRequested](const QString &name) {
                if (supersededRequested || !name.isEmpty()) return;
                supersededRequested = true;
                page.setRecentRoutes({
                    {QStringLiteral("Superseded"),
                     QStringLiteral("app://pilot/files")},
                });
            });
    bool finalRequested = false;
    connect(oldButton, &QObject::objectNameChanged, &page,
            [&page, &finalRequested](const QString &name) {
                if (finalRequested || !name.isEmpty()) return;
                finalRequested = true;
                page.setRecentRoutes({
                    {QStringLiteral("Final"),
                     QStringLiteral("app://pilot/settings")},
                });
            });

    page.setRecentRoutes({
        {QStringLiteral("Intermediate"), QStringLiteral("app://pilot/files")},
    });
    QCoreApplication::processEvents();

    QVERIFY(supersededRequested);
    QVERIFY(finalRequested);
    QCOMPARE(recentButtons(page).size(), 1);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->text(),
             QStringLiteral("Final"));
}

void NewTabPageTest::synchronousActivationRefreshKeepsTheSenderAlive()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
    });
    QPointer<QAbstractButton> sender = button(
        page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(!sender.isNull());
    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    bool refreshed = false;
    connect(&page, &NewTabPage::addressActivated, &page,
            [&page, &refreshed](const QString &) {
                if (refreshed) return;
                refreshed = true;
                page.setRecentRoutes({
                    {QStringLiteral("Files"),
                     QStringLiteral("app://pilot/files")},
                });
            });

    sender->click();

    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(),
             QStringLiteral("app://pilot/orders"));
    QVERIFY(!sender.isNull());
    QCOMPARE(recentButtons(page).size(), 1);
    QCOMPARE(button(page, QStringLiteral("new-tab-recent-0"))->text(),
             QStringLiteral("Files"));
}

void NewTabPageTest::populatedEmptyRepopulatedStateIsDeterministic()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
        {QStringLiteral("Help"), QStringLiteral("app://pilot/web/help")},
    });
    QVector<QPointer<QAbstractButton>> oldButtons;
    for (QAbstractButton *const oldButton : recentButtons(page)) {
        oldButtons.append(oldButton);
    }
    QCOMPARE(oldButtons.size(), 2);

    page.setRecentRoutes({});

    QCOMPARE(recentButtons(page).size(), 0);
    for (const QPointer<QAbstractButton> &oldButton : oldButtons) {
        QVERIFY(!oldButton.isNull());
        QVERIFY(oldButton->isHidden());
        QVERIFY(!oldButton->isEnabled());
        QCOMPARE(oldButton->focusPolicy(), Qt::NoFocus);
        QVERIFY(oldButton->objectName().isEmpty());
        QVERIFY(oldButton->accessibleName().isEmpty());
        QVERIFY(oldButton->shortcut().isEmpty());
    }

    page.setRecentRoutes({
        {QStringLiteral("Files"), QStringLiteral("app://pilot/files")},
        {QStringLiteral("Settings"), QStringLiteral("app://pilot/settings")},
    });

    QCOMPARE(recentButtons(page).size(), 2);
    QSet<QString> names;
    const QList<QAbstractButton *> allButtons =
        page.findChildren<QAbstractButton *>();
    for (QAbstractButton *const candidate : allButtons) {
        if (candidate->objectName().isEmpty()) continue;
        QVERIFY2(!names.contains(candidate->objectName()),
                 qPrintable(candidate->objectName()));
        names.insert(candidate->objectName());
    }
    QAbstractButton *const first = button(
        page, QStringLiteral("new-tab-recent-0"));
    QAbstractButton *const second = button(
        page, QStringLiteral("new-tab-recent-1"));
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);
    QCOMPARE(first->text(), QStringLiteral("Files"));
    QCOMPARE(second->text(), QStringLiteral("Settings"));
    QCOMPARE(first->focusPolicy(), Qt::StrongFocus);
    QCOMPARE(second->focusPolicy(), Qt::StrongFocus);
    QCOMPARE(nextFocusable(button(page, QStringLiteral("new-tab-pilot-help"))),
             first);
    QCOMPARE(nextFocusable(first), second);
}

void NewTabPageTest::activatesFocusedEntriesFromTheKeyboard()
{
    NewTabPage page;
    page.setRecentRoutes({
        {QStringLiteral("Orders"), QStringLiteral("app://pilot/orders")},
    });
    QAbstractButton *const recent = button(page, QStringLiteral("new-tab-recent-0"));
    QVERIFY(recent != nullptr);

    QSignalSpy activated(&page, &NewTabPage::addressActivated);
    recent->setFocus(Qt::TabFocusReason);
    QTest::keyClick(recent, Qt::Key_Space);

    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.takeFirst().at(0).toString(),
             QStringLiteral("app://pilot/orders"));
}

void NewTabPageTest::constructionCreatesNoWebOrWorkerResources()
{
#ifdef Q_OS_WIN
    const int rendererProcessesBefore = directChildProcessCount(
        QStringLiteral("QtWebEngineProcess.exe"));
    const int workerProcessesBefore = directChildProcessCount(
        QStringLiteral("qbrowser-worker.exe"));
    QVERIFY(rendererProcessesBefore >= 0);
    QVERIFY(workerProcessesBefore >= 0);
#endif

    NewTabPage page;
    QCoreApplication::processEvents();

    QCOMPARE(page.findChildren<QWebEnginePage *>().size(), 0);
    QCOMPARE(page.findChildren<QWebEngineView *>().size(), 0);
    QCOMPARE(page.findChildren<WebSurface *>().size(), 0);
    QCOMPARE(page.findChildren<WorkerSurface *>().size(), 0);
    const QList<QObject *> descendants = page.findChildren<QObject *>();
    for (const QObject *descendant : descendants) {
        const QString className = QString::fromLatin1(
            descendant->metaObject()->className());
        QVERIFY2(!className.contains(QStringLiteral("QWebEngine")),
                 qPrintable(className));
        QVERIFY2(className != QStringLiteral("WebSurface"), qPrintable(className));
        QVERIFY2(className != QStringLiteral("WorkerSurface"), qPrintable(className));
    }

#ifdef Q_OS_WIN
    QCOMPARE(directChildProcessCount(QStringLiteral("QtWebEngineProcess.exe")),
             rendererProcessesBefore);
    QCOMPARE(directChildProcessCount(QStringLiteral("qbrowser-worker.exe")),
             workerProcessesBefore);
#endif
}

QTEST_MAIN(NewTabPageTest)

#include "tst_new_tab_page.moc"
