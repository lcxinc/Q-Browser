#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN

#include <QByteArray>
#include <QString>

#include <qt_windows.h>

#include <cstddef>
#include <vector>

namespace qbrowser_archive_detail
{
class UniqueWindowsHandle final
{
public:
    UniqueWindowsHandle() = default;
    explicit UniqueWindowsHandle(HANDLE handle) noexcept;
    ~UniqueWindowsHandle();

    UniqueWindowsHandle(const UniqueWindowsHandle &) = delete;
    UniqueWindowsHandle &operator=(const UniqueWindowsHandle &) = delete;
    UniqueWindowsHandle(UniqueWindowsHandle &&other) noexcept;
    UniqueWindowsHandle &operator=(UniqueWindowsHandle &&other) noexcept;

    [[nodiscard]] HANDLE get() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept;

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

struct WindowsFileIdentity final
{
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;

    [[nodiscard]] bool operator==(const WindowsFileIdentity &other) const noexcept = default;
};

class WindowsStableDirectoryTree final
{
public:
    [[nodiscard]] bool openRoot(const QString &rootPath);
    [[nodiscard]] bool addExistingDirectory(const QString &path);
    [[nodiscard]] bool createAndHoldDirectory(const QString &path);
    [[nodiscard]] bool contains(const QString &path) const;
    [[nodiscard]] bool isStable() const;
    void cleanupCreatedDirectories() noexcept;

    [[nodiscard]] const QString &rootFinalPath() const noexcept;
    [[nodiscard]] DWORD rootVolumeSerial() const noexcept;

private:
    struct DirectoryRecord final
    {
        QString path;
        QString key;
        QString finalPath;
        WindowsFileIdentity identity;
        UniqueWindowsHandle handle;
        bool created = false;
    };

    [[nodiscard]] bool addDirectory(const QString &path, bool created);
    [[nodiscard]] bool pathIsWithinRoot(const QString &path) const;

    std::vector<DirectoryRecord> m_directories;
    QString m_rootPath;
    QString m_rootKey;
    QString m_rootFinalPath;
    DWORD m_rootVolumeSerial = 0;
};

class WindowsStableFile final
{
public:
    [[nodiscard]] bool openSource(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool openReadLocked(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool createOwnedOutput(
        const QString &path,
        const WindowsStableDirectoryTree &tree,
        SECURITY_ATTRIBUTES *securityAttributes = nullptr);
    [[nodiscard]] bool writeAll(const char *bytes, size_t size);
    [[nodiscard]] bool flush();
    [[nodiscard]] bool readExact(
        quint64 expected,
        quint64 maximum,
        QByteArray &bytes);
    [[nodiscard]] bool publishNoReplace(
        const QString &destination,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool deleteOwned() noexcept;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const QString &path() const noexcept;

private:
    [[nodiscard]] bool openAndVerify(
        const QString &path,
        DWORD access,
        DWORD shareMode,
        const WindowsStableDirectoryTree &tree);

    UniqueWindowsHandle m_handle;
    WindowsFileIdentity m_identity;
    QString m_path;
};
}

#endif
