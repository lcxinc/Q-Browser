#pragma once

#include "PerformanceSampler.h"
#include <QElapsedTimer>
#include <QWidget>
#include <functional>

class QLabel;
class QToolButton;
class QTreeWidget;
class QTimer;
class PerformanceThread;
class PerformanceHistory;

struct BrowserResourceCounts {
    int tabs = 0;
    qsizetype webPages = 0;
    int workerSurfaces = 0;
};

class PerformancePanel final : public QWidget {
    Q_OBJECT
public:
    explicit PerformancePanel(std::function<BrowserResourceCounts()> resources,
                              QWidget *parent = nullptr);
    ~PerformancePanel() override;

signals:
    void closeRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void updateSampling();
    void stopSampling();
    void displaySample(const PerformanceSample &sample);
    std::function<BrowserResourceCounts()> resources_;
    PerformanceThread *sampler_ = nullptr;
    QTimer *heartbeat_ = nullptr;
    QElapsedTimer beatClock_;
    qint64 maximumDelay_ = 0;
    quint64 generation_ = 0;
    QLabel *cpu_ = nullptr;
    QLabel *memory_ = nullptr;
    QLabel *details_ = nullptr;
    QLabel *resourcesLabel_ = nullptr;
    QLabel *latency_ = nullptr;
    QLabel *status_ = nullptr;
    QToolButton *pause_ = nullptr;
    QTreeWidget *processes_ = nullptr;
    PerformanceHistory *history_ = nullptr;
};
