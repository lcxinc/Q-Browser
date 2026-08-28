#include "FileDialogCoordinator.h"
#include "FileDialogTestHooks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shobjidl.h>

#include <fcntl.h>
#include <io.h>
#elif defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace {
constexpr qsizetype maximumIdentityTokenBytes = 256;
std::atomic<quint64> nextOperationToken{1};

quint64 issueOperationToken() noexcept
{
    const quint64 value = nextOperationToken.fetch_add(
        1, std::memory_order_relaxed);
    if (value == 0 || value == std::numeric_limits<quint64>::max()) {
        std::terminate();
    }
    return value;
}

bool isSafeFileLeafName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255
        || name == QStringLiteral(".") || name == QStringLiteral("..")
        || QDir::isAbsolutePath(name) || name.endsWith(u'.')
        || name.endsWith(u' ') || name.contains(u'/')
        || name.contains(u'\\') || name.contains(u':')) {
        return false;
    }
    for (const QChar character : name) {
        if (character.isNull()
            || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return QFileInfo(name).fileName() == name;
}

#ifdef Q_OS_WIN
template<typename Interface>
class ComPointer final
{
public:
    ComPointer() = default;
    ~ComPointer()
    {
        if (value_ != nullptr) {
            value_->Release();
        }
    }
    ComPointer(const ComPointer &) = delete;
    ComPointer &operator=(const ComPointer &) = delete;

    [[nodiscard]] Interface *get() const noexcept { return value_; }
    [[nodiscard]] Interface **put() noexcept { return &value_; }
    [[nodiscard]] Interface *operator->() const noexcept { return value_; }
    void attach(Interface *value) noexcept { value_ = value; }

private:
    Interface *value_ = nullptr;
};

class UniqueHandle final
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept
    {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool isValid() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct WindowsFileIdentity final
{
    quint64 volume = 0;
    QByteArray fileId;

    [[nodiscard]] bool operator==(
        const WindowsFileIdentity &) const noexcept = default;
};

QByteArray identityToken(const WindowsFileIdentity &identity)
{
    return QByteArray("win-id128:")
        + QByteArray::number(static_cast<qulonglong>(identity.volume), 16)
        + ':' + identity.fileId.toHex();
}

bool queryPlainFileHandle(HANDLE handle,
                          WindowsFileIdentity &identity,
                          qint64 &size)
{
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_ID_INFO identityInformation{};
    LARGE_INTEGER fileSize{};
    const bool valid = GetFileType(handle) == FILE_TYPE_DISK
        && GetFileInformationByHandleEx(handle,
                                        FileAttributeTagInfo,
                                        &tag,
                                        static_cast<DWORD>(sizeof(tag)))
            != FALSE
        && (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT
                                  | FILE_ATTRIBUTE_DIRECTORY))
            == 0U
        && GetFileInformationByHandleEx(handle,
                                        FileIdInfo,
                                        &identityInformation,
                                        static_cast<DWORD>(
                                            sizeof(identityInformation)))
            != FALSE
        && GetFileSizeEx(handle, &fileSize) != FALSE
        && fileSize.QuadPart >= 0;
    if (!valid) {
        return false;
    }
    identity = {
        static_cast<quint64>(identityInformation.VolumeSerialNumber),
        QByteArray(
            reinterpret_cast<const char *>(identityInformation.FileId.Identifier),
            static_cast<qsizetype>(sizeof(FILE_ID_128)))};
    size = fileSize.QuadPart;
    return true;
}

bool validateBoundPath(const QString &path,
                       HANDLE selectedHandle,
                       const WindowsFileIdentity &selectedIdentity)
{
    std::vector<UniqueHandle> heldAncestors;
    QString current = QFileInfo(path).dir().absolutePath();
    for (;;) {
        UniqueHandle handle(CreateFileW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (!handle.isValid()) {
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        const bool valid = GetFileInformationByHandleEx(
                               handle.get(),
                               FileAttributeTagInfo,
                               &tag,
                               static_cast<DWORD>(sizeof(tag)))
                != FALSE
            && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
        if (!valid) {
            return false;
        }
        heldAncestors.push_back(std::move(handle));
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    UniqueHandle rebound(CreateFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    WindowsFileIdentity reboundIdentity;
    qint64 reboundSize = 0;
    return rebound.isValid()
        && queryPlainFileHandle(rebound.get(), reboundIdentity, reboundSize)
        && reboundIdentity == selectedIdentity
        && GetFileType(selectedHandle) == FILE_TYPE_DISK;
}
#endif

QByteArray stableStreamIdentityToken(QIODevice &stream)
{
    auto *const file = dynamic_cast<QFile *>(&stream);
    if (file == nullptr || file->handle() < 0) {
        return {};
    }
#ifdef Q_OS_WIN
    const intptr_t native = _get_osfhandle(static_cast<int>(file->handle()));
    if (native == -1) {
        return {};
    }
    WindowsFileIdentity identity;
    qint64 size = 0;
    if (!queryPlainFileHandle(reinterpret_cast<HANDLE>(native), identity, size)) {
        return {};
    }
    return identityToken(identity);
#elif defined(Q_OS_UNIX)
    struct stat information {};
    if (::fstat(static_cast<int>(file->handle()), &information) != 0
        || !S_ISREG(information.st_mode)) {
        return {};
    }
    return QByteArray("unix:")
        + QByteArray::number(static_cast<qulonglong>(information.st_dev), 16)
        + ':'
        + QByteArray::number(static_cast<qulonglong>(information.st_ino), 16);
#else
    return {};
#endif
}

FileDialogResult openStablePath(const QString &path, const qint64 maximumBytes)
{
    if (path.isEmpty() || maximumBytes < 0) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
#ifdef Q_OS_WIN
    UniqueHandle file(CreateFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    WindowsFileIdentity identity;
    qint64 size = 0;
    if (!file.isValid() || !queryPlainFileHandle(file.get(), identity, size)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
#ifdef Q_BROWSER_BROKER_TESTING
    const auto hooks = qbrowser_broker_testing::fileDialogTestHooks();
    if (hooks.afterNativeHandleOpened) {
        hooks.afterNativeHandleOpened(path);
    }
#endif
    if (!validateBoundPath(path, file.get(), identity)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    if (size > maximumBytes) {
        return FileDialogResult::error(FileDialogStatus::TooLarge);
    }

    const HANDLE nativeHandle = file.release();
    const int descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>(nativeHandle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(nativeHandle);
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    auto stream = std::make_unique<QFile>();
    if (!stream->open(descriptor,
                      QIODevice::ReadOnly,
                      QFileDevice::AutoCloseHandle)) {
        _close(descriptor);
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return FileDialogResult::opened(QFileInfo(path).fileName(),
                                    size,
                                    std::move(stream),
                                    identityToken(identity));
#else
    const QFileInfo information(path);
    if (information.isSymLink() || !information.isFile()
        || information.size() > maximumBytes) {
        return FileDialogResult::error(information.size() > maximumBytes
                                           ? FileDialogStatus::TooLarge
                                           : FileDialogStatus::Failed);
    }
    auto stream = std::make_unique<QFile>(path);
    if (!stream->open(QIODevice::ReadOnly)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const QByteArray identity = stableStreamIdentityToken(*stream);
    if (identity.isEmpty()) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return FileDialogResult::opened(information.fileName(),
                                    information.size(),
                                    std::move(stream),
                                    identity);
#endif
}

#ifdef Q_OS_WIN
class CoordinatorDialogEventHandler final : public IFileDialogEvents
{
public:
    explicit CoordinatorDialogEventHandler(const qint64 maximumBytes)
        : maximumBytes_(maximumBytes)
    {
    }

    [[nodiscard]] FileDialogResult takeResult()
    {
        if (result_.status != FileDialogStatus::Opened) {
            return FileDialogResult::error(status_);
        }
        return std::move(result_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId,
                                              void **object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (interfaceId == IID_IUnknown || interfaceId == IID_IFileDialogEvents) {
            *object = static_cast<IFileDialogEvents *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *dialog) override
    {
        ComPointer<IShellItem> item;
        if (dialog == nullptr || FAILED(dialog->GetResult(item.put()))) {
            status_ = FileDialogStatus::Failed;
            return S_FALSE;
        }
        SFGAOF attributes = 0;
        if (FAILED(item->GetAttributes(
                SFGAO_FILESYSTEM | SFGAO_LINK | SFGAO_FOLDER, &attributes))
            || (attributes & SFGAO_FILESYSTEM) == 0U
            || (attributes & (SFGAO_LINK | SFGAO_FOLDER)) != 0U) {
            status_ = FileDialogStatus::Failed;
            return S_FALSE;
        }
        PWSTR selectedPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath))
            || selectedPath == nullptr) {
            status_ = FileDialogStatus::Failed;
            return S_FALSE;
        }
        const QString path = QString::fromWCharArray(selectedPath);
        CoTaskMemFree(selectedPath);
        result_ = openStablePath(path, maximumBytes_);
        status_ = result_.status;
        return status_ == FileDialogStatus::Opened ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(
        IFileDialog *,
        IShellItem *,
        FDE_SHAREVIOLATION_RESPONSE *response) override
    {
        if (response != nullptr) {
            *response = FDESVR_DEFAULT;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(
        IFileDialog *,
        IShellItem *,
        FDE_OVERWRITE_RESPONSE *response) override
    {
        if (response != nullptr) {
            *response = FDEOR_DEFAULT;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1U};
    qint64 maximumBytes_ = 0;
    FileDialogStatus status_ = FileDialogStatus::Failed;
    FileDialogResult result_;
};
#endif

struct CoordinatorShowResult final
{
    FileDialogStatus status = FileDialogStatus::Failed;
    QString selectedPath;
};
}

class FileDialogCoordinatorControl final
    : public std::enable_shared_from_this<FileDialogCoordinatorControl>
{
public:
    FileDialogCoordinatorControl() = default;
    ~FileDialogCoordinatorControl() = default;
    FileDialogCoordinatorControl(const FileDialogCoordinatorControl &) = delete;
    FileDialogCoordinatorControl &operator=(
        const FileDialogCoordinatorControl &) = delete;

    void start();
    [[nodiscard]] std::optional<FileDialogOperation> openAsync(
        FileDialogOpenRequest request,
        FileDialogCompletion completion);
    [[nodiscard]] bool requestCancel(
        const FileDialogOperationToken &token) noexcept;
    void shutdown() noexcept;

private:
    enum class OperationState {
        Idle,
        Showing,
        Terminal,
    };

    struct Operation final
    {
        Operation(FileDialogOperationToken operationToken,
                  const qint64 approvedMaximumBytes,
                  FileDialogCompletion operationCompletion)
            : token(operationToken),
              maximumBytes(approvedMaximumBytes),
              completion(std::move(operationCompletion))
        {
        }

        FileDialogOperationToken token;
        qint64 maximumBytes = 0;
        FileDialogCompletion completion;
        OperationState state = OperationState::Idle;
        std::mutex fakeMutex;
        std::condition_variable fakeCondition;
        std::optional<CoordinatorShowResult> fakeResult;
        bool fakeShowFinished = false;
        bool cancelDelivered = false;
    };

    struct CallbackWork final
    {
        FileDialogOperationToken token;
        FileDialogSelection selection;
        FileDialogCompletion completion;
    };

    [[nodiscard]] bool finishLocked(
        const std::shared_ptr<Operation> &operation,
        FileDialogSelection selection) noexcept;
    void finishAndReleaseOperation(
        const std::shared_ptr<Operation> &operation,
        FileDialogSelection selection) noexcept;
    void releaseOperationReservation(
        const std::shared_ptr<Operation> &operation) noexcept;
    [[nodiscard]] bool releaseOperationReservationLocked(
        const std::shared_ptr<Operation> &operation) noexcept;
    void notifyOperationQuiesced(bool released) noexcept;
    void enqueueCallback(CallbackWork work) noexcept;
    void callbackLoop() noexcept;
    void staLoop() noexcept;
    void runNextOperation() noexcept;
    [[nodiscard]] FileDialogSelection runOperation(
        const std::shared_ptr<Operation> &operation) noexcept;
    [[nodiscard]] FileDialogSelection materializeSelection(
        FileDialogResult result,
        qint64 maximumBytes,
        const std::function<void(qint64)> &beforeRead,
        const std::function<void()> &beforeEncode) noexcept;
    void setFakeResult(const std::shared_ptr<Operation> &operation,
                       CoordinatorShowResult result) noexcept;
    void beginShutdown() noexcept;
#ifdef Q_BROWSER_BROKER_TESTING
    [[nodiscard]] CoordinatorShowResult runFakeShow(
        const std::shared_ptr<Operation> &operation,
        const qbrowser_broker_testing::FileDialogTestShowHook &show)
        noexcept;
#endif
#ifdef Q_OS_WIN
    [[nodiscard]] FileDialogResult runNativeDialog(
        const std::shared_ptr<Operation> &operation) noexcept;
    [[nodiscard]] bool postWindowMessage(UINT message) noexcept;
    void handleStartMessage() noexcept;
    void handleCancelMessage() noexcept;
    void handleShutdownMessage() noexcept;
    void deliverPhysicalCancellation() noexcept;
    static LRESULT CALLBACK windowProcedure(HWND window,
                                             UINT message,
                                             WPARAM word,
                                             LPARAM parameter) noexcept;
#else
    void wakeSta() noexcept;
#endif

    std::mutex stateMutex_;
    bool stopping_ = false;
    std::shared_ptr<Operation> activeOperation_;
    std::deque<std::shared_ptr<Operation>> pendingOperations_;

    std::mutex cancelMutex_;
    std::deque<quint64> pendingCancellations_;

    std::mutex callbackMutex_;
    std::condition_variable callbackCondition_;
    std::deque<CallbackWork> callbacks_;
    bool callbackStopping_ = false;

    std::mutex startMutex_;
    std::condition_variable startCondition_;
    bool staReady_ = false;
    bool staAvailable_ = false;

#ifndef Q_OS_WIN
    std::mutex staMutex_;
    std::condition_variable staCondition_;
    bool staWake_ = false;
#else
    std::atomic<HWND> cancelWindow_{nullptr};
    std::shared_ptr<Operation> physicalOperation_;
    IFileOpenDialog *physicalDialog_ = nullptr;
#endif

    std::thread staThread_;
    std::thread callbackThread_;
    std::thread::id staThreadId_;
    std::thread::id callbackThreadId_;
    std::mutex shutdownMutex_;
    bool shutdownComplete_ = false;
};

FileDialogOperationToken::FileDialogOperationToken(const quint64 value) noexcept
    : value_(value)
{
}

FileDialogCancellationHandle::FileDialogCancellationHandle(
    std::weak_ptr<FileDialogCoordinatorControl> control,
    FileDialogOperationToken token) noexcept
    : control_(std::move(control)), token_(token)
{
}

bool FileDialogCancellationHandle::cancel() const noexcept
{
    const std::shared_ptr<FileDialogCoordinatorControl> control = control_.lock();
    return control != nullptr && control->requestCancel(token_);
}

FileDialogOpenRequest::FileDialogOpenRequest(
    const PreparedFileRequest &prepared,
    OwnerPredicate ownerPredicate)
    : maximumBytes_(prepared.maximumBytes()),
      ownerPredicate_(std::move(ownerPredicate))
{
}

FileDialogCoordinator::FileDialogCoordinator()
    : control_(std::make_shared<FileDialogCoordinatorControl>())
{
    control_->start();
}

FileDialogCoordinator::~FileDialogCoordinator()
{
    shutdown();
}

std::optional<FileDialogOperation> FileDialogCoordinator::openAsync(
    FileDialogOpenRequest request,
    FileDialogCompletion completion)
{
    return control_->openAsync(std::move(request), std::move(completion));
}

bool FileDialogCoordinator::cancel(
    const FileDialogOperationToken &token) noexcept
{
    return control_->requestCancel(token);
}

void FileDialogCoordinator::shutdown() noexcept
{
    if (control_ != nullptr) {
        control_->shutdown();
    }
}

void FileDialogCoordinatorControl::start()
{
    callbackThread_ = std::thread([this] { callbackLoop(); });
    callbackThreadId_ = callbackThread_.get_id();
    try {
        staThread_ = std::thread([this] { staLoop(); });
        staThreadId_ = staThread_.get_id();
    } catch (...) {
        {
            const std::scoped_lock lock(callbackMutex_);
            callbackStopping_ = true;
        }
        callbackCondition_.notify_all();
        callbackThread_.join();
        throw;
    }
    std::unique_lock lock(startMutex_);
    startCondition_.wait(lock, [this] { return staReady_; });
}

std::optional<FileDialogOperation> FileDialogCoordinatorControl::openAsync(
    FileDialogOpenRequest request,
    FileDialogCompletion completion)
{
    if (!completion || request.maximumBytes_ <= 0) {
        return std::nullopt;
    }
    bool admitted = false;
    try {
        admitted = request.ownerPredicate_ && request.ownerPredicate_();
    } catch (...) {
        admitted = false;
    }

    const FileDialogOperationToken token(issueOperationToken());
    auto operation = std::make_shared<Operation>(
        token, request.maximumBytes_, std::move(completion));
    const FileDialogOperation publicOperation{
        token,
        FileDialogCancellationHandle(weak_from_this(), token)};

    FileDialogStatus immediateStatus = FileDialogStatus::Opened;
    {
        const std::scoped_lock lock(stateMutex_);
        if (stopping_) {
            return std::nullopt;
        }
        if (!admitted) {
            immediateStatus = FileDialogStatus::Denied;
        } else if (activeOperation_ != nullptr) {
            immediateStatus = FileDialogStatus::Busy;
        } else if (!staAvailable_) {
            immediateStatus = FileDialogStatus::Failed;
        } else {
            operation->state = OperationState::Showing;
            activeOperation_ = operation;
            pendingOperations_.push_back(operation);
        }
        if (immediateStatus != FileDialogStatus::Opened) {
#ifdef Q_BROWSER_BROKER_TESTING
            const auto hooks = qbrowser_broker_testing::fileDialogTestHooks();
            if (hooks.coordinatorBeforeImmediateCompletion) {
                try {
                    hooks.coordinatorBeforeImmediateCompletion();
                } catch (...) {
                }
            }
#endif
            FileDialogSelection selection;
            selection.status = immediateStatus;
            (void)finishLocked(operation, std::move(selection));
            return publicOperation;
        }
    }

#ifdef Q_OS_WIN
    if (!postWindowMessage(WM_APP + 0x351U)) {
        FileDialogSelection selection;
        selection.status = FileDialogStatus::Failed;
        finishAndReleaseOperation(operation, std::move(selection));
    }
#else
    wakeSta();
#endif
    return publicOperation;
}

bool FileDialogCoordinatorControl::requestCancel(
    const FileDialogOperationToken &token) noexcept
{
    std::shared_ptr<Operation> operation;
    {
        const std::scoped_lock lock(stateMutex_);
        if (activeOperation_ == nullptr
            || !(activeOperation_->token == token)) {
            return false;
        }
        operation = activeOperation_;
        FileDialogSelection selection;
        selection.status = FileDialogStatus::Cancelled;
        if (!finishLocked(operation, std::move(selection))) {
            return false;
        }
    }
    {
        const std::scoped_lock lock(cancelMutex_);
        pendingCancellations_.push_back(token.value_);
    }
#ifdef Q_OS_WIN
    (void)postWindowMessage(WM_APP + 0x352U);
#else
    setFakeResult(operation,
                  {FileDialogStatus::Cancelled, {}});
    wakeSta();
#endif
    return true;
}

bool FileDialogCoordinatorControl::finishLocked(
    const std::shared_ptr<Operation> &operation,
    FileDialogSelection selection) noexcept
{
    if (operation->state == OperationState::Terminal) {
        return false;
    }
    operation->state = OperationState::Terminal;

    selection.approvedMaximumBytes = operation->maximumBytes;
    enqueueCallback(
        {operation->token, std::move(selection), std::move(operation->completion)});
    return true;
}

void FileDialogCoordinatorControl::finishAndReleaseOperation(
    const std::shared_ptr<Operation> &operation,
    FileDialogSelection selection) noexcept
{
    bool released = false;
    {
        const std::scoped_lock lock(stateMutex_);
        (void)finishLocked(operation, std::move(selection));
        released = releaseOperationReservationLocked(operation);
    }
    notifyOperationQuiesced(released);
}

void FileDialogCoordinatorControl::releaseOperationReservation(
    const std::shared_ptr<Operation> &operation) noexcept
{
    bool released = false;
    {
        const std::scoped_lock lock(stateMutex_);
        released = releaseOperationReservationLocked(operation);
    }
    notifyOperationQuiesced(released);
}

bool FileDialogCoordinatorControl::releaseOperationReservationLocked(
    const std::shared_ptr<Operation> &operation) noexcept
{
    for (auto iterator = pendingOperations_.begin();
         iterator != pendingOperations_.end();) {
        if (*iterator == operation) {
            iterator = pendingOperations_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (activeOperation_ != operation) {
        return false;
    }
    activeOperation_.reset();
    return true;
}

void FileDialogCoordinatorControl::notifyOperationQuiesced(
    const bool released) noexcept
{
#ifdef Q_BROWSER_BROKER_TESTING
    if (released) {
        const auto hooks = qbrowser_broker_testing::fileDialogTestHooks();
        if (hooks.coordinatorOperationQuiesced) {
            try {
                hooks.coordinatorOperationQuiesced();
            } catch (...) {
            }
        }
    }
#else
    Q_UNUSED(released);
#endif
}

void FileDialogCoordinatorControl::enqueueCallback(CallbackWork work) noexcept
{
    try {
        {
            const std::scoped_lock lock(callbackMutex_);
            callbacks_.push_back(std::move(work));
        }
        callbackCondition_.notify_one();
    } catch (...) {
        std::terminate();
    }
}

void FileDialogCoordinatorControl::callbackLoop() noexcept
{
    for (;;) {
        CallbackWork work = [&] {
            std::unique_lock lock(callbackMutex_);
            callbackCondition_.wait(lock, [this] {
                return callbackStopping_ || !callbacks_.empty();
            });
            if (callbacks_.empty()) {
                return CallbackWork{
                    FileDialogOperationToken(1), {}, {}};
            }
            CallbackWork next = std::move(callbacks_.front());
            callbacks_.pop_front();
            return next;
        }();
        if (!work.completion) {
            const std::scoped_lock lock(callbackMutex_);
            if (callbackStopping_ && callbacks_.empty()) {
                return;
            }
            continue;
        }
        try {
            work.completion(work.token, std::move(work.selection));
        } catch (...) {
        }
    }
}

FileDialogSelection FileDialogCoordinatorControl::materializeSelection(
    FileDialogResult result,
    const qint64 maximumBytes,
    const std::function<void(qint64)> &beforeRead,
    const std::function<void()> &beforeEncode) noexcept
{
    FileDialogSelection selection;
    selection.approvedMaximumBytes = maximumBytes;
    selection.status = result.status;
    if (result.status != FileDialogStatus::Opened) {
        return selection;
    }
    if (result.stream == nullptr || !result.stream->isOpen()
        || !result.stream->isReadable() || result.size < 0
        || !isSafeFileLeafName(result.name)) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }
    if (result.size > maximumBytes) {
        selection.status = FileDialogStatus::TooLarge;
        return selection;
    }
    if (maximumBytes >= std::numeric_limits<qint64>::max()
        || maximumBytes + 1
            > static_cast<qint64>(std::numeric_limits<qsizetype>::max())) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }

    const QByteArray boundIdentity = stableStreamIdentityToken(*result.stream);
    const QByteArray identityBefore = result.identityBeforeRead.isEmpty()
        ? boundIdentity
        : result.identityBeforeRead;
    if (boundIdentity.isEmpty() || identityBefore.isEmpty()
        || identityBefore.size() > maximumIdentityTokenBytes
        || identityBefore != boundIdentity) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }

    const qint64 readCapacity = maximumBytes + 1;
    try {
        if (beforeRead) {
            beforeRead(readCapacity);
        }
    } catch (...) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }
    QByteArray content;
    content.resize(static_cast<qsizetype>(readCapacity));
    qint64 totalBytesRead = 0;
    while (totalBytesRead < readCapacity) {
        const qint64 bytesRead = result.stream->read(
            content.data() + totalBytesRead,
            readCapacity - totalBytesRead);
        if (bytesRead < 0) {
            selection.status = FileDialogStatus::Failed;
            return selection;
        }
        if (bytesRead == 0) {
            break;
        }
        totalBytesRead += bytesRead;
    }
    content.resize(static_cast<qsizetype>(totalBytesRead));
    if (totalBytesRead > maximumBytes) {
        selection.status = FileDialogStatus::TooLarge;
        return selection;
    }
    if (totalBytesRead != result.size) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }

    const QByteArray identityAfter = stableStreamIdentityToken(*result.stream);
    if (identityAfter.isEmpty()
        || identityAfter.size() > maximumIdentityTokenBytes
        || identityAfter != identityBefore
        || (!result.identityAfterRead.isEmpty()
            && result.identityAfterRead != identityAfter)) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }
    result.stream.reset();
    try {
        if (beforeEncode) {
            beforeEncode();
        }
    } catch (...) {
        selection.status = FileDialogStatus::Failed;
        return selection;
    }

    selection.status = FileDialogStatus::Opened;
    selection.name = std::move(result.name);
    selection.declaredSize = totalBytesRead;
    selection.contentBase64 = content.toBase64(QByteArray::Base64Encoding);
    selection.identityBeforeRead = identityBefore;
    selection.identityAfterRead = identityAfter;
    return selection;
}

void FileDialogCoordinatorControl::setFakeResult(
    const std::shared_ptr<Operation> &operation,
    CoordinatorShowResult result) noexcept
{
    {
        const std::scoped_lock lock(operation->fakeMutex);
        if (operation->fakeShowFinished || operation->fakeResult.has_value()) {
            return;
        }
        operation->fakeResult = std::move(result);
    }
    operation->fakeCondition.notify_all();
#ifdef Q_OS_WIN
    (void)postWindowMessage(WM_APP + 0x353U);
#endif
}

#ifdef Q_BROWSER_BROKER_TESTING
CoordinatorShowResult FileDialogCoordinatorControl::runFakeShow(
    const std::shared_ptr<Operation> &operation,
    const qbrowser_broker_testing::FileDialogTestShowHook &show) noexcept
{
    const std::weak_ptr<FileDialogCoordinatorControl> weakControl =
        weak_from_this();
    const std::weak_ptr<Operation> weakOperation = operation;
    try {
        show(operation->maximumBytes,
             [weakControl, weakOperation](
                 qbrowser_broker_testing::FileDialogTestShowResult result) {
                 const auto control = weakControl.lock();
                 const auto pending = weakOperation.lock();
                 if (control != nullptr && pending != nullptr) {
                     control->setFakeResult(
                         pending, {result.status, std::move(result.selectedPath)});
                 }
             });
    } catch (...) {
        setFakeResult(operation, {FileDialogStatus::Failed, {}});
    }

#ifdef Q_OS_WIN
    for (;;) {
        {
            const std::scoped_lock lock(operation->fakeMutex);
            if (operation->fakeResult.has_value()) {
                CoordinatorShowResult result =
                    std::move(*operation->fakeResult);
                operation->fakeResult.reset();
                operation->fakeShowFinished = true;
                return result;
            }
        }
        MSG message{};
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            const std::scoped_lock lock(operation->fakeMutex);
            operation->fakeShowFinished = true;
            return {FileDialogStatus::Failed, {}};
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#else
    std::unique_lock lock(operation->fakeMutex);
    operation->fakeCondition.wait(lock, [&] {
        return operation->fakeResult.has_value();
    });
    operation->fakeShowFinished = true;
    if (!operation->fakeResult.has_value()) {
        return {FileDialogStatus::Cancelled, {}};
    }
    CoordinatorShowResult result = std::move(*operation->fakeResult);
    operation->fakeResult.reset();
    return result;
#endif
}
#endif

FileDialogSelection FileDialogCoordinatorControl::runOperation(
    const std::shared_ptr<Operation> &operation) noexcept
{
    FileDialogResult dialogResult =
        FileDialogResult::error(FileDialogStatus::Failed);
    std::function<void(qint64)> beforeRead;
    std::function<void()> beforeEncode;
    try {
#ifdef Q_BROWSER_BROKER_TESTING
        const auto hooks = qbrowser_broker_testing::fileDialogTestHooks();
        if (hooks.coordinatorDialogCreated) {
            hooks.coordinatorDialogCreated();
        }
        beforeRead = hooks.coordinatorBeforeRead;
        beforeEncode = hooks.coordinatorBeforeEncode;
        if (hooks.coordinatorShow) {
            const CoordinatorShowResult shown = runFakeShow(
                operation, hooks.coordinatorShow);
            dialogResult = shown.status == FileDialogStatus::Opened
                ? openStablePath(shown.selectedPath, operation->maximumBytes)
                : FileDialogResult::error(shown.status);
        } else {
#ifdef Q_OS_WIN
            dialogResult = runNativeDialog(operation);
#endif
        }
#else
#ifdef Q_OS_WIN
        dialogResult = runNativeDialog(operation);
#endif
#endif
    } catch (...) {
        dialogResult = FileDialogResult::error(FileDialogStatus::Failed);
    }

    return materializeSelection(
        std::move(dialogResult), operation->maximumBytes,
        beforeRead, beforeEncode);
}

void FileDialogCoordinatorControl::runNextOperation() noexcept
{
#ifdef Q_OS_WIN
    if (physicalOperation_ != nullptr) {
        return;
    }
#endif
    for (;;) {
        std::shared_ptr<Operation> operation;
        bool alreadyTerminal = false;
        {
            const std::scoped_lock lock(stateMutex_);
            if (pendingOperations_.empty()) {
                break;
            }
            operation = pendingOperations_.front();
            pendingOperations_.pop_front();
            alreadyTerminal = operation->state == OperationState::Terminal;
        }
        if (alreadyTerminal) {
            releaseOperationReservation(operation);
            continue;
        }
#ifdef Q_OS_WIN
        physicalOperation_ = operation;
#endif
        FileDialogSelection selection = runOperation(operation);
#ifdef Q_OS_WIN
        physicalDialog_ = nullptr;
        physicalOperation_.reset();
#endif
        finishAndReleaseOperation(operation, std::move(selection));
        break;
    }

    bool hasPending = false;
    bool stopping = false;
    {
        const std::scoped_lock lock(stateMutex_);
        hasPending = !pendingOperations_.empty();
        stopping = stopping_;
    }
#ifdef Q_OS_WIN
    if (hasPending) {
        (void)postWindowMessage(WM_APP + 0x351U);
    } else if (stopping) {
        PostQuitMessage(0);
    }
#else
    if (hasPending || stopping) {
        wakeSta();
    }
#endif
}

#ifdef Q_OS_WIN
FileDialogResult FileDialogCoordinatorControl::runNativeDialog(
    const std::shared_ptr<Operation> &operation) noexcept
{
    ComPointer<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog,
                                reinterpret_cast<void **>(dialog.put())))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->GetOptions(&options))
        || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM
                                     | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST
                                     | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT
                                     | FOS_NODEREFERENCELINKS))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }

    ComPointer<CoordinatorDialogEventHandler> handler;
    handler.attach(new CoordinatorDialogEventHandler(operation->maximumBytes));
    DWORD cookie = 0;
    if (FAILED(dialog->Advise(handler.get(), &cookie))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    physicalDialog_ = dialog.get();
    const HRESULT shown = dialog->Show(nullptr);
    physicalDialog_ = nullptr;
    const HRESULT unadvised = dialog->Unadvise(cookie);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return FileDialogResult::error(FileDialogStatus::Cancelled);
    }
    if (FAILED(shown) || FAILED(unadvised)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return handler->takeResult();
}

bool FileDialogCoordinatorControl::postWindowMessage(
    const UINT message) noexcept
{
    const HWND window = cancelWindow_.load(std::memory_order_acquire);
    return window != nullptr && PostMessageW(window, message, 0, 0) != FALSE;
}

void FileDialogCoordinatorControl::handleStartMessage() noexcept
{
    runNextOperation();
}

void FileDialogCoordinatorControl::deliverPhysicalCancellation() noexcept
{
    if (physicalOperation_ == nullptr || physicalOperation_->cancelDelivered) {
        return;
    }
    physicalOperation_->cancelDelivered = true;
#ifdef Q_BROWSER_BROKER_TESTING
    const auto hooks = qbrowser_broker_testing::fileDialogTestHooks();
    if (hooks.coordinatorCancel) {
        try {
            hooks.coordinatorCancel();
        } catch (...) {
        }
    }
#endif
    if (physicalDialog_ != nullptr) {
        (void)physicalDialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    } else {
        setFakeResult(physicalOperation_,
                      {FileDialogStatus::Cancelled, {}});
    }
}

void FileDialogCoordinatorControl::handleCancelMessage() noexcept
{
    std::deque<quint64> cancellations;
    {
        const std::scoped_lock lock(cancelMutex_);
        cancellations.swap(pendingCancellations_);
    }
    for (const quint64 cancelled : cancellations) {
        if (physicalOperation_ != nullptr
            && physicalOperation_->token.value_ == cancelled) {
            deliverPhysicalCancellation();
        }
    }
}

void FileDialogCoordinatorControl::handleShutdownMessage() noexcept
{
    if (physicalOperation_ != nullptr) {
        deliverPhysicalCancellation();
        return;
    }
    PostQuitMessage(0);
}

LRESULT CALLBACK FileDialogCoordinatorControl::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM word,
    const LPARAM parameter) noexcept
{
    auto *control = reinterpret_cast<FileDialogCoordinatorControl *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *const creation =
            reinterpret_cast<const CREATESTRUCTW *>(parameter);
        control = static_cast<FileDialogCoordinatorControl *>(
            creation->lpCreateParams);
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(control));
    }
    if (control != nullptr) {
        if (message == WM_APP + 0x351U) {
            control->handleStartMessage();
            return 0;
        }
        if (message == WM_APP + 0x352U) {
            control->handleCancelMessage();
            return 0;
        }
        if (message == WM_APP + 0x354U) {
            control->handleShutdownMessage();
            return 0;
        }
        if (message == WM_APP + 0x353U) {
            return 0;
        }
    }
    return DefWindowProcW(window, message, word, parameter);
}

void FileDialogCoordinatorControl::staLoop() noexcept
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(initialized);
    constexpr wchar_t windowClassName[] =
        L"QBrowser.FileDialogCoordinator.CancelWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &FileDialogCoordinatorControl::windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = windowClassName;
    const ATOM registered = RegisterClassW(&windowClass);
    const bool classAvailable = registered != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    HWND window = nullptr;
    if (classAvailable) {
        window = CreateWindowExW(0,
                                 windowClassName,
                                 L"",
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 HWND_MESSAGE,
                                 nullptr,
                                 windowClass.hInstance,
                                 this);
    }
    cancelWindow_.store(window, std::memory_order_release);
    {
        const std::scoped_lock lock(startMutex_);
        staAvailable_ = comInitialized && window != nullptr;
        staReady_ = true;
    }
    startCondition_.notify_all();

    if (window != nullptr) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DestroyWindow(window);
    }
    cancelWindow_.store(nullptr, std::memory_order_release);
    if (comInitialized) {
        CoUninitialize();
    }
}
#else
void FileDialogCoordinatorControl::wakeSta() noexcept
{
    {
        const std::scoped_lock lock(staMutex_);
        staWake_ = true;
    }
    staCondition_.notify_all();
}

void FileDialogCoordinatorControl::staLoop() noexcept
{
    {
        const std::scoped_lock lock(startMutex_);
        staAvailable_ = true;
        staReady_ = true;
    }
    startCondition_.notify_all();
    for (;;) {
        {
            std::unique_lock lock(staMutex_);
            staCondition_.wait(lock, [this] { return staWake_; });
            staWake_ = false;
        }
        runNextOperation();
        const std::scoped_lock lock(stateMutex_);
        if (stopping_ && pendingOperations_.empty()) {
            return;
        }
    }
}
#endif

void FileDialogCoordinatorControl::beginShutdown() noexcept
{
    std::shared_ptr<Operation> active;
    {
        const std::scoped_lock lock(stateMutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        active = activeOperation_;
    }
    if (active != nullptr) {
        (void)requestCancel(active->token);
    }
#ifdef Q_OS_WIN
    (void)postWindowMessage(WM_APP + 0x354U);
#else
    wakeSta();
#endif
}

void FileDialogCoordinatorControl::shutdown() noexcept
{
    const std::thread::id caller = std::this_thread::get_id();
    if (caller == callbackThreadId_ || caller == staThreadId_) {
        beginShutdown();
        return;
    }

    const std::scoped_lock shutdownLock(shutdownMutex_);
    if (shutdownComplete_) {
        return;
    }
    beginShutdown();
    if (staThread_.joinable()) {
        staThread_.join();
    }
    {
        const std::scoped_lock lock(callbackMutex_);
        callbackStopping_ = true;
    }
    callbackCondition_.notify_all();
    if (callbackThread_.joinable()) {
        callbackThread_.join();
    }
    shutdownComplete_ = true;
}
