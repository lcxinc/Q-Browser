#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN

#include <QByteArray>
#include <QString>

#include <qt_windows.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace qbrowser_archive_detail
{
[[nodiscard]] std::optional<QString> windowsApiPath(const QString &path);

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

enum class WindowsStableFileOpenStatus
{
    Opened,
    Missing,
    Failure,
};

enum class WindowsStableReadStatus
{
    Read,
    TooLarge,
    Failure,
};

class WindowsStableDirectoryTree final
{
public:
    [[nodiscard]] bool openRoot(const QString &rootPath);
    [[nodiscard]] bool openSharedRoot(const QString &rootPath);
    [[nodiscard]] bool openMovableRoot(const QString &rootPath);
    [[nodiscard]] bool addExistingDirectory(const QString &path);
    [[nodiscard]] bool addImmutableDirectory(const QString &path);
    [[nodiscard]] bool createAndHoldDirectory(const QString &path);
    [[nodiscard]] bool adoptCreatedDirectoryRoot(
        WindowsStableDirectoryTree &source,
        const QString &path);
    [[nodiscard]] bool contains(const QString &path) const;
    [[nodiscard]] bool isStable() const;
    [[nodiscard]] bool rootHasRestrictedTrustAcl() const;
    [[nodiscard]] bool isSameRootIdentityAt(const QString &path) const;
    [[nodiscard]] bool publishRootNoReplace(
        const QString &destination,
        const WindowsStableDirectoryTree &destinationTree);
    [[nodiscard]] bool sealMutationsForMove();
    void releaseDescendantsForMove() noexcept;
    [[nodiscard]] bool restoreMutationSeals() noexcept;
    [[nodiscard]] bool verifyMovedTree(const QString &destination) const;
    [[nodiscard]] bool deleteHeldTree() noexcept;
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
        QByteArray originalSecurity;
        bool mutationSealed = false;
        bool created = false;
    };

    [[nodiscard]] bool addDirectory(
        const QString &path,
        bool created,
        bool immutable = false);
    [[nodiscard]] bool openRootImpl(const QString &rootPath,
                                    bool movable,
                                    bool requestRootDeleteAccess);
    [[nodiscard]] bool pathIsWithinRoot(const QString &path) const;

    std::vector<DirectoryRecord> m_directories;
    QString m_rootPath;
    QString m_rootKey;
    QString m_rootFinalPath;
    DWORD m_rootVolumeSerial = 0;
    bool m_movableRoot = false;
    bool m_requestRootDeleteAccess = true;
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
    [[nodiscard]] bool openReadMoveLocked(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool openForDelete(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] WindowsStableFileOpenStatus openReadDeleteLocked(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool createOwnedOutput(
        const QString &path,
        const WindowsStableDirectoryTree &tree,
        SECURITY_ATTRIBUTES *securityAttributes = nullptr);
    [[nodiscard]] bool createRestrictedOutput(
        const QString &path,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool writeAll(const char *bytes, size_t size);
    [[nodiscard]] bool flush();
    [[nodiscard]] bool readExact(
        quint64 expected,
        quint64 maximum,
        QByteArray &bytes);
    [[nodiscard]] bool readBounded(quint64 maximum, QByteArray &bytes);
    [[nodiscard]] WindowsStableReadStatus readBoundedIncludingEmpty(
        quint64 maximum,
        QByteArray &bytes);
    [[nodiscard]] bool hasRestrictedTrustAcl() const;
    [[nodiscard]] bool hasSingleLink() const;
    [[nodiscard]] bool publishNoReplace(
        const QString &destination,
        const WindowsStableDirectoryTree &tree);
    [[nodiscard]] bool publishAtomic(
        const QString &destination,
        const WindowsStableDirectoryTree &tree,
        bool replaceExisting);
    [[nodiscard]] bool deleteOwned() noexcept;
    [[nodiscard]] bool setReadOnly(bool readOnly) noexcept;
    [[nodiscard]] bool sealMutationsForMove();
    void releaseSealedForMove() noexcept;
    [[nodiscard]] bool restoreMutationSeal(const QString &path) noexcept;
    [[nodiscard]] bool isSameIdentityAt(const QString &path) const;
    [[nodiscard]] bool isStableWithin(
        const WindowsStableDirectoryTree &tree) const;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const QString &path() const noexcept;
    [[nodiscard]] WindowsFileIdentity identity() const noexcept;

private:
    [[nodiscard]] bool openAndVerify(
        const QString &path,
        DWORD access,
        DWORD shareMode,
        const WindowsStableDirectoryTree &tree);

    UniqueWindowsHandle m_handle;
    WindowsFileIdentity m_identity;
    QByteArray m_originalSecurity;
    QString m_finalPath;
    QString m_path;
    bool m_mutationSealed = false;
};
}

#endif
