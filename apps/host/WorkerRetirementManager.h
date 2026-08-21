#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

struct WorkerRetirementAttemptResult final
{
    bool succeeded = false;
    QString stableError;
};

struct WorkerRetirementStatus final
{
    qsizetype pending = 0;
    qsizetype running = 0;
    qsizetype fatal = 0;
    qsizetype activeThreads = 0;
    QStringList fatalErrors;

    [[nodiscard]] bool isIdle() const noexcept
    {
        return pending == 0 && running == 0 && fatal == 0
            && activeThreads == 0;
    }
};

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
void failNextWorkerRetirementThreadStartForTesting();
}
#endif

class WorkerRetirementManager final
{
public:
    using Ticket = quint64;
    using Attempt = std::function<WorkerRetirementAttemptResult()>;
    using Completion = std::function<void(bool, const QString &)>;

    [[nodiscard]] static WorkerRetirementManager &instance();

    [[nodiscard]] Ticket retire(Attempt attempt, Completion completion);
    [[nodiscard]] bool retryFatal();
    [[nodiscard]] WorkerRetirementStatus status() const;
    [[nodiscard]] bool flush(int timeoutMs) const;
    [[nodiscard]] bool shutdownChecked(int timeoutMs);

    WorkerRetirementManager(const WorkerRetirementManager &) = delete;
    WorkerRetirementManager &operator=(const WorkerRetirementManager &) = delete;

private:
    WorkerRetirementManager() = default;
    ~WorkerRetirementManager();

    struct Record;
    void start(const std::shared_ptr<Record> &record);
    void run(const std::shared_ptr<Record> &record);

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::unordered_map<Ticket, std::shared_ptr<Record>> records_;
    Ticket nextTicket_ = 1;
    qsizetype activeThreads_ = 0;
    bool shuttingDown_ = false;
};
