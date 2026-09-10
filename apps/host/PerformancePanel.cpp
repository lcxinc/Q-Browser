#include "PerformancePanel.h"

#include <QDateTime>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWaitCondition>
#include <algorithm>
#include <cmath>
#include <utility>

class PerformanceThread final : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;
    void stop()
    {
        {
            QMutexLocker lock(&mutex_);
            requestInterruption();
            wake_.wakeAll();
        }
        wait();
    }
signals:
    void sampled(const PerformanceSample &sample);
protected:
    void run() override
    {
        PerformanceSampler sampler;
        while (!isInterruptionRequested()) {
            emit sampled(sampler.sample());
            QMutexLocker lock(&mutex_);
            if (!isInterruptionRequested()) wake_.wait(&mutex_, 1000);
        }
    }
private:
    QMutex mutex_;
    QWaitCondition wake_;
};

namespace {
QString megabytes(quint64 bytes)
{
    return QStringLiteral("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
}
QString percent(double value)
{
    return value < 0 ? QStringLiteral("—")
                     : QStringLiteral("%1%").arg(value, 0, 'f', 1);
}
}

class PerformanceHistory final : public QWidget {
public:
    explicit PerformanceHistory(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(158);
        setAccessibleName(QStringLiteral("最近 60 次采样的 CPU 和内存趋势"));
    }
    void append(double cpu, double memory)
    {
        cpu_.append(cpu);
        memory_.append(memory);
        if (cpu_.size() > 60) { cpu_.removeFirst(); memory_.removeFirst(); }
        update();
    }
    void clear() { cpu_.clear(); memory_.clear(); update(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto chart = [&](const QVector<double> &values, int top,
                               double limit, const QColor &color, const QString &title) {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(QRect(0, top, width(), 20), Qt::AlignLeft, title);
            const QRectF graph(1, top + 24, width() - 2, 44);
            painter.fillRect(graph, palette().color(QPalette::AlternateBase));
            QPainterPath path;
            bool connected = false;
            for (qsizetype i = 0; i < values.size(); ++i) {
                if (values[i] < 0) { connected = false; continue; }
                const QPointF point(graph.left() + double(i) / 59 * graph.width(),
                    graph.bottom() - std::clamp(values[i] / limit, 0.0, 1.0) * graph.height());
                if (connected) path.lineTo(point); else path.moveTo(point);
                connected = true;
            }
            painter.setPen(QPen(color, 2));
            painter.drawPath(path);
        };
        chart(cpu_, 0, 100, QColor("#13a58c"), QStringLiteral("CPU 趋势 · 0–100%"));
        double ceiling = 1;
        for (double value : memory_) ceiling = std::max(ceiling, value);
        ceiling = std::ceil(ceiling);
        chart(memory_, 80, ceiling, QColor("#6482eb"),
              QStringLiteral("内存趋势 · 0–%1 MB").arg(ceiling, 0, 'f', 0));
    }
private:
    QVector<double> cpu_;
    QVector<double> memory_;
};

PerformancePanel::PerformancePanel(std::function<BrowserResourceCounts()> resources,
                                 QWidget *parent)
    : QWidget(parent), resources_(std::move(resources))
{
    qRegisterMetaType<PerformanceSample>();
    setObjectName(QStringLiteral("performance-panel"));
    setAccessibleName(QStringLiteral("性能监测"));
    setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    setFixedWidth(320);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("性能监测"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    title->setFont(titleFont);
    heading->addWidget(title, 1);
    pause_ = new QToolButton(this);
    pause_->setObjectName(QStringLiteral("performance-pause"));
    pause_->setText(QStringLiteral("暂停"));
    pause_->setCheckable(true);
    heading->addWidget(pause_);
    auto *close = new QToolButton(this);
    close->setObjectName(QStringLiteral("performance-close"));
    close->setText(QStringLiteral("×"));
    close->setAccessibleName(QStringLiteral("关闭性能监测"));
    heading->addWidget(close);
    layout->addLayout(heading);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    auto *content = new QVBoxLayout(body);
    content->setContentsMargins(0, 0, 4, 0);
    content->setSpacing(12);
    const auto label = [body, content](const QString &name, const QString &text) {
        auto *result = new QLabel(text, body);
        result->setObjectName(name);
        result->setWordWrap(true);
        result->setTextFormat(Qt::PlainText);
        content->addWidget(result);
        return result;
    };
    label(QString(), QStringLiteral("Q-Browser 及其子进程 · 每秒采样"));
    cpu_ = label(QStringLiteral("performance-cpu"), QStringLiteral("CPU  —"));
    memory_ = label(QStringLiteral("performance-memory"), QStringLiteral("内存  —"));
    for (auto *metric : {cpu_, memory_}) {
        QFont font = metric->font(); font.setPointSize(17); font.setBold(true);
        metric->setFont(font);
    }
    details_ = label(QStringLiteral("performance-private"), QStringLiteral("专用提交  —"));
    history_ = new PerformanceHistory(body);
    content->addWidget(history_);
    resourcesLabel_ = label(QStringLiteral("performance-resources"), QString());
    latency_ = label(QStringLiteral("performance-latency"), QStringLiteral("界面延迟  —"));
    latency_->setToolTip(QStringLiteral("100 ms 界面心跳在本次采样区间内的最大额外延迟，不是渲染帧率。"));
    processes_ = new QTreeWidget(body);
    processes_->setObjectName(QStringLiteral("performance-processes"));
    processes_->setAccessibleName(QStringLiteral("进程资源占用"));
    processes_->setColumnCount(3);
    processes_->setHeaderLabels({QStringLiteral("进程"), QStringLiteral("CPU"), QStringLiteral("内存")});
    processes_->setRootIsDecorated(false);
    processes_->setMinimumHeight(120);
    processes_->setMaximumHeight(190);
    processes_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    processes_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    processes_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    content->addWidget(processes_);
    status_ = label(QStringLiteral("performance-status"), QStringLiteral("等待采样…"));
    label(QString(), QStringLiteral("CPU 按全机逻辑核心归一化。内存为各进程工作集之和，共享页可能重复计数。趋势保留最近 60 次采样。"));
    content->addStretch();
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    heartbeat_ = new QTimer(this);
    heartbeat_->setInterval(100);
    heartbeat_->setTimerType(Qt::PreciseTimer);
    connect(heartbeat_, &QTimer::timeout, this, [this] {
        maximumDelay_ = std::max(maximumDelay_, std::max(qint64(0), beatClock_.restart() - 100));
    });
    connect(close, &QToolButton::clicked, this, &PerformancePanel::closeRequested);
    connect(pause_, &QToolButton::toggled, this, [this](bool paused) {
        pause_->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
        updateSampling();
    });
}

PerformancePanel::~PerformancePanel() { stopSampling(); }
void PerformancePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateSampling();
}
void PerformancePanel::hideEvent(QHideEvent *event)
{
    stopSampling();
    QWidget::hideEvent(event);
}
void PerformancePanel::stopSampling()
{
    ++generation_;
    heartbeat_->stop();
    if (sampler_) { sampler_->stop(); delete sampler_; sampler_ = nullptr; }
}
void PerformancePanel::updateSampling()
{
    stopSampling();
    if (!isVisible() || pause_->isChecked()) {
        status_->setText(QStringLiteral("已暂停 · 显示最后一次采样"));
        return;
    }
    history_->clear();
    maximumDelay_ = 0;
    beatClock_.start();
    heartbeat_->start();
    status_->setText(QStringLiteral("正在采样…"));
    sampler_ = new PerformanceThread(this);
    const auto generation = generation_;
    connect(sampler_, &PerformanceThread::sampled, this,
            [this, generation](const PerformanceSample &sample) {
        if (generation == generation_ && sampler_) displaySample(sample);
    });
    sampler_->start();
}
void PerformancePanel::displaySample(const PerformanceSample &sample)
{
    const bool valid = !sample.processes.isEmpty();
    cpu_->setText(QStringLiteral("CPU  %1").arg(percent(sample.cpuPercent)));
    memory_->setText(QStringLiteral("内存  %1").arg(valid ? megabytes(sample.workingSet) : QStringLiteral("—")));
    details_->setText(QStringLiteral("专用提交  %1").arg(valid ? megabytes(sample.privateBytes) : QStringLiteral("—")));
    history_->append(sample.cpuPercent, valid ? double(sample.workingSet) / (1024 * 1024) : -1);
    const auto counts = resources_();
    resourcesLabel_->setText(QStringLiteral("%1 个标签页  ·  %2 个网页实例\n%3 个 Worker 视图  ·  %4 个可读进程")
        .arg(counts.tabs).arg(counts.webPages).arg(counts.workerSurfaces).arg(sample.processes.size()));
    // Include a late heartbeat that is still queued behind this delivery.
    maximumDelay_ = std::max(maximumDelay_, std::max(qint64(0), beatClock_.elapsed() - 100));
    latency_->setText(QStringLiteral("界面延迟  %1 ms").arg(maximumDelay_));
    maximumDelay_ = 0;
    processes_->clear();
    for (const auto &usage : sample.processes) {
        auto *item = new QTreeWidgetItem(processes_, {usage.counters.name,
            percent(usage.cpuPercent), megabytes(usage.counters.workingSet)});
        item->setToolTip(0, QStringLiteral("%1\nPID %2 · 父进程 %3\n专用提交 %4")
            .arg(usage.counters.name).arg(usage.counters.pid).arg(usage.counters.parentPid)
            .arg(megabytes(usage.counters.privateBytes)));
    }
    QString status = QStringLiteral("更新于 %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    if (!sample.error.isEmpty()) status += QStringLiteral("\n") + sample.error;
    if (sample.unavailableProcesses > 0)
        status += QStringLiteral("\n%1 个进程已退出或无法读取，合计可能不完整").arg(sample.unavailableProcesses);
    status_->setText(status);
}

#include "PerformancePanel.moc"
