#include "EventRecorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
QString errorCode(const EventRecorderError error)
{
    switch (error) {
    case EventRecorderError::None: return {};
    case EventRecorderError::InvalidConfiguration:
        return QStringLiteral("telemetry.invalid_recorder_configuration");
    case EventRecorderError::OpenFailed:
        return QStringLiteral("telemetry.file_open_failed");
    case EventRecorderError::WriteFailed:
        return QStringLiteral("telemetry.file_write_failed");
    case EventRecorderError::RotationFailed:
        return QStringLiteral("telemetry.file_rotation_failed");
    }
    return QStringLiteral("telemetry.recorder_failed");
}

bool validConfig(const EventRecorderConfig &config)
{
    static const QRegularExpression fileNamePattern(
        QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$)"));
    const QFileInfo directory(config.directoryPath);
    return directory.isDir() && !directory.isSymLink()
        && fileNamePattern.match(config.fileName).hasMatch()
        && config.maximumFileBytes >= 256
        && config.maximumFileBytes <= 16LL * 1024LL * 1024LL
        && config.maximumArchivedFiles >= 0
        && config.maximumArchivedFiles <= 10
        && config.maximumQueuedEvents > 0
        && config.maximumQueuedEvents <= 4096;
}
}

class EventRecorder::Private final
{
public:
    explicit Private(EventRecorderConfig value)
        : config(std::move(value))
    {
        if (!validConfig(config)) {
            error = EventRecorderError::InvalidConfiguration;
            return;
        }
        const QString active = QDir(config.directoryPath).filePath(config.fileName);
        for (int index = 0; index <= config.maximumArchivedFiles; ++index) {
            const QString path = index == 0
                ? active : active + QLatin1Char('.') + QString::number(index);
            const QFileInfo info(path);
            if (info.exists()
                && (!info.isFile() || info.isSymLink() || info.size() < 0
                    || info.size() > config.maximumFileBytes)) {
                error = EventRecorderError::InvalidConfiguration;
                return;
            }
        }
        worker = std::thread([this] { run(); });
    }

    ~Private()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        workAvailable.notify_all();
        if (worker.joinable()) worker.join();
    }

    bool rotate()
    {
        const QString active = QDir(config.directoryPath).filePath(config.fileName);
        if (config.maximumArchivedFiles == 0) {
            return !QFileInfo::exists(active) || QFile::remove(active);
        }
        const QString oldest = active + QLatin1Char('.')
            + QString::number(config.maximumArchivedFiles);
        if (QFileInfo::exists(oldest) && !QFile::remove(oldest)) return false;
        for (int index = config.maximumArchivedFiles - 1; index >= 1; --index) {
            const QString source = active + QLatin1Char('.') + QString::number(index);
            const QString destination = active + QLatin1Char('.')
                + QString::number(index + 1);
            if (QFileInfo::exists(source) && !QFile::rename(source, destination)) {
                return false;
            }
        }
        return !QFileInfo::exists(active)
            || QFile::rename(active, active + QStringLiteral(".1"));
    }

    EventRecorderError write(const QByteArray &line)
    {
        const QString path = QDir(config.directoryPath).filePath(config.fileName);
        const QFileInfo info(path);
        if (!QFileInfo(config.directoryPath).isDir()) {
            return EventRecorderError::OpenFailed;
        }
        if (line.size() > config.maximumFileBytes) {
            return EventRecorderError::WriteFailed;
        }
        if (info.exists() && (info.size() < 0
                              || info.size() + line.size() > config.maximumFileBytes)
            && !rotate()) {
            return EventRecorderError::RotationFailed;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            return EventRecorderError::OpenFailed;
        }
        if (file.write(line) != line.size() || !file.flush()) {
            return EventRecorderError::WriteFailed;
        }
        return EventRecorderError::None;
    }

    void run()
    {
        for (;;) {
            QByteArray line;
            {
                std::unique_lock lock(mutex);
                workAvailable.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                line = std::move(queue.front());
                queue.pop_front();
                writing = true;
            }
            const EventRecorderError writeError = write(line);
            {
                const std::lock_guard lock(mutex);
                writing = false;
                if (writeError != EventRecorderError::None) {
                    error = writeError;
                    queue.clear();
                }
            }
            drained.notify_all();
        }
    }

    EventRecorderConfig config;
    mutable std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable drained;
    std::deque<QByteArray> queue;
    std::thread worker;
    EventRecorderError error = EventRecorderError::None;
    bool stopping = false;
    bool writing = false;
};

EventRecorder::EventRecorder(EventRecorderConfig config)
    : d_(std::make_unique<Private>(std::move(config)))
{
}

EventRecorder::~EventRecorder() = default;

bool EventRecorder::isValid() const noexcept
{
    const std::lock_guard lock(d_->mutex);
    return d_->error == EventRecorderError::None && !d_->stopping;
}

EventRecordStatus EventRecorder::record(const SafeEvent &event)
{
    QByteArray line = event.toJson();
    line += '\n';
    {
        const std::lock_guard lock(d_->mutex);
        if (d_->stopping) return EventRecordStatus::Stopped;
        if (d_->error != EventRecorderError::None) {
            return EventRecordStatus::RecorderFailed;
        }
        if (static_cast<qsizetype>(d_->queue.size()) >= d_->config.maximumQueuedEvents) {
            return EventRecordStatus::QueueFull;
        }
        d_->queue.push_back(std::move(line));
    }
    d_->workAvailable.notify_one();
    return EventRecordStatus::Accepted;
}

bool EventRecorder::flush(const int timeoutMs)
{
    if (timeoutMs < 0) return false;
    std::unique_lock lock(d_->mutex);
    const bool finished = d_->drained.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this] {
            return d_->error != EventRecorderError::None
                || (d_->queue.empty() && !d_->writing);
        });
    return finished && d_->error == EventRecorderError::None
        && d_->queue.empty() && !d_->writing;
}

EventRecorderError EventRecorder::lastError() const noexcept
{
    const std::lock_guard lock(d_->mutex);
    return d_->error;
}

QString EventRecorder::stableErrorCode() const
{
    return errorCode(lastError());
}

qsizetype EventRecorder::queuedEventCount() const noexcept
{
    const std::lock_guard lock(d_->mutex);
    return static_cast<qsizetype>(d_->queue.size()) + (d_->writing ? 1 : 0);
}
