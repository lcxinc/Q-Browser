#include "WorkerRetirementManager.h"

#include <array>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::array retryDelays{
    std::chrono::milliseconds(25),
    std::chrono::milliseconds(50),
    std::chrono::milliseconds(100),
    std::chrono::milliseconds(200),
    std::chrono::milliseconds(400),
    std::chrono::milliseconds(800),
};
#ifdef Q_BROWSER_HOST_TESTING
std::atomic_bool failNextThreadStart{false};
#endif
}

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
void failNextWorkerRetirementThreadStartForTesting()
{
    failNextThreadStart.store(true, std::memory_order_release);
}
}
#endif

struct WorkerRetirementManager::Record final
{
    enum class State
    {
        Pending,
        Running,
        Fatal,
    };

    Ticket ticket = 0;
    Attempt attempt;
    Completion completion;
    State state = State::Pending;
    QString stableError;
};

WorkerRetirementManager &WorkerRetirementManager::instance()
{
    // Cleanup threads are deliberately detached so the manager must outlive all
    // static destruction. The OS reclaims this process-lifetime object if a
    // checked shutdown times out, while in-flight cleanup retains its state.
    static WorkerRetirementManager *const manager = new WorkerRetirementManager;
    return *manager;
}

WorkerRetirementManager::~WorkerRetirementManager() = default;

WorkerRetirementManager::Ticket WorkerRetirementManager::retire(
    Attempt attempt,
    Completion completion)
{
    if (!attempt) return 0;
    auto record = std::make_shared<Record>();
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return 0;
        record->ticket = nextTicket_++;
        record->attempt = std::move(attempt);
        record->completion = std::move(completion);
        records_.emplace(record->ticket, record);
    }
    start(record);
    return record->ticket;
}

void WorkerRetirementManager::start(const std::shared_ptr<Record> &record)
{
    {
        std::lock_guard lock(mutex_);
        if (record->state == Record::State::Running) return;
        record->state = Record::State::Running;
        ++activeThreads_;
    }
    try {
#ifdef Q_BROWSER_HOST_TESTING
        if (failNextThreadStart.exchange(false, std::memory_order_acq_rel)) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
#endif
        std::thread([this, record] { run(record); }).detach();
        return;
    } catch (...) {
    }

    Completion completion;
    const QString stableError =
        QStringLiteral("host.launch.retirement_thread_unavailable");
    {
        std::lock_guard lock(mutex_);
        --activeThreads_;
        record->state = Record::State::Fatal;
        record->stableError = stableError;
        completion = record->completion;
        changed_.notify_all();
    }
    if (completion) completion(false, stableError);
}

void WorkerRetirementManager::run(const std::shared_ptr<Record> &record)
{
    WorkerRetirementAttemptResult result;
    for (qsizetype attemptIndex = 0;; ++attemptIndex) {
        result = record->attempt();
        if (result.succeeded) break;
        if (attemptIndex >= static_cast<qsizetype>(retryDelays.size())) break;
        {
            std::lock_guard lock(mutex_);
            record->state = Record::State::Pending;
            record->stableError = result.stableError;
            changed_.notify_all();
        }
        std::this_thread::sleep_for(
            retryDelays[static_cast<std::size_t>(attemptIndex)]);
        {
            std::lock_guard lock(mutex_);
            record->state = Record::State::Running;
            changed_.notify_all();
        }
    }

    Completion completion;
    {
        std::lock_guard lock(mutex_);
        record->stableError = result.stableError;
        if (result.succeeded) {
            completion = record->completion;
            records_.erase(record->ticket);
        } else {
            record->state = Record::State::Fatal;
            completion = record->completion;
        }
    }
    if (completion) completion(result.succeeded, result.stableError);
    {
        std::lock_guard lock(mutex_);
        --activeThreads_;
        changed_.notify_all();
    }
}

bool WorkerRetirementManager::retryFatal()
{
    std::vector<std::shared_ptr<Record>> retry;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return false;
        for (const auto &[ticket, record] : records_) {
            static_cast<void>(ticket);
            if (record->state == Record::State::Fatal) retry.push_back(record);
        }
    }
    for (const auto &record : retry) start(record);
    return !retry.empty();
}

WorkerRetirementStatus WorkerRetirementManager::status() const
{
    std::lock_guard lock(mutex_);
    WorkerRetirementStatus result;
    result.activeThreads = activeThreads_;
    for (const auto &[ticket, record] : records_) {
        static_cast<void>(ticket);
        switch (record->state) {
        case Record::State::Pending: ++result.pending; break;
        case Record::State::Running: ++result.running; break;
        case Record::State::Fatal:
            ++result.fatal;
            result.fatalErrors.push_back(record->stableError);
            break;
        }
    }
    return result;
}

bool WorkerRetirementManager::flush(const int timeoutMs) const
{
    if (timeoutMs < 0) return false;
    std::unique_lock lock(mutex_);
    return changed_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return records_.empty() && activeThreads_ == 0; });
}

bool WorkerRetirementManager::shutdownChecked(const int timeoutMs)
{
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
    }
    return flush(timeoutMs);
}
