#include "WindowsStableIo.h"
#include "ArchiveTestHooks.h"

#ifdef Q_OS_WIN

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace qbrowser_archive_detail
{
namespace
{
QString absolutePath(const QString &path)
{
    return QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

QString pathKey(const QString &path)
{
    return absolutePath(path).toCaseFolded();
}

bool queryHandle(
    HANDLE handle,
    bool requireDirectory,
    WindowsFileIdentity &identity,
    QString &finalPath)
{
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &tagInfo,
            static_cast<DWORD>(sizeof(tagInfo)))
            == FALSE
        || (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || ((tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
            != requireDirectory) {
        return false;
    }
    if (!requireDirectory && GetFileType(handle) != FILE_TYPE_DISK) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle, &information) == FALSE) {
        return false;
    }
    identity = {information.dwVolumeSerialNumber,
                information.nFileIndexHigh,
                information.nFileIndexLow};

    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U
        || required > static_cast<DWORD>(std::numeric_limits<int>::max())) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1U);
    const DWORD length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0U || length >= buffer.size()) {
        return false;
    }
    finalPath = QString::fromWCharArray(
        buffer.data(), static_cast<qsizetype>(length));
    return true;
}

bool finalPathIsWithin(const QString &root, const QString &candidate)
{
    const QString foldedRoot = QDir::toNativeSeparators(root).toCaseFolded();
    const QString foldedCandidate = QDir::toNativeSeparators(candidate).toCaseFolded();
    return foldedCandidate == foldedRoot
        || foldedCandidate.startsWith(foldedRoot + QLatin1Char('\\'));
}

UniqueWindowsHandle openDirectory(const QString &path, DWORD desiredAccess)
{
    return UniqueWindowsHandle(CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
}

void deleteOnClose(HANDLE handle) noexcept
{
    FILE_DISPOSITION_INFO disposition{TRUE};
    (void)SetFileInformationByHandle(
        handle,
        FileDispositionInfo,
        &disposition,
        static_cast<DWORD>(sizeof(disposition)));
}
}

UniqueWindowsHandle::UniqueWindowsHandle(HANDLE handle) noexcept
    : m_handle(handle)
{
}

UniqueWindowsHandle::~UniqueWindowsHandle()
{
    reset();
}

UniqueWindowsHandle::UniqueWindowsHandle(UniqueWindowsHandle &&other) noexcept
    : m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE))
{
}

UniqueWindowsHandle &UniqueWindowsHandle::operator=(
    UniqueWindowsHandle &&other) noexcept
{
    if (this != &other) {
        reset(std::exchange(other.m_handle, INVALID_HANDLE_VALUE));
    }
    return *this;
}

HANDLE UniqueWindowsHandle::get() const noexcept
{
    return m_handle;
}

bool UniqueWindowsHandle::isValid() const noexcept
{
    return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
}

void UniqueWindowsHandle::reset(HANDLE handle) noexcept
{
    if (isValid()) {
        (void)CloseHandle(m_handle);
    }
    m_handle = handle;
}

bool WindowsStableDirectoryTree::openRoot(const QString &rootPath)
{
    m_directories.clear();
    m_rootPath = absolutePath(rootPath);
    m_rootKey = pathKey(m_rootPath);

    std::vector<QString> ancestors;
    QString current = m_rootPath;
    while (!current.isEmpty()) {
        ancestors.push_back(current);
        const QString parent = absolutePath(QFileInfo(current).dir().absolutePath());
        if (pathKey(parent) == pathKey(current)) {
            break;
        }
        current = parent;
    }
    std::reverse(ancestors.begin(), ancestors.end());
    for (const QString &ancestor : ancestors) {
        if (!addDirectory(ancestor, false)) {
            m_directories.clear();
            return false;
        }
    }
    const DirectoryRecord &root = m_directories.back();
    m_rootFinalPath = root.finalPath;
    m_rootVolumeSerial = root.identity.volumeSerial;
    return true;
}

bool WindowsStableDirectoryTree::addExistingDirectory(const QString &path)
{
    if (!pathIsWithinRoot(path)) {
        return false;
    }
    if (contains(path)) {
        return true;
    }

    std::vector<QString> missing;
    QString current = absolutePath(path);
    while (!contains(current)) {
        if (!pathIsWithinRoot(current)) {
            return false;
        }
        missing.push_back(current);
        const QString parent = absolutePath(QFileInfo(current).dir().absolutePath());
        if (pathKey(parent) == pathKey(current)) {
            return false;
        }
        current = parent;
    }
    std::reverse(missing.begin(), missing.end());
    for (const QString &directory : missing) {
        if (!addDirectory(directory, false)) {
            return false;
        }
    }
    return true;
}

bool WindowsStableDirectoryTree::createAndHoldDirectory(const QString &path)
{
    if (!pathIsWithinRoot(path) || contains(path)) {
        return contains(path);
    }
    const QString parent = QFileInfo(absolutePath(path)).dir().absolutePath();
    if (!contains(parent)
        || CreateDirectoryW(
               reinterpret_cast<LPCWSTR>(absolutePath(path).utf16()), nullptr)
            == FALSE) {
        return false;
    }
    if (!addDirectory(path, true)) {
        return false;
    }
    return true;
}

bool WindowsStableDirectoryTree::contains(const QString &path) const
{
    const QString key = pathKey(path);
    return std::any_of(
        m_directories.cbegin(),
        m_directories.cend(),
        [&key](const DirectoryRecord &record) {
            return record.key == key && record.handle.isValid();
        });
}

bool WindowsStableDirectoryTree::isStable() const
{
    for (const DirectoryRecord &record : m_directories) {
        if (!record.handle.isValid()) {
            continue;
        }
        WindowsFileIdentity identity;
        QString finalPath;
        if (!queryHandle(record.handle.get(), true, identity, finalPath)
            || !(identity == record.identity)
            || finalPath.toCaseFolded() != record.finalPath.toCaseFolded()) {
            return false;
        }
    }
    return true;
}

void WindowsStableDirectoryTree::cleanupCreatedDirectories() noexcept
{
    if (!isStable()) {
        return;
    }
    for (auto iterator = m_directories.rbegin();
         iterator != m_directories.rend();
         ++iterator) {
        if (!iterator->created || !iterator->handle.isValid()) {
            continue;
        }
#ifdef Q_BROWSER_ARCHIVE_TESTING
        if (qbrowser_archive_testing::archiveTestHooks()
                .beforeOwnedDirectoryDelete) {
            qbrowser_archive_testing::archiveTestHooks()
                .beforeOwnedDirectoryDelete(iterator->path);
        }
#endif
        deleteOnClose(iterator->handle.get());
        iterator->handle.reset();
    }
}

const QString &WindowsStableDirectoryTree::rootFinalPath() const noexcept
{
    return m_rootFinalPath;
}

DWORD WindowsStableDirectoryTree::rootVolumeSerial() const noexcept
{
    return m_rootVolumeSerial;
}

bool WindowsStableDirectoryTree::addDirectory(const QString &path, bool created)
{
    const QString normalized = absolutePath(path);
    const QString key = pathKey(normalized);
    const bool lockRename = key == m_rootKey
        || key.startsWith(m_rootKey + QLatin1Char('\\'));
    const DWORD desiredAccess = FILE_READ_ATTRIBUTES
        | ((created || lockRename) ? DELETE : 0U);
    UniqueWindowsHandle handle = openDirectory(normalized, desiredAccess);
    WindowsFileIdentity identity;
    QString finalPath;
    if (!handle.isValid()
        || !queryHandle(handle.get(), true, identity, finalPath)) {
        return false;
    }
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened) {
        qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened(
            normalized, static_cast<quint32>(desiredAccess), created);
    }
#endif
    if (!m_rootFinalPath.isEmpty()
        && (!finalPathIsWithin(m_rootFinalPath, finalPath)
            || identity.volumeSerial != m_rootVolumeSerial)) {
        return false;
    }
    m_directories.push_back({normalized,
                             key,
                             finalPath,
                             identity,
                             std::move(handle),
                             created});
    return true;
}

bool WindowsStableDirectoryTree::pathIsWithinRoot(const QString &path) const
{
    const QString key = pathKey(path);
    return key == m_rootKey || key.startsWith(m_rootKey + QLatin1Char('\\'));
}

bool WindowsStableFile::openSource(
    const QString &path,
    const WindowsStableDirectoryTree &tree)
{
    return openAndVerify(
        path, GENERIC_READ | FILE_READ_ATTRIBUTES, tree);
}

bool WindowsStableFile::createOwnedOutput(
    const QString &path,
    const WindowsStableDirectoryTree &tree)
{
    constexpr DWORD desiredAccess =
        GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES;
    m_handle.reset(CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    QString finalPath;
    WindowsFileIdentity identity;
    if (!m_handle.isValid()
        || !queryHandle(m_handle.get(), false, identity, finalPath)
        || identity.volumeSerial != tree.rootVolumeSerial()
        || !finalPathIsWithin(tree.rootFinalPath(), finalPath)) {
        if (m_handle.isValid()) {
            deleteOnClose(m_handle.get());
        }
        m_handle.reset();
        return false;
    }
    m_identity = identity;
    m_path = absolutePath(path);
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened) {
        qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened(
            m_path, static_cast<quint32>(desiredAccess), true);
    }
#endif
    return true;
}

bool WindowsStableFile::writeAll(const char *bytes, size_t size)
{
    if (!m_handle.isValid() || (size > 0U && bytes == nullptr)) {
        return false;
    }
    constexpr DWORD maximumRequest = 64U * 1024U;
    size_t offset = 0;
    while (offset < size) {
        DWORD request = static_cast<DWORD>(std::min<size_t>(
            size - offset, static_cast<size_t>(maximumRequest)));
#ifdef Q_BROWSER_ARCHIVE_TESTING
        if (qbrowser_archive_testing::archiveTestHooks()
                .limitWindowsWriteRequest) {
            const quint32 limited = qbrowser_archive_testing::archiveTestHooks()
                .limitWindowsWriteRequest(static_cast<quint32>(request));
            if (limited == 0U || limited > request) {
                return false;
            }
            request = static_cast<DWORD>(limited);
        }
#endif
        DWORD written = 0;
        if (WriteFile(
                m_handle.get(), bytes + offset, request, &written, nullptr)
                == FALSE
            || written == 0U || written > request) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool WindowsStableFile::flush()
{
    if (!m_handle.isValid()) {
        return false;
    }
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().allowWindowsFlush
        && !qbrowser_archive_testing::archiveTestHooks().allowWindowsFlush()) {
        return false;
    }
#endif
    return FlushFileBuffers(m_handle.get()) != FALSE;
}

bool WindowsStableFile::readExact(
    quint64 expected,
    quint64 maximum,
    QByteArray &bytes)
{
    if (!m_handle.isValid() || expected > maximum
        || expected >= static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return false;
    }
    LARGE_INTEGER start{};
    if (SetFilePointerEx(m_handle.get(), start, nullptr, FILE_BEGIN) == FALSE) {
        return false;
    }

    constexpr DWORD chunkSize = 64U * 1024U;
    char buffer[chunkSize];
    const quint64 sentinel = expected + 1U;
    while (static_cast<quint64>(bytes.size()) < sentinel) {
        const DWORD request = static_cast<DWORD>(std::min<quint64>(
            sentinel - static_cast<quint64>(bytes.size()), chunkSize));
        DWORD count = 0;
        if (ReadFile(m_handle.get(), buffer, request, &count, nullptr) == FALSE) {
            bytes.clear();
            return false;
        }
        if (count == 0U) {
            return static_cast<quint64>(bytes.size()) == expected;
        }
        bytes.append(buffer, static_cast<qsizetype>(count));
    }
    bytes.clear();
    return false;
}

bool WindowsStableFile::publishNoReplace(
    const QString &destination,
    const WindowsStableDirectoryTree &tree)
{
    if (!m_handle.isValid()
        || !tree.contains(QFileInfo(destination).dir().absolutePath())) {
        return false;
    }
    const QString normalizedDestination = absolutePath(destination);
    const size_t nameBytes = static_cast<size_t>(normalizedDestination.size())
        * sizeof(wchar_t);
    constexpr size_t renameHeaderSize = sizeof(FILE_RENAME_INFO);
    if (nameBytes > static_cast<size_t>(std::numeric_limits<DWORD>::max())
            - renameHeaderSize
        || nameBytes > std::numeric_limits<size_t>::max()
            - renameHeaderSize) {
        return false;
    }
    std::vector<unsigned char> renameBuffer(
        renameHeaderSize + nameBytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(renameBuffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(
        rename->FileName,
        normalizedDestination.utf16(),
        nameBytes);
    if (SetFileInformationByHandle(
            m_handle.get(),
            FileRenameInfo,
            rename,
            static_cast<DWORD>(renameBuffer.size()))
        == FALSE) {
        return false;
    }
    m_path = normalizedDestination;
    return true;
}

bool WindowsStableFile::deleteOwned() noexcept
{
    if (!m_handle.isValid()) {
        return false;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool deleted = SetFileInformationByHandle(
                             m_handle.get(),
                             FileDispositionInfo,
                             &disposition,
                             static_cast<DWORD>(sizeof(disposition)))
        != FALSE;
    m_handle.reset();
    return deleted;
}

bool WindowsStableFile::isOpen() const noexcept
{
    return m_handle.isValid();
}

const QString &WindowsStableFile::path() const noexcept
{
    return m_path;
}

bool WindowsStableFile::openAndVerify(
    const QString &path,
    DWORD access,
    const WindowsStableDirectoryTree &tree)
{
    m_handle.reset(CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    QString finalPath;
    WindowsFileIdentity identity;
    if (!m_handle.isValid()
        || !queryHandle(m_handle.get(), false, identity, finalPath)
        || identity.volumeSerial != tree.rootVolumeSerial()
        || !finalPathIsWithin(tree.rootFinalPath(), finalPath)) {
        m_handle.reset();
        return false;
    }
    m_identity = identity;
    m_path = absolutePath(path);
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened) {
        qbrowser_archive_testing::archiveTestHooks().afterWindowsHandleOpened(
            m_path, static_cast<quint32>(access), false);
    }
#endif
    return true;
}
}

#endif
