#include "DemoExamples.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
QPushButton *button(const QString &text, const char *name, QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}
QLabel *text(const QString &value, QWidget *parent)
{
    auto *result = new QLabel(value, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
QSlider *slider(const char *name, int minimum, int maximum, int value, QWidget *parent)
{
    auto *result = new QSlider(Qt::Horizontal, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setRange(minimum, maximum);
    result->setValue(value);
    return result;
}

QWidget *controlsDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(20);
    auto *form = new QFormLayout;
    form->setSpacing(14);
    auto *name = new QLineEdit(QStringLiteral("我的工作空间"), page);
    name->setObjectName(QStringLiteral("demo-control-name"));
    name->setMaxLength(80);
    auto *theme = new QComboBox(page);
    theme->setObjectName(QStringLiteral("demo-control-theme"));
    theme->addItems({QStringLiteral("薄荷绿"), QStringLiteral("海洋蓝"), QStringLiteral("暮光紫")});
    auto *intensity = slider("demo-control-intensity", 0, 100, 65, page);
    auto *notifications = new QCheckBox(QStringLiteral("启用消息提醒"), page);
    notifications->setChecked(true);
    form->addRow(QStringLiteral("工作空间名称"), name);
    form->addRow(QStringLiteral("主题颜色"), theme);
    form->addRow(QStringLiteral("专注程度"), intensity);
    form->addRow(QString(), notifications);
    layout->addLayout(form);
    auto *preview = new QFrame(page);
    preview->setObjectName(QStringLiteral("demo-control-preview"));
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(24, 24, 24, 24);
    auto *heading = text({}, preview);
    heading->setObjectName(QStringLiteral("demo-control-heading"));
    auto font = heading->font();
    font.setPointSize(20);
    font.setBold(true);
    heading->setFont(font);
    auto *summary = text({}, preview);
    previewLayout->addWidget(heading);
    previewLayout->addWidget(summary);
    layout->addWidget(preview);
    const auto update = [name, theme, intensity, notifications, preview, heading, summary] {
        static const std::array<const char *, 3> colors{"#218970", "#386cc9", "#8258bc"};
        const QString color = QString::fromLatin1(colors.at(theme->currentIndex()));
        preview->setStyleSheet(QStringLiteral("QFrame#demo-control-preview { border: 2px solid %1; border-radius: 12px; }").arg(color));
        heading->setText(name->text().trimmed().isEmpty() ? QStringLiteral("未命名工作空间") : name->text());
        summary->setText(QStringLiteral("专注程度 %1%   ·   消息提醒%2").arg(intensity->value()).arg(notifications->isChecked() ? QStringLiteral("已开启") : QStringLiteral("已关闭")));
    };
    QObject::connect(name, &QLineEdit::textChanged, page, update);
    QObject::connect(theme, &QComboBox::currentIndexChanged, page, update);
    QObject::connect(intensity, &QSlider::valueChanged, page, update);
    QObject::connect(notifications, &QCheckBox::toggled, page, update);
    update();
    auto *progress = new QProgressBar(page);
    progress->setObjectName(QStringLiteral("demo-control-progress"));
    progress->setValue(0);
    auto *apply = button(QStringLiteral("应用设置（演示）"), "demo-control-apply", page);
    auto *status = text(QStringLiteral("调整上方控件，预览会实时更新。"), page);
    auto *timer = new QTimer(page);
    timer->setInterval(25);
    QObject::connect(apply, &QPushButton::clicked, page, [progress, apply, status, timer] {
        progress->setValue(0);
        apply->setEnabled(false);
        status->setText(QStringLiteral("正在应用示例设置…"));
        timer->start();
    });
    QObject::connect(timer, &QTimer::timeout, page, [progress, apply, status, timer] {
        progress->setValue(progress->value() + 2);
        if (progress->value() == 100) {
            timer->stop();
            apply->setEnabled(true);
            status->setText(QStringLiteral("设置已应用。所有更改仅保留在本示例中。"));
        }
    });
    layout->addWidget(progress);
    layout->addWidget(apply, 0, Qt::AlignLeft);
    layout->addWidget(status);
    layout->addStretch();
    return page;
}

QWidget *tableDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *toolbar = new QHBoxLayout;
    auto *search = new QLineEdit(page);
    search->setObjectName(QStringLiteral("demo-table-search"));
    search->setPlaceholderText(QStringLiteral("搜索订单编号或客户"));
    auto *status = new QComboBox(page);
    status->setObjectName(QStringLiteral("demo-table-status"));
    status->addItems({QStringLiteral("全部状态"), QStringLiteral("已完成"), QStringLiteral("处理中"), QStringLiteral("待确认")});
    toolbar->addWidget(search, 1);
    toolbar->addWidget(status);
    layout->addLayout(toolbar);
    auto *table = new QTableWidget(12, 5, page);
    table->setObjectName(QStringLiteral("demo-orders-table"));
    table->setHorizontalHeaderLabels({QStringLiteral("订单编号"), QStringLiteral("客户"), QStringLiteral("金额 / 元"), QStringLiteral("状态"), QStringLiteral("日期")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setMinimumHeight(330);
    const std::array<QString, 6> customers{QStringLiteral("青禾设计"), QStringLiteral("云川科技"), QStringLiteral("远山工作室"), QStringLiteral("星河设备"), QStringLiteral("木白文化"), QStringLiteral("北辰制造")};
    const std::array<QString, 3> states{QStringLiteral("已完成"), QStringLiteral("处理中"), QStringLiteral("待确认")};
    for (int row = 0; row < 12; ++row) {
        table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("QB-%1").arg(1001 + row)));
        table->setItem(row, 1, new QTableWidgetItem(customers.at(row % 6)));
        auto *amount = new QTableWidgetItem;
        amount->setData(Qt::DisplayRole, 1280.0 + row * 375.5);
        table->setItem(row, 2, amount);
        table->setItem(row, 3, new QTableWidgetItem(states.at(row % 3)));
        table->setItem(row, 4, new QTableWidgetItem(QStringLiteral("2026-09-%1").arg(row + 1, 2, 10, QLatin1Char('0'))));
        table->setRowHeight(row, 38);
    }
    table->setSortingEnabled(true);
    table->sortItems(0, Qt::AscendingOrder);
    auto *count = text({}, page);
    count->setObjectName(QStringLiteral("demo-table-count"));
    const auto filter = [table, search, status, count] {
        int shown = 0;
        for (int row = 0; row < table->rowCount(); ++row) {
            const bool matches = (table->item(row, 0)->text() + table->item(row, 1)->text()).contains(search->text().trimmed(), Qt::CaseInsensitive)
                && (status->currentIndex() == 0 || table->item(row, 3)->text() == status->currentText());
            table->setRowHidden(row, !matches);
            if (matches) ++shown;
        }
        count->setText(QStringLiteral("显示 %1 / 12 条示例记录 · 点击列标题排序").arg(shown));
    };
    QObject::connect(search, &QLineEdit::textChanged, page, filter);
    QObject::connect(status, &QComboBox::currentIndexChanged, page, filter);
    QObject::connect(table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, page, [filter] { filter(); });
    filter();
    layout->addWidget(table, 1);
    layout->addWidget(count);
    return page;
}

class ChartCanvas final : public QWidget {
public:
    explicit ChartCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("demo-chart-canvas"));
        setMinimumSize(300, 320);
        setMouseTracking(true);
        setAccessibleName(QStringLiteral("月度数据图表"));
    }
    void setMetric(int value) { metric_ = value; update(); }
    void setBars(bool value) { bars_ = value; update(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        const QRectF plot(54, 30, width() - 78, height() - 76);
        const double maximum = metric_ == 0 ? 100 : 250;
        const auto values = data();
        QColor guideColor = palette().text().color();
        guideColor.setAlpha(28);
        p.setPen(palette().text().color());
        for (int grid = 0; grid <= 4; ++grid) {
            const qreal y = plot.bottom() - plot.height() * grid / 4;
            p.setPen(QPen(guideColor, 1));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(palette().text().color());
            p.drawText(QRectF(0, y - 10, 44, 20), Qt::AlignRight | Qt::AlignVCenter, QString::number(maximum * grid / 4));
        }
        QPainterPath line;
        const qreal step = plot.width() / 12;
        for (int i = 0; i < 12; ++i) {
            const qreal x = plot.left() + step * (i + 0.5);
            const qreal y = plot.bottom() - values.at(i) / maximum * plot.height();
            p.setPen(palette().text().color());
            p.drawText(QRectF(x - 18, plot.bottom() + 10, 36, 22), Qt::AlignCenter, QStringLiteral("%1月").arg(i + 1));
            if (bars_) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#38b99a"));
                p.drawRoundedRect(QRectF(x - step * 0.28, y, step * 0.56, plot.bottom() - y), 4, 4);
            } else {
                if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
            }
        }
        if (!bars_) {
            p.setPen(QPen(QColor("#6485ef"), 3));
            p.setBrush(Qt::NoBrush);
            p.drawPath(line);
            p.setBrush(QColor("#6485ef"));
            for (int i = 0; i < 12; ++i) p.drawEllipse(QPointF(plot.left() + step * (i + 0.5), plot.bottom() - values.at(i) / maximum * plot.height()), 4, 4);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int month = static_cast<int>((event->position().x() - 54) / ((width() - 78) / 12.0));
        if (month >= 0 && month < 12) QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("%1月：%2 %3").arg(month + 1).arg(data().at(month)).arg(metric_ == 0 ? QStringLiteral("万元") : QStringLiteral("笔")), this);
    }
private:
    std::array<double, 12> data() const
    {
        if (metric_ == 0) return {26, 38, 34, 51, 46, 62, 58, 73, 66, 81, 76, 92};
        return {72, 98, 86, 131, 116, 158, 149, 184, 165, 207, 192, 235};
    }
    int metric_ = 0;
    bool bars_ = false;
};
QWidget *chartsDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *toolbar = new QHBoxLayout;
    auto *metric = new QComboBox(page);
    metric->setObjectName(QStringLiteral("demo-chart-metric"));
    metric->addItems({QStringLiteral("营收 / 万元"), QStringLiteral("订单量 / 笔")});
    auto *type = new QComboBox(page);
    type->setObjectName(QStringLiteral("demo-chart-type"));
    type->addItems({QStringLiteral("折线图"), QStringLiteral("柱状图")});
    toolbar->addWidget(text(QStringLiteral("2026 年度概览"), page));
    toolbar->addStretch();
    toolbar->addWidget(metric);
    toolbar->addWidget(type);
    layout->addLayout(toolbar);
    auto *chart = new ChartCanvas(page);
    QObject::connect(metric, &QComboBox::currentIndexChanged, chart, [chart](int index) { chart->setMetric(index); });
    QObject::connect(type, &QComboBox::currentIndexChanged, chart, [chart](int index) { chart->setBars(index == 1); });
    layout->addWidget(chart, 1);
    layout->addWidget(text(QStringLiteral("示例数据 · 将鼠标移到图表上查看每月数值。"), page));
    return page;
}

class MotionCanvas final : public QWidget {
public:
    explicit MotionCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("demo-motion-canvas"));
        setMinimumSize(300, 290);
        timer_.setParent(this);
        timer_.setInterval(16);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            phase_ += clock_.restart() / 1000.0 * speed_;
            setProperty("progress", phase_);
            update();
        });
    }
    void setPlaying(bool value) { playing_ = value; synchronizeTimer(); }
    void setSpeed(int value) { speed_ = value / 100.0; }
    void reset() { phase_ = 0; setProperty("progress", phase_); clock_.restart(); update(); }
protected:
    void showEvent(QShowEvent *event) override { QWidget::showEvent(event); synchronizeTimer(); }
    void hideEvent(QHideEvent *event) override { timer_.stop(); QWidget::hideEvent(event); }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        const std::array<QString, 3> names{QStringLiteral("匀速 Linear"), QStringLiteral("平滑 InOutCubic"), QStringLiteral("弹跳 OutBounce")};
        const std::array<QEasingCurve::Type, 3> curves{QEasingCurve::Linear, QEasingCurve::InOutCubic, QEasingCurve::OutBounce};
        const std::array<QColor, 3> colors{QColor("#38b99a"), QColor("#6485ef"), QColor("#ebaa51")};
        const double cycle = std::fmod(phase_ / 2.0, 2.0);
        const double progress = cycle <= 1.0 ? cycle : 2.0 - cycle;
        QColor guideColor = palette().text().color();
        guideColor.setAlpha(35);
        for (int row = 0; row < 3; ++row) {
            const qreal y = 18 + (height() - 36) / 3.0 * (row + 0.65);
            p.setPen(palette().text().color());
            p.drawText(QPointF(24, y - 24), names.at(row));
            p.setPen(QPen(guideColor, 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(30, y), QPointF(width() - 30, y));
            p.setPen(Qt::NoPen);
            p.setBrush(colors.at(row));
            const qreal value = QEasingCurve(curves.at(row)).valueForProgress(progress);
            p.drawEllipse(QPointF(30 + (width() - 60) * value, y), 12, 12);
        }
    }
private:
    void synchronizeTimer() { if (playing_ && isVisible()) { clock_.restart(); timer_.start(); } else timer_.stop(); }
    QTimer timer_;
    QElapsedTimer clock_;
    double phase_ = 0;
    double speed_ = 1;
    bool playing_ = true;
};
QWidget *animationDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *motion = new MotionCanvas(page);
    layout->addWidget(motion, 1);
    auto *toolbar = new QHBoxLayout;
    auto *play = button(QStringLiteral("暂停"), "demo-animation-play", page);
    play->setCheckable(true);
    auto *reset = button(QStringLiteral("重新播放"), "demo-animation-reset", page);
    auto *speed = slider("demo-animation-speed", 25, 200, 100, page);
    auto *speedLabel = text(QStringLiteral("1.00×"), page);
    toolbar->addWidget(play);
    toolbar->addWidget(reset);
    toolbar->addWidget(text(QStringLiteral("播放速度"), page));
    toolbar->addWidget(speed, 1);
    toolbar->addWidget(speedLabel);
    QObject::connect(play, &QPushButton::toggled, motion, [motion, play](bool paused) { motion->setPlaying(!paused); play->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停")); });
    QObject::connect(reset, &QPushButton::clicked, motion, [motion, play] { motion->reset(); play->setChecked(false); });
    QObject::connect(speed, &QSlider::valueChanged, motion, [motion, speedLabel](int value) { motion->setSpeed(value); speedLabel->setText(QStringLiteral("%1×").arg(value / 100.0, 0, 'f', 2)); });
    layout->addLayout(toolbar);
    layout->addWidget(text(QStringLiteral("相同时间，不同节奏。切换到其他页签时动画自动暂停。"), page));
    return page;
}

QWidget *kanbanDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *toolbar = new QHBoxLayout;
    auto *input = new QLineEdit(page);
    input->setObjectName(QStringLiteral("demo-task-input"));
    input->setPlaceholderText(QStringLiteral("写下一项新任务…"));
    input->setMaxLength(120);
    auto *add = button(QStringLiteral("添加任务"), "demo-task-add", page);
    auto *advance = button(QStringLiteral("推进选中任务 →"), "demo-task-advance", page);
    toolbar->addWidget(input, 1);
    toolbar->addWidget(add);
    toolbar->addWidget(advance);
    layout->addLayout(toolbar);
    auto *columns = new QHBoxLayout;
    std::array<QListWidget *, 3> lists{};
    const std::array<QString, 3> titles{QStringLiteral("计划中"), QStringLiteral("进行中"), QStringLiteral("已完成")};
    for (int column = 0; column < 3; ++column) {
        auto *columnLayout = new QVBoxLayout;
        auto *heading = text({}, page);
        auto *list = new QListWidget(page);
        lists.at(column) = list;
        list->setObjectName(QStringLiteral("demo-kanban-%1").arg(column));
        list->setAccessibleName(titles.at(column));
        list->setMinimumWidth(120);
        list->setMinimumHeight(290);
        list->setDragDropMode(QAbstractItemView::DragDrop);
        list->setDefaultDropAction(Qt::MoveAction);
        list->setDragEnabled(true);
        list->setAcceptDrops(true);
        list->setDropIndicatorShown(true);
        list->setSpacing(6);
        list->setStyleSheet(QStringLiteral("QListWidget { border: 1px solid palette(midlight); border-radius: 8px; padding: 8px; } QListWidget::item { padding: 14px 8px; border: 1px solid palette(midlight); border-radius: 5px; } QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"));
        const auto update = [heading, list, title = titles.at(column)] { heading->setText(QStringLiteral("%1  ·  %2").arg(title).arg(list->count())); };
        QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, page, update);
        QObject::connect(list->model(), &QAbstractItemModel::rowsRemoved, page, update);
        update();
        columnLayout->addWidget(heading);
        columnLayout->addWidget(list);
        columns->addLayout(columnLayout, 1);
    }
    lists[0]->addItems({QStringLiteral("整理设计灵感"), QStringLiteral("准备交互原型"), QStringLiteral("编写项目说明")});
    lists[1]->addItems({QStringLiteral("实现示例中心"), QStringLiteral("优化窗口体验")});
    lists[2]->addItem(QStringLiteral("搭建开发环境"));
    for (QListWidget *list : lists) {
        QObject::connect(list, &QListWidget::itemSelectionChanged, page, [list, lists] {
            if (list->selectedItems().isEmpty()) return;
            for (QListWidget *other : lists) if (other != list) { QSignalBlocker guard(other); other->clearSelection(); }
        });
    }
    const auto addTask = [input, lists] {
        const QString value = input->text().trimmed();
        if (value.isEmpty()) return;
        lists[0]->addItem(value);
        lists[0]->setCurrentRow(lists[0]->count() - 1);
        input->clear();
    };
    QObject::connect(add, &QPushButton::clicked, page, addTask);
    QObject::connect(input, &QLineEdit::returnPressed, page, addTask);
    QObject::connect(advance, &QPushButton::clicked, page, [lists] {
        for (int column = 0; column < 2; ++column) {
            QListWidget *list = lists.at(column);
            if (list->selectedItems().isEmpty()) continue;
            auto *item = list->takeItem(list->row(list->selectedItems().first()));
            lists.at(column + 1)->addItem(item);
            lists.at(column + 1)->setCurrentItem(item);
            break;
        }
    });
    layout->addLayout(columns, 1);
    layout->addWidget(text(QStringLiteral("拖动卡片切换状态，或选中任务后点击“推进”。任务仅保存在当前示例中。"), page));
    return page;
}

class DrawingCanvas final : public QWidget {
public:
    explicit DrawingCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("demo-drawing-canvas"));
        setMinimumSize(300, 320);
        setCursor(Qt::CrossCursor);
        setAccessibleName(QStringLiteral("自由绘图画布"));
        setProperty("strokeCount", 0);
    }
    void setColor(const QColor &color) { color_ = color; }
    void setPenWidth(int value) { penWidth_ = value; }
    void clear() { strokes_.clear(); setProperty("strokeCount", 0); update(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        QColor guideColor = palette().text().color();
        guideColor.setAlpha(45);
        p.setPen(QPen(guideColor, 1));
        for (int x = 16; x < width(); x += 20) for (int y = 16; y < height(); y += 20) p.drawPoint(x, y);
        if (strokes_.isEmpty()) {
            p.setPen(palette().placeholderText().color());
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("按住鼠标左键，开始绘制"));
        }
        for (const auto &stroke : strokes_) {
            p.setPen(QPen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (stroke.points.size() == 1) p.drawPoint(stroke.points.first()); else p.drawPolyline(stroke.points);
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (strokes_.size() >= 200) strokes_.removeFirst();
        strokes_.append({QPolygonF{event->position()}, color_, penWidth_});
        setProperty("strokeCount", strokes_.size());
        update();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!event->buttons().testFlag(Qt::LeftButton) || strokes_.isEmpty()) return;
        if (strokes_.last().points.size() < 10000) strokes_.last().points.append(event->position());
        update();
    }
private:
    struct Stroke { QPolygonF points; QColor color; qreal width; };
    QVector<Stroke> strokes_;
    QColor color_{"#386cc9"};
    qreal penWidth_ = 4;
};
QWidget *canvasDemo(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *toolbar = new QHBoxLayout;
    auto *canvas = new DrawingCanvas(page);
    const std::array<std::pair<QString, QColor>, 4> colors{{{QStringLiteral("海洋蓝"), QColor("#386cc9")}, {QStringLiteral("薄荷绿"), QColor("#218970")}, {QStringLiteral("珊瑚红"), QColor("#d86377")}, {QStringLiteral("琥珀黄"), QColor("#d99127")}}};
    auto *color = new QComboBox(page);
    color->setObjectName(QStringLiteral("demo-canvas-color"));
    for (const auto &entry : colors) color->addItem(entry.first, entry.second);
    auto *width = slider("demo-canvas-width", 1, 18, 4, page);
    auto *clear = button(QStringLiteral("清空画布"), "demo-canvas-clear", page);
    toolbar->addWidget(color);
    toolbar->addWidget(text(QStringLiteral("笔宽"), page));
    toolbar->addWidget(width, 1);
    toolbar->addWidget(clear);
    QObject::connect(color, &QComboBox::currentIndexChanged, canvas, [color, canvas] { canvas->setColor(color->currentData().value<QColor>()); });
    QObject::connect(width, &QSlider::valueChanged, canvas, [canvas](int value) { canvas->setPenWidth(value); });
    QObject::connect(clear, &QPushButton::clicked, canvas, [canvas] { canvas->clear(); });
    layout->addLayout(toolbar);
    layout->addWidget(canvas, 1);
    layout->addWidget(text(QStringLiteral("使用不同颜色和笔宽自由创作。画布内容在返回示例中心后清除。"), page));
    return page;
}
}

QWidget *createDemoExample(const QString &id, QWidget *parent)
{
    if (id == QLatin1String("widgets")) return controlsDemo(parent);
    if (id == QLatin1String("table")) return tableDemo(parent);
    if (id == QLatin1String("charts")) return chartsDemo(parent);
    if (id == QLatin1String("animation")) return animationDemo(parent);
    if (id == QLatin1String("kanban")) return kanbanDemo(parent);
    if (id == QLatin1String("canvas")) return canvasDemo(parent);
    return nullptr;
}
