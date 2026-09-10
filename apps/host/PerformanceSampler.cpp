#include "PerformanceSampler.h"

#include <QCoreApplication>
#include <algorithm>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#include <psapi.h>

namespace {
quint64 ticks(const FILETIME &time)
{
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::optional<ProcessCounters> readProcess(quint64 pid, quint64 parent,
                                          const QString &name)
{
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (handle == nullptr) return std::nullopt;
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool valid = GetProcessTimes(handle, &created, &exited, &kernel, &user)
        && GetProcessMemoryInfo(handle,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
    CloseHandle(handle);
    if (!valid) return std::nullopt;
    return ProcessCounters{pid, parent, ticks(created), ticks(kernel) + ticks(user),
                           memory.WorkingSetSize, memory.PrivateUsage, name};
}
}
#endif

PerformanceSample PerformanceSampler::calculate(
    const QVector<ProcessCounters> &current,
    const QHash<quint64, ProcessCounters> &previous,
    qint64 elapsedNanoseconds, int logicalProcessors)
{
    PerformanceSample result;
    const bool intervalValid = elapsedNanoseconds > 0 && logicalProcessors > 0
        && !previous.isEmpty();
    for (const auto &process : current) {
        ProcessUsage usage{process, -1};
        const auto before = previous.constFind(process.pid);
        if (intervalValid && before != previous.cend()
            && before->creationTime == process.creationTime
            && process.cpuTicks >= before->cpuTicks) {
            usage.cpuPercent = std::clamp(
                double(process.cpuTicks - before->cpuTicks) * 10000.0
                    / double(elapsedNanoseconds) / logicalProcessors, 0.0, 100.0);
            if (result.cpuPercent < 0) result.cpuPercent = 0;
            result.cpuPercent += usage.cpuPercent;
        }
        result.workingSet += process.workingSet;
        result.privateBytes += process.privateBytes;
        result.processes.append(usage);
    }
    if (result.cpuPercent >= 0) result.cpuPercent = std::min(result.cpuPercent, 100.0);
    return result;
}

PerformanceSample PerformanceSampler::sample()
{
#ifdef Q_OS_WIN
    QVector<ProcessCounters> counters;
    int unavailable = 0;
    const auto root = readProcess(quint64(QCoreApplication::applicationPid()), 0,
                                 QStringLiteral("Q-Browser · Host"));
    if (!root) return {{}, -1, 0, 0, 0, QStringLiteral("无法读取主进程性能数据")};
    counters.append(*root);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    QString error;
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = QStringLiteral("子进程枚举失败，当前仅显示主进程");
    } else {
        struct Entry { quint64 pid; QString name; };
        QHash<quint64, QVector<Entry>> children;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                children[entry.th32ParentProcessID].append(
                    {entry.th32ProcessID, QString::fromWCharArray(entry.szExeFile)});
            } while (Process32NextW(snapshot, &entry));
        } else {
            error = QStringLiteral("子进程枚举失败，当前仅显示主进程");
        }
        CloseHandle(snapshot);
        QHash<quint64, bool> visited{{root->pid, true}};
        for (qsizetype i = 0; i < counters.size(); ++i) {
            const ProcessCounters parent = counters.at(i);
            for (const auto &child : children.value(parent.pid)) {
                if (visited.contains(child.pid)) continue;
                visited.insert(child.pid, true);
                const auto data = readProcess(child.pid, parent.pid, child.name);
                if (!data) { ++unavailable; continue; }
                // A recycled parent PID must not pull an unrelated process
                // into this application's tree.
                if (data->creationTime >= parent.creationTime) counters.append(*data);
            }
        }
    }
    const qint64 elapsed = clock_.isValid() ? clock_.nsecsElapsed() : 0;
    clock_.start();
    auto result = calculate(counters, previous_, elapsed,
                            int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    previous_.clear();
    for (const auto &process : counters) previous_.insert(process.pid, process);
    result.unavailableProcesses = unavailable;
    result.error = error;
    return result;
#else
    return {{}, -1, 0, 0, 0, QStringLiteral("此平台暂不支持进程 CPU / 内存采样")};
#endif
}
