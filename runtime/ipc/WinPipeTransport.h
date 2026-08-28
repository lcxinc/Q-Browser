#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
using HANDLE = void *;
#endif

enum class PipeIoStatus {
    Ok,
    TimedOut,
    Cancelled,
    PeerClosed,
    Failed,
};

struct PipeReadResult {
    PipeIoStatus status = PipeIoStatus::Failed;
    QByteArray bytes;
};

struct PipeWriteResult final
{
    PipeIoStatus status = PipeIoStatus::Failed;
    QString errorCode;
};

struct PipeWriteWork final
{
    QByteArray bytes;
    std::function<bool()> publicationGate;
    std::function<void(const PipeWriteResult &)> completion;
};

struct PipeWriterState;
struct PipeWriteOperation;

class PipeWriteCancellation final
{
public:
    PipeWriteCancellation() = default;

    [[nodiscard]] bool cancel() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;

private:
    friend class WinPipeTransport;
    PipeWriteCancellation(std::weak_ptr<PipeWriterState> writer,
                          std::weak_ptr<PipeWriteOperation> operation);

    std::weak_ptr<PipeWriterState> writer_;
    std::weak_ptr<PipeWriteOperation> operation_;
};

struct PipeWriteSubmission final
{
    bool accepted = false;
    QString errorCode;
    PipeWriteCancellation cancellation;
};

class WorkerPipeEnds final
{
public:
    WorkerPipeEnds() = default;
    ~WorkerPipeEnds();

    WorkerPipeEnds(const WorkerPipeEnds &) = delete;
    WorkerPipeEnds &operator=(const WorkerPipeEnds &) = delete;
    WorkerPipeEnds(WorkerPipeEnds &&other) noexcept;
    WorkerPipeEnds &operator=(WorkerPipeEnds &&other) noexcept;

    bool isValid() const noexcept;
    HANDLE nativeReadHandle() const noexcept;
    HANDLE nativeWriteHandle() const noexcept;
    void close() noexcept;

private:
    friend class WinPipePair;
    friend class WinPipeTransport;
    WorkerPipeEnds(HANDLE readHandle, HANDLE writeHandle);
    HANDLE takeReadHandle() noexcept;
    HANDLE takeWriteHandle() noexcept;

    HANDLE readHandle_ = nullptr;
    HANDLE writeHandle_ = nullptr;
};

class WinPipePair;

class WinPipeTransport final
{
public:
    WinPipeTransport() = default;
    ~WinPipeTransport();

    WinPipeTransport(const WinPipeTransport &) = delete;
    WinPipeTransport &operator=(const WinPipeTransport &) = delete;
    WinPipeTransport(WinPipeTransport &&other) noexcept;
    WinPipeTransport &operator=(WinPipeTransport &&other) noexcept;

    static WinPipePair createHostPair();
    static WinPipeTransport adoptWorkerEnds(WorkerPipeEnds &&ends);
    static std::optional<WinPipeTransport> adoptInheritedHandles(HANDLE readHandle,
                                                                HANDLE writeHandle);

    bool isValid() const noexcept;
    HANDLE nativeReadHandle() const noexcept;
    HANDLE nativeWriteHandle() const noexcept;
    PipeReadResult readSome(qsizetype maximumBytes, int timeoutMs);
    [[nodiscard]] PipeWriteSubmission submitWrite(PipeWriteWork work);
    bool writeAll(QByteArrayView bytes, int timeoutMs);
    PipeIoStatus lastStatus() const noexcept;
    void close() noexcept;

    static constexpr qsizetype maximumPendingWriteCount() noexcept
    {
        return 64;
    }
    static constexpr qsizetype maximumPendingWriteBytes() noexcept
    {
        return 4 * 1024 * 1024;
    }

private:
    WinPipeTransport(HANDLE readHandle, HANDLE writeHandle);

    HANDLE readHandle_ = nullptr;
    HANDLE writeHandle_ = nullptr;
    std::shared_ptr<PipeWriterState> writer_;
    PipeIoStatus lastStatus_ = PipeIoStatus::Ok;
};

class WinPipePair final
{
public:
    WinPipePair() = default;
    ~WinPipePair() = default;

    WinPipePair(const WinPipePair &) = delete;
    WinPipePair &operator=(const WinPipePair &) = delete;
    WinPipePair(WinPipePair &&other) noexcept;
    WinPipePair &operator=(WinPipePair &&other) noexcept;

    bool isValid() const noexcept;
    WinPipeTransport &host() noexcept;
    const WorkerPipeEnds &workerEnds() const noexcept;
    WinPipeTransport takeHost() noexcept;
    WorkerPipeEnds takeWorkerEnds() noexcept;

private:
    friend class WinPipeTransport;
    WinPipePair(WinPipeTransport host, WorkerPipeEnds workerEnds);

    WinPipeTransport host_;
    WorkerPipeEnds workerEnds_;
};
