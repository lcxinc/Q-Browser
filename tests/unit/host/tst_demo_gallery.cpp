#include "DemoGallery.h"
#include "NewTabPage.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTableWidget>
#include <QTabWidget>
#include <QTest>
#include <array>

class DemoGalleryTest final : public QObject {
    Q_OBJECT
private slots:
    void newTabsOpenExamplesAndKeepPilotAvailable();
    void catalogFiltersAndOpensExamples();
    void controlsApplySettings();
    void tableFiltersAndSortsNumericAmounts();
    void animationPausesWhenHiddenAndDisposesOnReturn();
    void kanbanAddsAndMovesTasks();
    void canvasDrawsAndClears();
    void everyExampleRendersAndReturns();
};

void DemoGalleryTest::newTabsOpenExamplesAndKeepPilotAvailable()
{
    NewTabPage page;
    page.resize(900, 650);
    page.show();
    QCoreApplication::processEvents();
    auto *sections = page.findChild<QTabWidget *>(QStringLiteral("new-tab-sections"));
    QVERIFY(sections != nullptr);
    QCOMPARE(sections->currentIndex(), 0);
    auto *firstCard = page.findChild<QWidget *>(QStringLiteral("demo-card-widgets"));
    auto *secondCard = page.findChild<QWidget *>(QStringLiteral("demo-card-table"));
    QVERIFY(firstCard && secondCard);
    QTRY_VERIFY(secondCard->x() > firstCard->x());
    page.showPilotRoutes();
    QCOMPARE(sections->currentIndex(), 1);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("new-tab-pilot-login")) != nullptr);
    page.showExamples();
    QCOMPARE(sections->currentIndex(), 0);
}

void DemoGalleryTest::catalogFiltersAndOpensExamples()
{
    DemoGallery gallery;
    gallery.resize(1100, 750);
    gallery.show();
    auto *search = gallery.findChild<QLineEdit *>(QStringLiteral("demo-search"));
    auto *category = gallery.findChild<QComboBox *>(QStringLiteral("demo-category"));
    auto *open = gallery.findChild<QPushButton *>(QStringLiteral("demo-open-table"));
    auto *chart = gallery.findChild<QPushButton *>(QStringLiteral("demo-open-charts"));
    QVERIFY(search && category && open && chart);
    search->setText(QStringLiteral("table"));
    QVERIFY(open->isVisible());
    QVERIFY(!chart->isVisible());
    search->clear();
    category->setCurrentIndex(2);
    QVERIFY(open->isVisible());
    QVERIFY(chart->isVisible());
    QVERIFY(!gallery.findChild<QPushButton *>(QStringLiteral("demo-open-widgets"))->isVisible());
    search->setText(QStringLiteral("no-such-example"));
    QVERIFY(gallery.findChild<QLabel *>(QStringLiteral("demo-empty"))->isVisible());
    search->clear();
    QTest::mouseClick(open, Qt::LeftButton);
    QPointer<QWidget> example = gallery.findChild<QWidget *>(QStringLiteral("demo-example-table"));
    QVERIFY(example && example->isVisible());
    QVERIFY(!gallery.openDemo(QStringLiteral("invalid")));
    QVERIFY(!example.isNull());
    QTest::mouseClick(gallery.findChild<QPushButton *>(QStringLiteral("demo-back")), Qt::LeftButton);
    QVERIFY(example.isNull());
    QVERIFY(open->isVisible());
}

void DemoGalleryTest::controlsApplySettings()
{
    DemoGallery gallery;
    gallery.show();
    QVERIFY(gallery.openDemo(QStringLiteral("widgets")));
    auto *name = gallery.findChild<QLineEdit *>(QStringLiteral("demo-control-name"));
    auto *heading = gallery.findChild<QLabel *>(QStringLiteral("demo-control-heading"));
    auto *apply = gallery.findChild<QPushButton *>(QStringLiteral("demo-control-apply"));
    auto *progress = gallery.findChild<QProgressBar *>(QStringLiteral("demo-control-progress"));
    QVERIFY(name && heading && apply && progress);
    name->setText(QStringLiteral("新的工作空间"));
    QCOMPARE(heading->text(), name->text());
    apply->click();
    QVERIFY(!apply->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(progress->value(), 100, 4000);
    QVERIFY(apply->isEnabled());
}

void DemoGalleryTest::tableFiltersAndSortsNumericAmounts()
{
    DemoGallery gallery;
    QVERIFY(gallery.openDemo(QStringLiteral("table")));
    auto *search = gallery.findChild<QLineEdit *>(QStringLiteral("demo-table-search"));
    auto *status = gallery.findChild<QComboBox *>(QStringLiteral("demo-table-status"));
    auto *table = gallery.findChild<QTableWidget *>(QStringLiteral("demo-orders-table"));
    QVERIFY(search && status && table);
    search->setText(QStringLiteral("QB-1001"));
    int visible = 0;
    for (int row = 0; row < table->rowCount(); ++row) if (!table->isRowHidden(row)) ++visible;
    QCOMPARE(visible, 1);
    search->clear();
    status->setCurrentIndex(2);
    table->sortItems(2, Qt::DescendingOrder);
    visible = 0;
    double previous = 100000;
    for (int row = 0; row < table->rowCount(); ++row) {
        const double amount = table->item(row, 2)->data(Qt::DisplayRole).toDouble();
        QVERIFY(amount <= previous);
        previous = amount;
        if (!table->isRowHidden(row)) {
            QCOMPARE(table->item(row, 3)->text(), status->currentText());
            ++visible;
        }
    }
    QCOMPARE(visible, 4);
}

void DemoGalleryTest::animationPausesWhenHiddenAndDisposesOnReturn()
{
    DemoGallery gallery;
    gallery.show();
    QVERIFY(gallery.openDemo(QStringLiteral("animation")));
    QPointer<QWidget> canvas = gallery.findChild<QWidget *>(QStringLiteral("demo-motion-canvas"));
    auto *play = gallery.findChild<QPushButton *>(QStringLiteral("demo-animation-play"));
    QVERIFY(canvas && play);
    QTRY_VERIFY(canvas->property("progress").toDouble() > 0);
    play->click();
    const double paused = canvas->property("progress").toDouble();
    QTest::qWait(70);
    QCOMPARE(canvas->property("progress").toDouble(), paused);
    play->click();
    QTRY_VERIFY(canvas->property("progress").toDouble() > paused);
    gallery.hide();
    const double hidden = canvas->property("progress").toDouble();
    QTest::qWait(70);
    QCOMPARE(canvas->property("progress").toDouble(), hidden);
    gallery.showGallery();
    QVERIFY(canvas.isNull());
}

void DemoGalleryTest::kanbanAddsAndMovesTasks()
{
    DemoGallery gallery;
    QVERIFY(gallery.openDemo(QStringLiteral("kanban")));
    auto *input = gallery.findChild<QLineEdit *>(QStringLiteral("demo-task-input"));
    auto *planned = gallery.findChild<QListWidget *>(QStringLiteral("demo-kanban-0"));
    auto *active = gallery.findChild<QListWidget *>(QStringLiteral("demo-kanban-1"));
    QVERIFY(input && planned && active);
    const int plannedBefore = planned->count();
    const int activeBefore = active->count();
    input->setText(QStringLiteral("验证示例交互"));
    gallery.findChild<QPushButton *>(QStringLiteral("demo-task-add"))->click();
    QCOMPARE(planned->count(), plannedBefore + 1);
    gallery.findChild<QPushButton *>(QStringLiteral("demo-task-advance"))->click();
    QCOMPARE(planned->count(), plannedBefore);
    QCOMPARE(active->count(), activeBefore + 1);
    QCOMPARE(active->currentItem()->text(), QStringLiteral("验证示例交互"));
}

void DemoGalleryTest::canvasDrawsAndClears()
{
    DemoGallery gallery;
    gallery.resize(900, 700);
    gallery.show();
    QVERIFY(gallery.openDemo(QStringLiteral("canvas")));
    auto *canvas = gallery.findChild<QWidget *>(QStringLiteral("demo-drawing-canvas"));
    QVERIFY(canvas);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(80, 80));
    QCOMPARE(canvas->property("strokeCount").toInt(), 1);
    gallery.findChild<QPushButton *>(QStringLiteral("demo-canvas-clear"))->click();
    QCOMPARE(canvas->property("strokeCount").toInt(), 0);
}

void DemoGalleryTest::everyExampleRendersAndReturns()
{
    DemoGallery gallery;
    gallery.resize(1150, 780);
    gallery.show();
    QCoreApplication::processEvents();
    const QString output = qEnvironmentVariable("Q_BROWSER_DEMO_SCREENSHOTS");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(gallery.grab().save(output + QStringLiteral("/catalog.png")));
    }
    const std::array<QString, 6> ids{QStringLiteral("widgets"), QStringLiteral("table"), QStringLiteral("charts"), QStringLiteral("animation"), QStringLiteral("kanban"), QStringLiteral("canvas")};
    for (const auto &id : ids) {
        QVERIFY(gallery.openDemo(id));
        QCoreApplication::processEvents();
        QPointer<QWidget> example = gallery.findChild<QWidget *>(QStringLiteral("demo-example-") + id);
        QVERIFY(example && example->isVisible());
        QVERIFY(!gallery.grab().isNull());
        if (!output.isEmpty()) QVERIFY(gallery.grab().save(output + QLatin1Char('/') + id + QStringLiteral(".png")));
        gallery.showGallery();
        QVERIFY(example.isNull());
    }
    gallery.resize(480, 700);
    QCoreApplication::processEvents();
    auto *scroll = gallery.findChild<QScrollArea *>();
    QVERIFY(scroll != nullptr);
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    if (!output.isEmpty()) QVERIFY(gallery.grab().save(output + QStringLiteral("/catalog-narrow.png")));
}

QTEST_MAIN(DemoGalleryTest)
#include "tst_demo_gallery.moc"
