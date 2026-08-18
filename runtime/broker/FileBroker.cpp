#include "FileBroker.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#endif

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
    const QString path = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Open file"));
    if (path.isEmpty()) {
        return FileDialogResult::error(FileDialogStatus::Cancelled);
    }

#ifdef Q_OS_WIN
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                                GENERIC_READ | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (GetFileType(handle) != FILE_TYPE_DISK
        || GetFileInformationByHandleEx(handle,
                                        FileAttributeTagInfo,
                                        &attributes,
                                        static_cast<DWORD>(sizeof(attributes)))
               == FALSE
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
        || GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0) {
        CloseHandle(handle);
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    if (size.QuadPart > maximumBytes) {
        CloseHandle(handle);
        return FileDialogResult::error(FileDialogStatus::TooLarge);
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                           _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    auto file = std::make_unique<QFile>();
    if (!file->open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        _close(descriptor);
        return FileDialogResult::error(FileDialogStatus::Failed);
    }
    return FileDialogResult::opened(QFileInfo(path).fileName(),
                                    size.QuadPart,
                                    std::move(file));
#else
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
