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
#endif

namespace {
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
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;

    [[nodiscard]] bool operator==(const WindowsFileIdentity &) const noexcept = default;
};

bool queryPlainFileHandle(HANDLE handle,
                          WindowsFileIdentity &identity,
                          qint64 &size)
{
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BY_HANDLE_FILE_INFORMATION information{};
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
        && GetFileInformationByHandle(handle, &information) != FALSE
        && GetFileSizeEx(handle, &fileSize) != FALSE
        && fileSize.QuadPart >= 0;
    if (!valid) {
        return false;
    }
    identity = {information.dwVolumeSerialNumber,
                information.nFileIndexHigh,
                information.nFileIndexLow};
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
        return FileDialogResult::opened(QFileInfo(path_).fileName(), size_, std::move(file));
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
    UniqueHandle file_;
};
#endif
}

FileDialogResult FileDialogResult::opened(QString name,
                                          const qint64 size,
                                          std::unique_ptr<QIODevice> stream)
{
    return {FileDialogStatus::Opened, std::move(name), size, std::move(stream)};
}

FileDialogResult FileDialogResult::error(const FileDialogStatus status)
{
    return {status, {}, 0, {}};
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
    return FileDialogResult::opened(information.fileName(), information.size(), std::move(file));
#endif
}

FileBroker::FileBroker(EffectiveFilePolicy policy, FileDialogBackend &backend)
    : policy_(policy), backend_(backend)
{
}

BrokerResult FileBroker::invoke(const QString &operation,
                                const QJsonObject &payload,
                                const HostRequestContext &context)
{
    Q_UNUSED(context)
    if (operation != QStringLiteral("open") || !payload.isEmpty()) {
        return BrokerResult::failure(QStringLiteral("file.invalid_request"),
                                     QStringLiteral("File request is invalid."));
    }
    if (!policy_.open) {
        return BrokerResult::failure(QStringLiteral("capability.denied"),
                                     QStringLiteral("Capability is not permitted."));
    }
    if (!dialogMutex_.tryLock()) {
        return BrokerResult::failure(QStringLiteral("file.failed"),
                                     QStringLiteral("File dialog is unavailable."));
    }
    const auto unlock = qScopeGuard([this] { dialogMutex_.unlock(); });
    const qint64 maximumBytes =
        std::min(policy_.maximumBytes, maximumIpcBinaryResultBytes());
    FileDialogResult selection = backend_.openFile(maximumBytes);
    if (selection.status == FileDialogStatus::Cancelled) {
        return BrokerResult::failure(QStringLiteral("file.cancelled"),
                                     QStringLiteral("No file was selected."));
    }
    if (selection.status == FileDialogStatus::TooLarge) {
        return BrokerResult::failure(QStringLiteral("file.too_large"),
                                     QStringLiteral("Selected file is too large."));
    }
    if (selection.status != FileDialogStatus::Opened || selection.stream == nullptr
        || !selection.stream->isOpen() || !selection.stream->isReadable()
        || selection.size < 0 || selection.size > maximumBytes
        || selection.name.isEmpty() || selection.name.size() > 255) {
        return BrokerResult::failure(QStringLiteral("file.failed"),
                                     QStringLiteral("Selected file is unavailable."));
    }
    for (const QChar character : selection.name) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return BrokerResult::failure(QStringLiteral("file.failed"),
                                         QStringLiteral("Selected file is unavailable."));
        }
    }
    const QByteArray content = selection.stream->read(maximumBytes + 1);
    if (content.size() > maximumBytes || content.size() != selection.size) {
        return BrokerResult::failure(content.size() > maximumBytes
                                         ? QStringLiteral("file.too_large")
                                         : QStringLiteral("file.failed"),
                                     content.size() > maximumBytes
                                         ? QStringLiteral("Selected file is too large.")
                                         : QStringLiteral("Selected file is unavailable."));
    }
    return BrokerResult::success(
        QJsonObject{{QStringLiteral("name"), selection.name},
                    {QStringLiteral("size"), content.size()},
                    {QStringLiteral("contentBase64"),
                     QString::fromLatin1(content.toBase64(QByteArray::Base64Encoding))}});
}
