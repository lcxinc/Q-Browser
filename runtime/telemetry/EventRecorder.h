#pragma once

#include "SafeEvent.h"

#include <QString>

#include <memory>

struct EventRecorderConfig final
{
    QString directoryPath;
    QString fileName = QStringLiteral("events.jsonl");
    qint64 maximumFileBytes = 256 * 1024;
    int maximumArchivedFiles = 2;
    qsizetype maximumQueuedEvents = 256;
};

enum class EventRecordStatus
{
    Accepted,
    QueueFull,
    RecorderFailed,
    Stopped,
};

enum class EventRecorderError
{
    None,
    InvalidConfiguration,
    OpenFailed,
    WriteFailed,
    RotationFailed,
};

class EventRecorder final
{
public:
    explicit EventRecorder(EventRecorderConfig config);
    ~EventRecorder();

    EventRecorder(const EventRecorder &) = delete;
    EventRecorder &operator=(const EventRecorder &) = delete;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] EventRecordStatus record(const SafeEvent &event);
    [[nodiscard]] bool flush(int timeoutMs);
    [[nodiscard]] EventRecorderError lastError() const noexcept;
    [[nodiscard]] QString stableErrorCode() const;
    [[nodiscard]] qsizetype queuedEventCount() const noexcept;

private:
    class Private;
    std::unique_ptr<Private> d_;
};
