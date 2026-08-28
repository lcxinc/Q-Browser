#include "WinPipeTransport.h"

#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
#ifdef Q_OS_WIN
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
    return handle != nullptr;
#endif
}

void closeHandle(HANDLE &handle) noexcept
{
#ifdef Q_OS_WIN
    if (validHandle(handle)) {
        CloseHandle(handle);
    }
#endif
    handle = nullptr;
}

#ifdef Q_OS_WIN
bool peerClosedError(const DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED
           || error == ERROR_NO_DATA;
}

bool inheritedPipeHandleHasAccess(const HANDLE handle, const bool requiresRead)
{
    DWORD flags = 0;
    if (!validHandle(handle) || !GetHandleInformation(handle, &flags)
        || (flags & HANDLE_FLAG_INHERIT) == 0 || GetFileType(handle) != FILE_TYPE_PIPE) {
        return false;
    }
    DWORD transferred = 0;
    char ignored = 0;
    return requiresRead ? PeekNamedPipe(handle, nullptr, 0, nullptr, &transferred, nullptr)
                        : WriteFile(handle, &ignored, 0, &transferred, nullptr);
}

#endif

} // namespace

struct PipeWriteOperation final
{
    explicit PipeWriteOperation(PipeWriteWork work)
        : bytes(std::move(work.bytes)),
          publicationGate(std::move(work.publicationGate)),
          completion(std::move(work.completion))
    {
    }

    void finish(PipeWriteResult result) noexcept
    {
        if (finished.exchange(true, std::memory_order_acq_rel)) return;
        std::function<void(const PipeWriteResult &)> callback =
            std::move(completion);
        if (!callback) return;
        try {
            callback(result);
        } catch (...) {
        }
    }

    QByteArray bytes;
    std::function<bool()> publicationGate;
    std::function<void(const PipeWriteResult &)> completion;
    std::atomic_bool cancellationRequested{false};
    std::atomic_bool finished{false};
    bool writeStarted = false;
};

struct PipeWriterState final
{
    explicit PipeWriterState(const HANDLE adoptedPipe) : pipe(adoptedPipe) {}
    ~PipeWriterState()
    {
#ifdef Q_OS_WIN
        if (validHandle(thread)) CloseHandle(thread);
#endif
        thread = nullptr;
    }

    [[nodiscard]] bool cancel(
        const std::shared_ptr<PipeWriteOperation> &operation) noexcept
    {
        if (operation == nullptr
            || operation->finished.load(std::memory_order_acquire)) {
            return false;
        }
        std::shared_ptr<PipeWriteOperation> queuedCancellation;
        {
            std::lock_guard lock(mutex);
            if (operation->finished.load(std::memory_order_acquire)) return false;
            const auto queued = std::find(queue.begin(), queue.end(), operation);
            if (queued != queue.end()) {
                queuedCancellation = *queued;
                pendingBytes -= queuedCancellation->bytes.size();
                --pendingCount;
                queue.erase(queued);
            } else if (current == operation) {
                if (operation->writeStarted) return false;
                operation->cancellationRequested.store(
                    true, std::memory_order_release);
            } else {
                return false;
            }
        }
        if (queuedCancellation != nullptr) {
            queuedCancellation->finish(
                {PipeIoStatus::Cancelled,
                 QStringLiteral("ipc.send_cancelled")});
            return true;
        }
        condition.notify_all();
        return true;
    }

    HANDLE pipe = nullptr;
    HANDLE thread = nullptr;
#ifdef Q_OS_WIN
    DWORD threadId = 0;
#endif
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<PipeWriteOperation>> queue;
    std::shared_ptr<PipeWriteOperation> current;
    qsizetype pendingCount = 0;
    qsizetype pendingBytes = 0;
    bool closing = false;
};

namespace {

PipeWriteResult cancelledWrite()
{
    return {PipeIoStatus::Cancelled, QStringLiteral("ipc.send_cancelled")};
}

#ifdef Q_OS_WIN
PipeWriteResult writeOperation(
    PipeWriterState &writer,
    const std::shared_ptr<PipeWriteOperation> &operation) noexcept
{
    if (operation->cancellationRequested.load(std::memory_order_acquire)) {
        return cancelledWrite();
    }
    if (operation->publicationGate) {
        bool admitted = false;
        try {
            admitted = operation->publicationGate();
        } catch (...) {
            admitted = false;
        }
        if (!admitted) return cancelledWrite();
    }
    {
        std::lock_guard lock(writer.mutex);
        if (operation->cancellationRequested.load(std::memory_order_acquire)) {
            return cancelledWrite();
        }
        operation->writeStarted = true;
    }

    qsizetype writtenTotal = 0;
    while (writtenTotal < operation->bytes.size()) {
        if (operation->cancellationRequested.load(std::memory_order_acquire)) {
            return cancelledWrite();
        }
        const DWORD wanted = static_cast<DWORD>(std::min<qsizetype>(
            operation->bytes.size() - writtenTotal,
            static_cast<qsizetype>(64U * 1024U)));
        DWORD written = 0;
        if (!WriteFile(writer.pipe,
                       operation->bytes.constData() + writtenTotal,
                       wanted,
                       &written,
                       nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED
                || operation->cancellationRequested.load(
                    std::memory_order_acquire)) {
                return cancelledWrite();
            }
            if (peerClosedError(error)) {
                return {PipeIoStatus::PeerClosed,
                        QStringLiteral("ipc.send_peer_closed")};
            }
            return {PipeIoStatus::Failed,
                    QStringLiteral("ipc.send_failed")};
        }
        if (written == 0) {
            return {PipeIoStatus::Failed,
                    QStringLiteral("ipc.send_failed")};
        }
        writtenTotal += static_cast<qsizetype>(written);
    }
    return {PipeIoStatus::Ok, {}};
}

DWORD WINAPI persistentPipeWriter(void *const rawContext) noexcept
{
    std::unique_ptr<std::shared_ptr<PipeWriterState>> lifetime(
        static_cast<std::shared_ptr<PipeWriterState> *>(rawContext));
    const std::shared_ptr<PipeWriterState> writer = *lifetime;
    for (;;) {
        std::shared_ptr<PipeWriteOperation> operation;
        {
            std::unique_lock lock(writer->mutex);
            writer->condition.wait(lock, [&writer] {
                return writer->closing || !writer->queue.empty();
            });
            if (writer->queue.empty()) {
                if (writer->closing) return 0;
                continue;
            }
            operation = std::move(writer->queue.front());
            writer->queue.pop_front();
            writer->current = operation;
            if (writer->closing) {
                operation->cancellationRequested.store(
                    true, std::memory_order_release);
            }
        }

        const PipeWriteResult result = writeOperation(*writer, operation);
        {
            std::lock_guard lock(writer->mutex);
            if (writer->current == operation) writer->current.reset();
            writer->pendingBytes -= operation->bytes.size();
            --writer->pendingCount;
        }
        operation->finish(result);
    }
}

std::shared_ptr<PipeWriterState> startPipeWriter(const HANDLE pipe)
{
    if (!validHandle(pipe)) return {};
    auto writer = std::make_shared<PipeWriterState>(pipe);
    auto *const lifetime = new std::shared_ptr<PipeWriterState>(writer);
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, persistentPipeWriter, lifetime,
                                 CREATE_SUSPENDED, &threadId);
    if (!validHandle(thread)) {
        delete lifetime;
        return {};
    }
    writer->thread = thread;
    writer->threadId = threadId;
    if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
        TerminateThread(thread, ERROR_OPERATION_ABORTED);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        writer->thread = nullptr;
        delete lifetime;
        return {};
    }
    return writer;
}
#else
std::shared_ptr<PipeWriterState> startPipeWriter(const HANDLE)
{
    return {};
}
#endif

void closePipeWriter(const std::shared_ptr<PipeWriterState> &writer) noexcept
{
    if (writer == nullptr) return;
    HANDLE thread = nullptr;
    std::deque<std::shared_ptr<PipeWriteOperation>> selfCancelled;
#ifdef Q_OS_WIN
    DWORD threadId = 0;
#endif
    {
        std::lock_guard lock(writer->mutex);
        if (!writer->closing) {
            writer->closing = true;
            if (writer->current != nullptr) {
                writer->current->cancellationRequested.store(
                    true, std::memory_order_release);
            }
        }
        thread = writer->thread;
#ifdef Q_OS_WIN
        threadId = writer->threadId;
        if (GetCurrentThreadId() == threadId && writer->current == nullptr) {
            selfCancelled.swap(writer->queue);
            for (const auto &operation : selfCancelled) {
                writer->pendingBytes -= operation->bytes.size();
                --writer->pendingCount;
            }
        }
#endif
    }
    for (const auto &operation : selfCancelled) {
        operation->finish(cancelledWrite());
    }
    writer->condition.notify_all();
#ifdef Q_OS_WIN
    if (!validHandle(thread)) return;
    if (GetCurrentThreadId() == threadId) return;
    for (;;) {
        CancelSynchronousIo(thread);
        if (WaitForSingleObject(thread, 1) == WAIT_OBJECT_0) break;
    }
    CloseHandle(thread);
    std::lock_guard lock(writer->mutex);
    writer->thread = nullptr;
#else
    Q_UNUSED(thread)
#endif
}

} // namespace

WorkerPipeEnds::~WorkerPipeEnds()
{
    close();
}

WorkerPipeEnds::WorkerPipeEnds(WorkerPipeEnds &&other) noexcept
    : readHandle_(other.takeReadHandle()), writeHandle_(other.takeWriteHandle())
{
}

WorkerPipeEnds &WorkerPipeEnds::operator=(WorkerPipeEnds &&other) noexcept
{
    if (this != &other) {
        close();
        readHandle_ = other.takeReadHandle();
        writeHandle_ = other.takeWriteHandle();
    }
    return *this;
}

bool WorkerPipeEnds::isValid() const noexcept
{
    return validHandle(readHandle_) && validHandle(writeHandle_);
}

HANDLE WorkerPipeEnds::nativeReadHandle() const noexcept
{
    return readHandle_;
}

HANDLE WorkerPipeEnds::nativeWriteHandle() const noexcept
{
    return writeHandle_;
}

void WorkerPipeEnds::close() noexcept
{
    closeHandle(readHandle_);
    closeHandle(writeHandle_);
}

WorkerPipeEnds::WorkerPipeEnds(const HANDLE readHandle, const HANDLE writeHandle)
    : readHandle_(readHandle), writeHandle_(writeHandle)
{
}

HANDLE WorkerPipeEnds::takeReadHandle() noexcept
{
    return std::exchange(readHandle_, nullptr);
}

HANDLE WorkerPipeEnds::takeWriteHandle() noexcept
{
    return std::exchange(writeHandle_, nullptr);
}

WinPipeTransport::~WinPipeTransport()
{
    close();
}

WinPipeTransport::WinPipeTransport(WinPipeTransport &&other) noexcept
    : readHandle_(std::exchange(other.readHandle_, nullptr)),
      writeHandle_(std::exchange(other.writeHandle_, nullptr)),
      writer_(std::move(other.writer_)),
      lastStatus_(other.lastStatus_)
{
}

WinPipeTransport &WinPipeTransport::operator=(WinPipeTransport &&other) noexcept
{
    if (this != &other) {
        close();
        readHandle_ = std::exchange(other.readHandle_, nullptr);
        writeHandle_ = std::exchange(other.writeHandle_, nullptr);
        writer_ = std::move(other.writer_);
        lastStatus_ = other.lastStatus_;
    }
    return *this;
}

WinPipePair WinPipeTransport::createHostPair()
{
#ifdef Q_OS_WIN
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE hostRead = nullptr;
    HANDLE workerWrite = nullptr;
    if (!CreatePipe(&hostRead, &workerWrite, &attributes, 64U * 1024U)) {
        return {};
    }

    HANDLE workerRead = nullptr;
    HANDLE hostWrite = nullptr;
    if (!CreatePipe(&workerRead, &hostWrite, &attributes, 64U * 1024U)) {
        closeHandle(hostRead);
        closeHandle(workerWrite);
        return {};
    }

    if (!SetHandleInformation(hostRead, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(hostWrite, HANDLE_FLAG_INHERIT, 0)) {
        closeHandle(hostRead);
        closeHandle(hostWrite);
        closeHandle(workerRead);
        closeHandle(workerWrite);
        return {};
    }

    return {WinPipeTransport(hostRead, hostWrite), WorkerPipeEnds(workerRead, workerWrite)};
#else
    return {};
#endif
}

WinPipeTransport WinPipeTransport::adoptWorkerEnds(WorkerPipeEnds &&ends)
{
    WinPipeTransport transport(ends.takeReadHandle(), ends.takeWriteHandle());
#ifdef Q_OS_WIN
    if (!transport.isValid()
        || !SetHandleInformation(transport.nativeReadHandle(), HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(transport.nativeWriteHandle(), HANDLE_FLAG_INHERIT, 0)) {
        transport.close();
    }
#endif
    return transport;
}

std::optional<WinPipeTransport> WinPipeTransport::adoptInheritedHandles(
    const HANDLE readHandle,
    const HANDLE writeHandle)
{
#ifdef Q_OS_WIN
    if (readHandle == writeHandle
        || !inheritedPipeHandleHasAccess(readHandle, true)
        || !inheritedPipeHandleHasAccess(writeHandle, false)) {
        return std::nullopt;
    }

    HANDLE adoptedRead = nullptr;
    HANDLE adoptedWrite = nullptr;
    const HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process,
                         readHandle,
                         process,
                         &adoptedRead,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return std::nullopt;
    }
    if (!DuplicateHandle(process,
                         writeHandle,
                         process,
                         &adoptedWrite,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        closeHandle(adoptedRead);
        return std::nullopt;
    }

    CloseHandle(readHandle);
    CloseHandle(writeHandle);
    return WinPipeTransport(adoptedRead, adoptedWrite);
#else
    Q_UNUSED(readHandle)
    Q_UNUSED(writeHandle)
    return std::nullopt;
#endif
}

bool WinPipeTransport::isValid() const noexcept
{
    return validHandle(readHandle_) && validHandle(writeHandle_)
        && writer_ != nullptr;
}

HANDLE WinPipeTransport::nativeReadHandle() const noexcept
{
    return readHandle_;
}

HANDLE WinPipeTransport::nativeWriteHandle() const noexcept
{
    return writeHandle_;
}

PipeReadResult WinPipeTransport::readSome(const qsizetype maximumBytes, const int timeoutMs)
{
    if (!isValid() || maximumBytes <= 0 || timeoutMs < 0) {
        lastStatus_ = PipeIoStatus::Failed;
        return {lastStatus_, {}};
    }

#ifdef Q_OS_WIN
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readHandle_, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            lastStatus_ = peerClosedError(error) ? PipeIoStatus::PeerClosed
                                                 : PipeIoStatus::Failed;
            return {lastStatus_, {}};
        }
        if (available > 0) {
            const DWORD wanted = static_cast<DWORD>(
                std::min<quint64>(static_cast<quint64>(available),
                                  static_cast<quint64>(maximumBytes)));
            QByteArray bytes(static_cast<qsizetype>(wanted), Qt::Uninitialized);
            DWORD read = 0;
            if (!ReadFile(readHandle_, bytes.data(), wanted, &read, nullptr)) {
                const DWORD error = GetLastError();
                lastStatus_ = peerClosedError(error) ? PipeIoStatus::PeerClosed
                                                     : PipeIoStatus::Failed;
                return {lastStatus_, {}};
            }
            bytes.resize(static_cast<qsizetype>(read));
            lastStatus_ = PipeIoStatus::Ok;
            return {lastStatus_, bytes};
        }
        if (timer.elapsed() >= timeoutMs) {
            lastStatus_ = PipeIoStatus::TimedOut;
            return {lastStatus_, {}};
        }
        QThread::msleep(1);
    }
#else
    Q_UNUSED(maximumBytes)
    Q_UNUSED(timeoutMs)
    lastStatus_ = PipeIoStatus::Failed;
    return {lastStatus_, {}};
#endif
}

bool WinPipeTransport::writeAll(const QByteArrayView bytes, const int timeoutMs)
{
    if (!isValid() || bytes.isEmpty() || timeoutMs < 0) {
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }

    constexpr qsizetype maximumWriteBytes = 1024 * 1024 + 4;
    if (bytes.size() > maximumWriteBytes) {
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }

    struct SynchronousCompletion final
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<PipeWriteResult> result;
    };
    const auto completion = std::make_shared<SynchronousCompletion>();
    PipeWriteSubmission submission = submitWrite(PipeWriteWork{
        QByteArray(bytes.data(), bytes.size()),
        {},
        [completion](const PipeWriteResult &result) {
            {
                std::lock_guard lock(completion->mutex);
                completion->result = result;
            }
            completion->ready.notify_all();
        }});
    if (!submission.accepted) {
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }

    std::unique_lock lock(completion->mutex);
    const bool finished = completion->ready.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [&completion] { return completion->result.has_value(); });
    if (!finished) {
        lock.unlock();
        (void)submission.cancellation.cancel();
        lastStatus_ = PipeIoStatus::TimedOut;
        return false;
    }
    lastStatus_ = completion->result->status;
    return lastStatus_ == PipeIoStatus::Ok;
}

PipeWriteSubmission WinPipeTransport::submitWrite(PipeWriteWork work)
{
    constexpr qsizetype maximumWriteBytes = 1024 * 1024 + 4;
    if (work.bytes.isEmpty() || work.bytes.size() > maximumWriteBytes) {
        return {false, QStringLiteral("ipc.send_invalid"), {}};
    }
    const std::shared_ptr<PipeWriterState> writer = writer_;
    if (!isValid() || writer == nullptr) {
        return {false, QStringLiteral("ipc.send_closed"), {}};
    }

    auto operation = std::make_shared<PipeWriteOperation>(std::move(work));
    {
        std::lock_guard lock(writer->mutex);
        if (writer->closing) {
            return {false, QStringLiteral("ipc.send_closed"), {}};
        }
        if (writer->pendingCount >= maximumPendingWriteCount()
            || operation->bytes.size()
                    > maximumPendingWriteBytes() - writer->pendingBytes) {
            return {false, QStringLiteral("ipc.send_queue_full"), {}};
        }
        ++writer->pendingCount;
        writer->pendingBytes += operation->bytes.size();
        writer->queue.push_back(operation);
    }
    writer->condition.notify_one();
    return {true, {}, PipeWriteCancellation(writer, operation)};
}

PipeIoStatus WinPipeTransport::lastStatus() const noexcept
{
    return lastStatus_;
}

void WinPipeTransport::close() noexcept
{
    closePipeWriter(writer_);
    writer_.reset();
    closeHandle(readHandle_);
    closeHandle(writeHandle_);
}

WinPipeTransport::WinPipeTransport(const HANDLE readHandle, const HANDLE writeHandle)
    : readHandle_(readHandle), writeHandle_(writeHandle),
      writer_(startPipeWriter(writeHandle))
{
    if (writer_ == nullptr) {
        closeHandle(readHandle_);
        closeHandle(writeHandle_);
        lastStatus_ = PipeIoStatus::Failed;
    }
}

PipeWriteCancellation::PipeWriteCancellation(
    std::weak_ptr<PipeWriterState> writer,
    std::weak_ptr<PipeWriteOperation> operation)
    : writer_(std::move(writer)), operation_(std::move(operation))
{
}

bool PipeWriteCancellation::cancel() const noexcept
{
    const std::shared_ptr<PipeWriterState> writer = writer_.lock();
    const std::shared_ptr<PipeWriteOperation> operation = operation_.lock();
    return writer != nullptr && operation != nullptr
        && writer->cancel(operation);
}

bool PipeWriteCancellation::isValid() const noexcept
{
    return !writer_.expired() && !operation_.expired();
}

WinPipePair::WinPipePair(WinPipePair &&other) noexcept
    : host_(std::move(other.host_)), workerEnds_(other.takeWorkerEnds())
{
}

WinPipePair &WinPipePair::operator=(WinPipePair &&other) noexcept
{
    if (this != &other) {
        host_ = std::move(other.host_);
        workerEnds_ = other.takeWorkerEnds();
    }
    return *this;
}

bool WinPipePair::isValid() const noexcept
{
    return host_.isValid() && workerEnds_.isValid();
}

WinPipeTransport &WinPipePair::host() noexcept
{
    return host_;
}

const WorkerPipeEnds &WinPipePair::workerEnds() const noexcept
{
    return workerEnds_;
}

WinPipeTransport WinPipePair::takeHost() noexcept
{
    return std::move(host_);
}

WorkerPipeEnds WinPipePair::takeWorkerEnds() noexcept
{
    return std::move(workerEnds_);
}

WinPipePair::WinPipePair(WinPipeTransport host, WorkerPipeEnds workerEnds)
    : host_(std::move(host)), workerEnds_(std::move(workerEnds))
{
}
