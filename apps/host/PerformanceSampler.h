#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

struct ProcessCounters {
    quint64 pid = 0;
    quint64 parentPid = 0;
    quint64 creationTime = 0;
    quint64 cpuTicks = 0; // Windows process time, in 100 ns units.
    quint64 workingSet = 0;
    quint64 privateBytes = 0;
    QString name;
};

struct ProcessUsage {
    ProcessCounters counters;
    double cpuPercent = -1;
};

struct PerformanceSample {
    QVector<ProcessUsage> processes;
    double cpuPercent = -1;
    quint64 workingSet = 0;
    quint64 privateBytes = 0;
    int unavailableProcesses = 0;
    QString error;
};
Q_DECLARE_METATYPE(PerformanceSample)

class PerformanceSampler final {
public:
    PerformanceSample sample();
    static PerformanceSample calculate(const QVector<ProcessCounters> &current,
        const QHash<quint64, ProcessCounters> &previous,
        qint64 elapsedNanoseconds, int logicalProcessors);

private:
    QElapsedTimer clock_;
    QHash<quint64, ProcessCounters> previous_;
};
