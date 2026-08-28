#include "FileBroker.h"
#include "FileDialogTestHooks.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shobjidl.h>

#include <atomic>
#include <fcntl.h>
#include <io.h>
#include <vector>
#elif defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace {
constexpr qsizetype maximumIdentityTokenBytes = 256;

BrokerResult fileFailure(const QString &code, const QString &message)
{
    return BrokerResult::failure(code, message);
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
    ~ComPointer()
    {
        if (value_ != nullptr) {
            value_->Release();
        }
    }
    ComPointer(const ComPointer &) = delete;
    ComPointer &operator=(const ComPointer &) = delete;
    ComPointer() = default;

    [[nodiscard]] Interface *get() const { return value_; }
    [[nodiscard]] Interface **put() { return &value_; }
    [[nodiscard]] Interface *operator->() const { return value_; }
    void attach(Interface *value)
    {
        value_ = value;
    }

private:
    Interface *value_ = nullptr;
};

class UniqueHandle final
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
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
    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool isValid() const { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release()
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

    [[nodiscard]] bool operator==(const WindowsFileIdentity &) const noexcept = default;
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
                                        static_cast<DWORD>(sizeof(identityInformation)))
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

class FileDialogEventHandler final : public IFileDialogEvents
{
public:
    explicit FileDialogEventHandler(const qint64 maximumBytes)
        : maximumBytes_(maximumBytes)
    {
    }

    HRESULT capture(IShellItem *item)
    {
        status_ = FileDialogStatus::Failed;
        if (item == nullptr) {
            return E_INVALIDARG;
        }
        SFGAOF attributes = 0;
        if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_LINK | SFGAO_FOLDER,
                                       &attributes))
            || (attributes & SFGAO_FILESYSTEM) == 0U
            || (attributes & (SFGAO_LINK | SFGAO_FOLDER)) != 0U) {
            return E_ACCESSDENIED;
        }
        PWSTR selectedPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath))
            || selectedPath == nullptr) {
            return E_FAIL;
        }
        const QString path = QString::fromWCharArray(selectedPath);
        CoTaskMemFree(selectedPath);
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
            return E_ACCESSDENIED;
        }
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::fileDialogTestHooks();
        if (hooks.afterNativeHandleOpened) {
            hooks.afterNativeHandleOpened(path);
        }
#endif
        if (!validateBoundPath(path, file.get(), identity)) {
            return E_ACCESSDENIED;
        }
        if (size > maximumBytes_) {
            status_ = FileDialogStatus::TooLarge;
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        path_ = path;
        size_ = size;
        identity_ = identity;
        file_ = std::move(file);
        status_ = FileDialogStatus::Opened;
        return S_OK;
    }

    HRESULT captureForFileOk(IShellItem *item)
    {
        return capture(item) == S_OK ? S_OK : S_FALSE;
    }

    [[nodiscard]] FileDialogResult takeResult()
    {
        if (status_ != FileDialogStatus::Opened || !file_.isValid()) {
            return FileDialogResult::error(status_);
        }
        const HANDLE nativeHandle = file_.release();
        const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(nativeHandle),
                                               _O_RDONLY | _O_BINARY);
        if (descriptor < 0) {
            CloseHandle(nativeHandle);
            return FileDialogResult::error(FileDialogStatus::Failed);
        }
        auto file = std::make_unique<QFile>();
        if (!file->open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            _close(descriptor);
            return FileDialogResult::error(FileDialogStatus::Failed);
        }
        return FileDialogResult::opened(QFileInfo(path_).fileName(),
                                        size_,
                                        std::move(file),
                                        identityToken(identity_));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) override
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
            return S_FALSE;
        }
        return captureForFileOk(item.get());
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *,
                                               IShellItem *,
                                               FDE_SHAREVIOLATION_RESPONSE *response) override
    {
        if (response != nullptr) {
            *response = FDESVR_DEFAULT;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *,
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
    QString path_;
    qint64 size_ = 0;
    WindowsFileIdentity identity_;
    UniqueHandle file_;
};
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
}

PreparedFileRequest::PreparedFileRequest(QString operation,
                                         QJsonObject payload,
                                         QString appIdentity,
                                         QString requestId,
                                         const qint64 maximumBytes)
    : operation_(std::move(operation)),
      payload_(std::move(payload)),
      appIdentity_(std::move(appIdentity)),
      requestId_(std::move(requestId)),
      maximumBytes_(maximumBytes)
{
}

const QString &PreparedFileRequest::operation() const noexcept
{
    return operation_;
}

const QJsonObject &PreparedFileRequest::payload() const noexcept
{
    return payload_;
}

const QString &PreparedFileRequest::appIdentity() const noexcept
{
    return appIdentity_;
}

const QString &PreparedFileRequest::requestId() const noexcept
{
    return requestId_;
}

qint64 PreparedFileRequest::maximumBytes() const noexcept
{
    return maximumBytes_;
}

FileDialogResult FileDialogResult::opened(QString name,
                                          const qint64 size,
                                          std::unique_ptr<QIODevice> stream,
                                          QByteArray identityBeforeRead,
                                          QByteArray identityAfterRead)
{
    return {FileDialogStatus::Opened,
            std::move(name),
            size,
            std::move(stream),
            std::move(identityBeforeRead),
            std::move(identityAfterRead)};
}

FileDialogResult FileDialogResult::error(const FileDialogStatus status)
{
    return {status, {}, 0, {}, {}, {}};
}

FileDialogResult QtFileDialogBackend::openFile(const qint64 maximumBytes)
{
#ifdef Q_OS_WIN
    if (maximumBytes < 0) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &hooks = qbrowser_broker_testing::fileDialogTestHooks();
    if (hooks.selectedPath) {
        ComPointer<IShellItem> item;
        const QString path = hooks.selectedPath();
        if (path.isEmpty()
            || FAILED(SHCreateItemFromParsingName(
                reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                nullptr,
                IID_IShellItem,
                reinterpret_cast<void **>(item.put())))) {
            return FileDialogResult::error(FileDialogStatus::Failed);
        }
        ComPointer<FileDialogEventHandler> handler;
        handler.attach(new FileDialogEventHandler(maximumBytes));
        const HRESULT accepted = handler->captureForFileOk(item.get());
        if (accepted == S_FALSE && hooks.cancelAfterRejectedSelection) {
            return FileDialogResult::error(FileDialogStatus::Cancelled);
        }
        return handler->takeResult();
    }
#endif
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
        || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST
                                     | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                                     | FOS_DONTADDTORECENT | FOS_NODEREFERENCELINKS))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    ComPointer<FileDialogEventHandler> handler;
    handler.attach(new FileDialogEventHandler(maximumBytes));
    DWORD cookie = 0;
    if (FAILED(dialog->Advise(handler.get(), &cookie))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const HRESULT shown = dialog->Show(nullptr);
    const HRESULT unadvised = dialog->Unadvise(cookie);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return FileDialogResult::error(FileDialogStatus::Cancelled);
    }
    if (FAILED(shown) || FAILED(unadvised)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return handler->takeResult();
#else
    const QString path = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Open file"));
    if (path.isEmpty()) {
        return FileDialogResult::error(FileDialogStatus::Cancelled);
    }
    const QFileInfo information(path);
    if (information.isSymLink() || !information.isFile()) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    if (information.size() > maximumBytes) {
        return FileDialogResult::error(FileDialogStatus::TooLarge);
    }
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const QByteArray identity = stableStreamIdentityToken(*file);
    if (identity.isEmpty()) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return FileDialogResult::opened(information.fileName(),
                                    information.size(),
                                    std::move(file),
                                    identity);
#endif
}

FileBroker::FileBroker(EffectiveFilePolicy policy, FileDialogBackend &backend)
    : policy_(policy), backend_(backend)
{
}

PreparedFileRequestResult FileBroker::prepareFileRequest(
    const QString &operation,
    const QJsonObject &payload,
    const HostRequestContext &context) const
{
    PreparedFileRequestResult result;
    if (!CapabilityBroker::validRequestContext(context)) {
        result.rejection = fileFailure(
            QStringLiteral("capability.denied"),
            QStringLiteral("Capability is not permitted."));
        return result;
    }
    if (operation != QStringLiteral("open")) {
        result.rejection = fileFailure(
            QStringLiteral("file.invalid_request"),
            QStringLiteral("File request is invalid."));
        return result;
    }
    if (!policy_.open || policy_.maximumBytes <= 0) {
        result.rejection = fileFailure(
            QStringLiteral("capability.denied"),
            QStringLiteral("Capability is not permitted."));
        return result;
    }
    if (!CapabilityBroker::requestFitsIpc(
            context.requestId, QStringLiteral("file"), operation, payload)) {
        result.rejection = fileFailure(
            QStringLiteral("capability.payload_too_large"),
            QStringLiteral("Capability request is too large."));
        return result;
    }
    if (!payload.isEmpty()) {
        result.rejection = fileFailure(
            QStringLiteral("file.invalid_request"),
            QStringLiteral("File request is invalid."));
        return result;
    }

    PreparedFileRequest prepared(
        operation,
        payload,
        context.appIdentity,
        context.requestId,
        std::min(policy_.maximumBytes, maximumIpcBinaryResultBytes()));
    result.request = std::move(prepared);
    return result;
}

BrokerResult FileBroker::completeFileRequest(
    const PreparedFileRequest &request,
    const FileDialogSelection &selection) const
{
    const auto finalize = [&request](BrokerResult result) {
        return CapabilityBroker::boundResponseToIpc(
            request.requestId_, std::move(result));
    };
    const HostRequestContext context{request.appIdentity_, request.requestId_};
    if (!CapabilityBroker::validRequestContext(context)
        || request.operation_ != QStringLiteral("open")
        || !request.payload_.isEmpty()
        || request.maximumBytes_ <= 0
        || request.maximumBytes_ > maximumIpcBinaryResultBytes()
        || !CapabilityBroker::requestFitsIpc(
            request.requestId_, QStringLiteral("file"), request.operation_,
            request.payload_)) {
        return finalize(fileFailure(
            QStringLiteral("capability.denied"),
            QStringLiteral("Capability is not permitted.")));
    }
    if (selection.approvedMaximumBytes != request.maximumBytes_) {
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }

    switch (selection.status) {
    case FileDialogStatus::Cancelled:
        return finalize(fileFailure(
            QStringLiteral("file.cancelled"),
            QStringLiteral("No file was selected.")));
    case FileDialogStatus::TooLarge:
        return finalize(fileFailure(
            QStringLiteral("file.too_large"),
            QStringLiteral("Selected file is too large.")));
    case FileDialogStatus::Busy:
        return finalize(fileFailure(
            QStringLiteral("file.busy"),
            QStringLiteral("File dialog is busy.")));
    case FileDialogStatus::Failed:
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    case FileDialogStatus::Opened:
        break;
    default:
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }

    if (selection.declaredSize < 0) {
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }
    if (selection.declaredSize > request.maximumBytes_) {
        return finalize(fileFailure(
            QStringLiteral("file.too_large"),
            QStringLiteral("Selected file is too large.")));
    }
    if (!isSafeFileLeafName(selection.name)
        || selection.identityBeforeRead.isEmpty()
        || selection.identityAfterRead.isEmpty()
        || selection.identityBeforeRead.size() > maximumIdentityTokenBytes
        || selection.identityAfterRead.size() > maximumIdentityTokenBytes
        || selection.identityBeforeRead != selection.identityAfterRead) {
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }

    const qint64 maximumBase64Bytes =
        ((request.maximumBytes_ + 2) / 3) * 4;
    if (selection.contentBase64.size() > maximumBase64Bytes) {
        return finalize(fileFailure(
            QStringLiteral("file.too_large"),
            QStringLiteral("Selected file is too large.")));
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        selection.contentBase64, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded
        || decoded.decoded.toBase64(QByteArray::Base64Encoding)
            != selection.contentBase64) {
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }
    if (decoded.decoded.size() > request.maximumBytes_) {
        return finalize(fileFailure(
            QStringLiteral("file.too_large"),
            QStringLiteral("Selected file is too large.")));
    }
    if (decoded.decoded.size() != selection.declaredSize) {
        return finalize(fileFailure(
            QStringLiteral("file.failed"),
            QStringLiteral("Selected file is unavailable.")));
    }

    return finalize(BrokerResult::success(
        QJsonObject{{QStringLiteral("name"), selection.name},
                    {QStringLiteral("size"), selection.declaredSize},
                    {QStringLiteral("contentBase64"),
                     QString::fromLatin1(selection.contentBase64)}}));
}

BrokerResult FileBroker::invoke(const QString &operation,
                                const QJsonObject &payload,
                                const HostRequestContext &context)
{
    PreparedFileRequestResult preparation = prepareFileRequest(
        operation, payload, context);
    if (!preparation.request.has_value()) {
        return preparation.rejection;
    }
    const PreparedFileRequest &prepared = *preparation.request;
    FileDialogSelection completed;
    completed.approvedMaximumBytes = prepared.maximumBytes();
    if (!dialogMutex_.tryLock()) {
        completed.status = FileDialogStatus::Busy;
        return completeFileRequest(prepared, completed);
    }
    const auto unlock = qScopeGuard([this] { dialogMutex_.unlock(); });
    FileDialogResult selection = backend_.openFile(prepared.maximumBytes());
    completed.status = selection.status;
    if (selection.status != FileDialogStatus::Opened) {
        return completeFileRequest(prepared, completed);
    }
    if (selection.stream == nullptr || !selection.stream->isOpen()
        || !selection.stream->isReadable()) {
        completed.status = FileDialogStatus::Failed;
        return completeFileRequest(prepared, completed);
    }
    if (selection.size > prepared.maximumBytes()) {
        completed.status = FileDialogStatus::TooLarge;
        return completeFileRequest(prepared, completed);
    }

    const QByteArray identityBeforeRead =
        stableStreamIdentityToken(*selection.stream);
    completed.name = std::move(selection.name);
    completed.declaredSize = selection.size;
    completed.identityBeforeRead = selection.identityBeforeRead.isEmpty()
        ? identityBeforeRead
        : selection.identityBeforeRead;
    const qint64 readCapacity = prepared.maximumBytes() + 1;
    QByteArray content;
    content.resize(static_cast<qsizetype>(readCapacity));
    qint64 totalBytesRead = 0;
    while (totalBytesRead < readCapacity) {
        const qint64 bytesRead = selection.stream->read(
            content.data() + totalBytesRead,
            readCapacity - totalBytesRead);
        if (bytesRead < 0) {
            completed.status = FileDialogStatus::Failed;
            return completeFileRequest(prepared, completed);
        }
        if (bytesRead == 0) {
            break;
        }
        totalBytesRead += bytesRead;
    }
    content.resize(static_cast<qsizetype>(totalBytesRead));
    completed.contentBase64 = content.toBase64(QByteArray::Base64Encoding);
    const QByteArray identityAfterRead =
        stableStreamIdentityToken(*selection.stream);
    completed.identityAfterRead = identityAfterRead.isEmpty()
        ? selection.identityAfterRead
        : identityAfterRead;
    return completeFileRequest(prepared, completed);
}
