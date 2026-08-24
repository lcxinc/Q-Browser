#include "NewTabPage.h"

#include "WebSurface.h"
#include "WorkerSurface.h"

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineView>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

#include <array>

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
    void excludesExternalHostAndNonCanonicalAddresses();
    void sanitizesAndBoundsUntrustedTitlesAsOrdinaryText();
    void providesStableNamesPlainLabelsAndFocusOrder();
    void activatesFocusedEntriesFromTheKeyboard();
    void constructionCreatesNoWebOrWorkerResources();
};

void NewTabPageTest::exposesFixedPilotEntries()
{
    NewTabPage page;
    QSignalSpy activated(&page, &NewTabPage::addressActivated);

    for (const ExpectedEntry &entry : fixedEntries()) {
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
        {QStringLiteral("<b>Orders</b>\n\u202e\u2066"),
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
             QStringLiteral("<b>Orders</b>"));
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
