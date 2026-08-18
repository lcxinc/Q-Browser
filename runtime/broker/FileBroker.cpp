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
#include <shlguid.h>

#include <limits>
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
    [[nodiscard]] Interface *releaseOwnership()
    {
        Interface *result = value_;
        value_ = nullptr;
        return result;
    }

private:
    Interface *value_ = nullptr;
};

class WindowsComStreamDevice final : public QIODevice
{
public:
    explicit WindowsComStreamDevice(IStream *stream) : stream_(stream)
    {
        QIODevice::open(QIODevice::ReadOnly);
    }
    ~WindowsComStreamDevice() override
    {
        if (stream_ != nullptr) {
            stream_->Release();
        }
    }

protected:
    qint64 readData(char *data, const qint64 maximumSize) override
    {
        if (stream_ == nullptr || data == nullptr || maximumSize < 0) {
            return -1;
        }
        const ULONG request = static_cast<ULONG>(std::min<quint64>(
            static_cast<quint64>(maximumSize),
            static_cast<quint64>(std::numeric_limits<ULONG>::max())));
        ULONG count = 0;
        const HRESULT result = stream_->Read(data, request, &count);
        return (result == S_OK || result == S_FALSE) ? static_cast<qint64>(count) : -1;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    IStream *stream_ = nullptr;
};

struct WindowsFileIdentity final
{
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;

    [[nodiscard]] bool operator==(const WindowsFileIdentity &) const noexcept = default;
};

bool queryPlainFileIdentity(const QString &path, WindowsFileIdentity &identity)
{
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool valid = GetFileType(handle) == FILE_TYPE_DISK
        && GetFileInformationByHandleEx(handle,
                                        FileAttributeTagInfo,
                                        &tag,
                                        static_cast<DWORD>(sizeof(tag)))
            != FALSE
        && (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT
                                  | FILE_ATTRIBUTE_DIRECTORY))
            == 0U
        && GetFileInformationByHandle(handle, &information) != FALSE;
    CloseHandle(handle);
    if (!valid) {
        return false;
    }
    identity = {information.dwVolumeSerialNumber,
                information.nFileIndexHigh,
                information.nFileIndexLow};
    return true;
}

bool hasNoReparseComponents(const QString &path)
{
    QString current = QFileInfo(path).absoluteFilePath();
    for (;;) {
        HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        const bool valid = GetFileInformationByHandleEx(
                               handle,
                               FileAttributeTagInfo,
                               &tag,
                               static_cast<DWORD>(sizeof(tag)))
                != FALSE
            && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
        CloseHandle(handle);
        if (!valid) {
            return false;
        }
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) {
            return true;
        }
        current = parent;
    }
}

FileDialogStatus selectedShellItem(ComPointer<IShellItem> &item)
{
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &hooks = qbrowser_broker_testing::fileDialogTestHooks();
    if (hooks.selectedPath) {
        const QString path = hooks.selectedPath();
        return !path.isEmpty()
                && SUCCEEDED(SHCreateItemFromParsingName(
                    reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                    nullptr,
                    IID_IShellItem,
                    reinterpret_cast<void **>(item.put())))
            ? FileDialogStatus::Opened
            : FileDialogStatus::Failed;
    }
#endif
    ComPointer<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog,
                                reinterpret_cast<void **>(dialog.put())))) {
        return FileDialogStatus::Failed;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->GetOptions(&options))
        || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST
                                     | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                                     | FOS_DONTADDTORECENT | FOS_NODEREFERENCELINKS))) {
        return FileDialogStatus::Failed;
    }
    const HRESULT shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return FileDialogStatus::Cancelled;
    }
    return SUCCEEDED(shown) && SUCCEEDED(dialog->GetResult(item.put()))
        ? FileDialogStatus::Opened
        : FileDialogStatus::Failed;
}
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
    ComPointer<IShellItem> item;
    const FileDialogStatus selected = selectedShellItem(item);
    if (selected != FileDialogStatus::Opened) {
        return FileDialogResult::error(selected);
    }
    SFGAOF attributes = 0;
    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_STREAM | SFGAO_LINK
                                       | SFGAO_FOLDER,
                                   &attributes))
        || (attributes & SFGAO_FILESYSTEM) == 0U
        || (attributes & SFGAO_STREAM) == 0U
        || (attributes & (SFGAO_LINK | SFGAO_FOLDER)) != 0U) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    ComPointer<IStream> stream;
    if (FAILED(item->BindToHandler(nullptr,
                                   BHID_Stream,
                                   IID_IStream,
                                   reinterpret_cast<void **>(stream.put())))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    PWSTR selectedPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath))
        || selectedPath == nullptr) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    const QString path = QString::fromWCharArray(selectedPath);
    CoTaskMemFree(selectedPath);
    WindowsFileIdentity before;
    if (!queryPlainFileIdentity(path, before)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &hooks = qbrowser_broker_testing::fileDialogTestHooks();
    if (hooks.afterStreamBound) {
        hooks.afterStreamBound(path);
    }
#endif
    WindowsFileIdentity after;
    if (!queryPlainFileIdentity(path, after) || !(after == before)
        || !hasNoReparseComponents(path)) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    STATSTG statistics{};
    if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME))) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    if (statistics.cbSize.HighPart != 0U) {
        return FileDialogResult::error(FileDialogStatus::TooLarge);
    }
    const qint64 size = static_cast<qint64>(statistics.cbSize.LowPart);
    if (size > maximumBytes) {
        return FileDialogResult::error(FileDialogStatus::TooLarge);
    }
    auto device = std::make_unique<WindowsComStreamDevice>(stream.releaseOwnership());
    return FileDialogResult::opened(QFileInfo(path).fileName(), size, std::move(device));
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
