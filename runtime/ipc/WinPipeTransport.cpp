#include "WinPipeTransport.h"

#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <atomic>
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

struct WriteContext {
    HANDLE pipe = nullptr;
    QByteArray bytes;
    std::atomic_bool cancelled = false;
    bool succeeded = false;
    DWORD error = ERROR_SUCCESS;
};

DWORD WINAPI pipeWriter(void *const rawContext) noexcept
{
    auto &context = *static_cast<WriteContext *>(rawContext);
    qsizetype writtenTotal = 0;
    while (writtenTotal < context.bytes.size()) {
        if (context.cancelled.load(std::memory_order_acquire)) {
            context.error = ERROR_OPERATION_ABORTED;
            return 0;
        }
        const DWORD wanted = static_cast<DWORD>(std::min<qsizetype>(
            context.bytes.size() - writtenTotal, static_cast<qsizetype>(64U * 1024U)));
        DWORD written = 0;
        if (!WriteFile(context.pipe,
                       context.bytes.constData() + writtenTotal,
                       wanted,
                       &written,
                       nullptr)) {
            context.error = GetLastError();
            return 0;
        }
        if (written == 0) {
            context.error = ERROR_WRITE_FAULT;
            return 0;
        }
        writtenTotal += static_cast<qsizetype>(written);
    }
    context.succeeded = true;
    return 0;
}

void cancelAndJoinWriter(const HANDLE thread, WriteContext &context) noexcept
{
    context.cancelled.store(true, std::memory_order_release);
    for (;;) {
        CancelSynchronousIo(thread);
        if (WaitForSingleObject(thread, 1) == WAIT_OBJECT_0) {
            return;
        }
    }
}
#endif

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
      lastStatus_(other.lastStatus_)
{
}

WinPipeTransport &WinPipeTransport::operator=(WinPipeTransport &&other) noexcept
{
    if (this != &other) {
        close();
        readHandle_ = std::exchange(other.readHandle_, nullptr);
        writeHandle_ = std::exchange(other.writeHandle_, nullptr);
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
    return validHandle(readHandle_) && validHandle(writeHandle_);
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

#ifdef Q_OS_WIN
    constexpr qsizetype maximumWriteBytes = 1024 * 1024 + 4;
    if (bytes.size() > maximumWriteBytes) {
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }

    WriteContext context;
    context.pipe = writeHandle_;
    context.bytes = QByteArray(bytes.data(), bytes.size());
    HANDLE thread = CreateThread(nullptr, 0, pipeWriter, &context, 0, nullptr);
    if (!validHandle(thread)) {
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, static_cast<DWORD>(timeoutMs));
    if (wait == WAIT_TIMEOUT) {
        cancelAndJoinWriter(thread, context);
        CloseHandle(thread);
        lastStatus_ = PipeIoStatus::TimedOut;
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        cancelAndJoinWriter(thread, context);
        CloseHandle(thread);
        lastStatus_ = PipeIoStatus::Failed;
        return false;
    }
    CloseHandle(thread);
    if (!context.succeeded) {
        lastStatus_ = peerClosedError(context.error) ? PipeIoStatus::PeerClosed
                                                     : PipeIoStatus::Failed;
        return false;
    }
    lastStatus_ = PipeIoStatus::Ok;
    return true;
#else
    Q_UNUSED(bytes)
    Q_UNUSED(timeoutMs)
    lastStatus_ = PipeIoStatus::Failed;
    return false;
#endif
}

PipeIoStatus WinPipeTransport::lastStatus() const noexcept
{
    return lastStatus_;
}

void WinPipeTransport::close() noexcept
{
    closeHandle(readHandle_);
    closeHandle(writeHandle_);
}

WinPipeTransport::WinPipeTransport(const HANDLE readHandle, const HANDLE writeHandle)
    : readHandle_(readHandle), writeHandle_(writeHandle)
{
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
