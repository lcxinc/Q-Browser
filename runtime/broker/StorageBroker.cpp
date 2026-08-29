#include "StorageBroker.h"
#include "StorageTestHooks.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#include <qt_windows.h>
#include <Aclapi.h>
#else
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace qbrowser_storage_detail {

class RootTransactionLock final
{
public:
    std::mutex mutex;
};

#ifdef Q_OS_WIN
class WindowsRootTransactionGuard final
{
public:
    explicit WindowsRootTransactionGuard(const QString &canonicalRoot)
    {
        const QByteArray digest = QCryptographicHash::hash(
            canonicalRoot.toUtf8(), QCryptographicHash::Sha256).toHex();
        const QString name = QStringLiteral("Local\\QBrowser.Storage.Root.")
            + QString::fromLatin1(digest);
        mutex_ = CreateMutexW(
            nullptr, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
        invalid_ = mutex_ == nullptr;
    }

    ~WindowsRootTransactionGuard()
    {
        if (locked_) {
            (void)ReleaseMutex(mutex_);
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }

    WindowsRootTransactionGuard(const WindowsRootTransactionGuard &) = delete;
    WindowsRootTransactionGuard &operator=(
        const WindowsRootTransactionGuard &) = delete;

    [[nodiscard]] bool tryLock(const int timeoutMilliseconds)
    {
        if (invalid_) {
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            const DWORD result = WaitForSingleObject(mutex_, 0);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
                locked_ = true;
                return true;
            }
            if (result != WAIT_TIMEOUT) {
                invalid_ = true;
                return false;
            }
            if (timeoutMilliseconds == 0
                || timer.elapsed() >= timeoutMilliseconds
                || QThread::currentThread()->isInterruptionRequested()) {
                return false;
            }
            QThread::msleep(10);
        }
    }

    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

private:
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
    bool invalid_ = false;
};
#endif

#ifndef Q_OS_WIN
class PosixStableDirectory final
{
public:
    ~PosixStableDirectory()
    {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    PosixStableDirectory(const PosixStableDirectory &) = delete;
    PosixStableDirectory &operator=(const PosixStableDirectory &) = delete;

    PosixStableDirectory() = default;

    [[nodiscard]] bool openRoot(const QString &path)
    {
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const QByteArray encoded = QFile::encodeName(path);
        const int descriptor = ::open(encoded.constData(), flags);
        struct stat status {};
        if (descriptor < 0 || ::fstat(descriptor, &status) != 0
            || !S_ISDIR(status.st_mode)) {
            if (descriptor >= 0) {
                (void)::close(descriptor);
            }
            return false;
        }
        descriptor_ = descriptor;
        device_ = status.st_dev;
        inode_ = status.st_ino;
        return isSameIdentityAt(path);
    }

    [[nodiscard]] bool isSameIdentityAt(const QString &path) const
    {
        if (descriptor_ < 0) {
            return false;
        }
        struct stat held {};
        struct stat current {};
        const QByteArray encoded = QFile::encodeName(path);
        return ::fstat(descriptor_, &held) == 0 && S_ISDIR(held.st_mode)
            && held.st_dev == device_ && held.st_ino == inode_
            && ::lstat(encoded.constData(), &current) == 0
            && S_ISDIR(current.st_mode) && !S_ISLNK(current.st_mode)
            && current.st_dev == device_ && current.st_ino == inode_;
    }

    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

    [[nodiscard]] bool permissions(mode_t &permissions) const
    {
        struct stat status {};
        if (descriptor_ < 0 || ::fstat(descriptor_, &status) != 0
            || !S_ISDIR(status.st_mode)) {
            return false;
        }
        permissions = status.st_mode & static_cast<mode_t>(07777);
        return true;
    }

    [[nodiscard]] bool setPermissions(const mode_t permissions) const
    {
        return descriptor_ >= 0 && ::fchmod(descriptor_, permissions) == 0;
    }

private:
    int descriptor_ = -1;
    dev_t device_{};
    ino_t inode_{};
};

class PosixStableFile final
{
public:
    ~PosixStableFile()
    {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    PosixStableFile(const PosixStableFile &) = delete;
    PosixStableFile &operator=(const PosixStableFile &) = delete;

    PosixStableFile() = default;

    [[nodiscard]] bool openAt(const PosixStableDirectory &root,
                              const QString &name,
                              const qint64 maximumBytes,
                              QByteArray *const content = nullptr)
    {
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const QByteArray encoded = QFile::encodeName(name);
        const int descriptor = ::openat(
            root.descriptor(), encoded.constData(), flags);
        struct stat status {};
        if (descriptor < 0 || ::fstat(descriptor, &status) != 0
            || !S_ISREG(status.st_mode) || status.st_nlink != 1
            || status.st_size < 0
            || status.st_size > maximumBytes) {
            if (descriptor >= 0) {
                (void)::close(descriptor);
            }
            return false;
        }
        descriptor_ = descriptor;
        device_ = status.st_dev;
        inode_ = status.st_ino;
        size_ = static_cast<qint64>(status.st_size);
        return isSameIdentityAt(root, name)
            && readBounded(maximumBytes, content);
    }

    [[nodiscard]] bool isSameIdentityAt(
        const PosixStableDirectory &root, const QString &name) const
    {
        if (descriptor_ < 0 || root.descriptor() < 0) {
            return false;
        }
        struct stat held {};
        struct stat current {};
        const QByteArray encoded = QFile::encodeName(name);
        return ::fstat(descriptor_, &held) == 0 && S_ISREG(held.st_mode)
            && held.st_nlink == 1
            && held.st_dev == device_ && held.st_ino == inode_
            && ::fstatat(root.descriptor(), encoded.constData(), &current,
                         AT_SYMLINK_NOFOLLOW) == 0
            && S_ISREG(current.st_mode)
            && current.st_nlink == 1
            && current.st_dev == device_ && current.st_ino == inode_;
    }

    [[nodiscard]] qint64 size() const noexcept { return size_; }

    [[nodiscard]] bool permissions(mode_t &permissions) const
    {
        struct stat status {};
        if (descriptor_ < 0 || ::fstat(descriptor_, &status) != 0
            || !S_ISREG(status.st_mode)) {
            return false;
        }
        permissions = status.st_mode & static_cast<mode_t>(07777);
        return true;
    }

    [[nodiscard]] bool setPermissions(const mode_t permissions) const
    {
        return descriptor_ >= 0 && ::fchmod(descriptor_, permissions) == 0;
    }

private:
    [[nodiscard]] bool readBounded(
        const qint64 maximumBytes, QByteArray *const content) const
    {
        if (descriptor_ < 0 || ::lseek(descriptor_, 0, SEEK_SET) < 0) {
            return false;
        }
        if (content != nullptr) {
            content->clear();
            content->reserve(static_cast<qsizetype>(size_));
        }
        char buffer[16 * 1024];
        qint64 total = 0;
        for (;;) {
            const ssize_t readBytes = ::read(descriptor_, buffer, sizeof(buffer));
            if (readBytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (readBytes == 0) {
                break;
            }
            if (readBytes > maximumBytes - total) {
                return false;
            }
            if (content != nullptr) {
                content->append(buffer, static_cast<qsizetype>(readBytes));
            }
            total += static_cast<qint64>(readBytes);
        }
        struct stat status {};
        return total == size_ && ::fstat(descriptor_, &status) == 0
            && S_ISREG(status.st_mode) && status.st_nlink == 1
            && status.st_dev == device_
            && status.st_ino == inode_ && status.st_size == total;
    }

    int descriptor_ = -1;
    dev_t device_{};
    ino_t inode_{};
    qint64 size_ = -1;
};

class PosixRootTransactionGuard final
{
public:
    explicit PosixRootTransactionGuard(const PosixStableDirectory &root)
        : descriptor_(root.descriptor())
    {
    }

    ~PosixRootTransactionGuard()
    {
        if (locked_) {
            (void)::flock(descriptor_, LOCK_UN);
        }
    }

    PosixRootTransactionGuard(const PosixRootTransactionGuard &) = delete;
    PosixRootTransactionGuard &operator=(const PosixRootTransactionGuard &) = delete;

    [[nodiscard]] bool tryLock(const int timeoutMilliseconds)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            if (::flock(descriptor_, LOCK_EX | LOCK_NB) == 0) {
                locked_ = true;
                return true;
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                invalid_ = true;
                return false;
            }
            if (timeoutMilliseconds == 0
                || timer.elapsed() >= timeoutMilliseconds
                || QThread::currentThread()->isInterruptionRequested()) {
                return false;
            }
            QThread::msleep(10);
        }
    }

    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

private:
    int descriptor_ = -1;
    bool locked_ = false;
    bool invalid_ = false;
};
#endif

} // namespace qbrowser_storage_detail

namespace {

QString canonicalRootKey(const QFileInfo &rootInfo)
{
    QString key = QDir::cleanPath(
        QDir::fromNativeSeparators(rootInfo.canonicalFilePath()));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

bool callerIsGuiThread()
{
    const QCoreApplication *const application = QCoreApplication::instance();
    return application != nullptr && application->thread() == QThread::currentThread();
}

std::shared_ptr<qbrowser_storage_detail::RootTransactionLock>
transactionLockForRoot(const QString &canonicalRoot)
{
    static std::mutex registryMutex;
    static QHash<
        QString,
        std::weak_ptr<qbrowser_storage_detail::RootTransactionLock>> registry;

    const std::lock_guard guard(registryMutex);
    for (auto iterator = registry.begin(); iterator != registry.end();) {
        if (iterator.value().expired()) {
            iterator = registry.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (const auto existing = registry.value(canonicalRoot).lock()) {
        return existing;
    }
    auto created = std::make_shared<qbrowser_storage_detail::RootTransactionLock>();
    registry.insert(canonicalRoot, created);
    return created;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    if (object.size() != expected.size()) {
        return false;
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!expected.contains(iterator.key())) {
            return false;
        }
    }
    return true;
}

bool validKey(const QJsonValue &value)
{
    if (!value.isString()) {
        return false;
    }
    static const QRegularExpression pattern(QStringLiteral(R"(^[A-Za-z0-9_.-]{1,128}$)"));
    return pattern.match(value.toString()).hasMatch();
}

QString storageObjectName(const QString &identity)
{
    const QByteArray digest = QCryptographicHash::hash(identity.toUtf8(),
                                                       QCryptographicHash::Sha256)
                                  .toHex();
    return QString::fromLatin1(digest) + QStringLiteral(".json");
}

#ifdef Q_OS_WIN
QString storagePath(const QString &rootDirectory, const QString &identity)
{
    return QDir(rootDirectory).filePath(storageObjectName(identity));
}
#endif

constexpr qsizetype maximumExistingStorageObjects = 8192;
constexpr qint64 maximumLockBytes = 64 * 1024;
#ifdef Q_OS_WIN
constexpr int concurrentUpdateLockTimeoutMilliseconds = 10'000;
#endif
constexpr int rootTransactionLockTimeoutMilliseconds = 10'000;

#ifdef Q_OS_WIN
QString rootTransactionLockPath(const QString &canonicalRoot)
{
    const QByteArray digest = QCryptographicHash::hash(
        canonicalRoot.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QDir::tempPath()).filePath(
        QStringLiteral(".qbrowser-storage-root-%1.lock")
            .arg(QString::fromLatin1(digest)));
}
#endif

bool isRootTransactionLockName(const QString &name)
{
    return name == QLatin1String(".qbrowser-storage-root.lock");
}

bool validStorageObjectName(const QString &name, bool &lockFile)
{
    if (isRootTransactionLockName(name)) {
        lockFile = true;
        return true;
    }
    static const QRegularExpression dataPattern(QStringLiteral(R"(^[0-9a-f]{64}\.json$)"));
    static const QRegularExpression lockPattern(
        QStringLiteral(R"(^[0-9a-f]{64}\.json\.lock$)"));
    lockFile = lockPattern.match(name).hasMatch();
    return lockFile || dataPattern.match(name).hasMatch();
}

#ifdef Q_OS_WIN
bool applyHostOnlyAcl(const QString &path)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<BYTE> tokenBytes(required);
    if (required == 0
        || GetTokenInformation(token,
                               TokenUser,
                               tokenBytes.data(),
                               required,
                               &required)
               == FALSE) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);
    auto *user = reinterpret_cast<TOKEN_USER *>(tokenBytes.data());
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemSize = sizeof(systemBuffer);
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemSize) == FALSE) {
        return false;
    }

    EXPLICIT_ACCESSW entries[2]{};
    PSID sids[2] = {user->User.Sid, systemBuffer};
    for (int index = 0; index < 2; ++index) {
        entries[index].grfAccessPermissions = GENERIC_ALL;
        entries[index].grfAccessMode = SET_ACCESS;
        entries[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[index].Trustee.TrusteeType = TRUSTEE_IS_USER;
        entries[index].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sids[index]);
    }
    PACL acl = nullptr;
    if (SetEntriesInAclW(2, entries, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const DWORD result = SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        acl,
        nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

struct DaclSnapshot final
{
    QString path;
    QByteArray acl;
    bool protectedAcl = false;
};

bool freezeDirectoryMembership(const DaclSnapshot &original)
{
    if (original.acl.size() < static_cast<qsizetype>(sizeof(ACL))) {
        return false;
    }
    BYTE worldBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD worldSize = sizeof(worldBuffer);
    if (CreateWellKnownSid(WinWorldSid, nullptr, worldBuffer, &worldSize) == FALSE) {
        return false;
    }

    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY
        | FILE_DELETE_CHILD;
    entry.grfAccessMode = DENY_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(worldBuffer);
    PACL acl = nullptr;
    if (SetEntriesInAclW(
            1,
            &entry,
            reinterpret_cast<PACL>(const_cast<char *>(original.acl.constData())),
            &acl)
        != ERROR_SUCCESS) {
        return false;
    }
    QString native = QDir::toNativeSeparators(QFileInfo(original.path).absoluteFilePath());
    const SECURITY_INFORMATION protection = original.protectedAcl
        ? PROTECTED_DACL_SECURITY_INFORMATION
        : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD result = SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | protection,
        nullptr,
        nullptr,
        acl,
        nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

bool hasHostOnlyAcl(const QString &path)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<BYTE> tokenBytes(required);
    const bool tokenValid = required > 0
        && GetTokenInformation(token,
                               TokenUser,
                               tokenBytes.data(),
                               required,
                               &required)
            != FALSE;
    CloseHandle(token);
    if (!tokenValid) {
        return false;
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(tokenBytes.data());
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemSize = sizeof(systemBuffer);
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemSize) == FALSE) {
        return false;
    }
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION information{};
    bool valid = GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE
        && (control & SE_DACL_PROTECTED) != 0U
        && GetAclInformation(dacl,
                             &information,
                             static_cast<DWORD>(sizeof(information)),
                             AclSizeInformation)
            != FALSE
        && information.AceCount >= 2U
        && information.AceCount <= 4U;
    bool foundUser = false;
    bool foundSystem = false;
    for (DWORD index = 0; valid && index < information.AceCount; ++index) {
        void *ace = nullptr;
        if (GetAce(dacl, index, &ace) == FALSE
            || static_cast<ACE_HEADER *>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            valid = false;
            break;
        }
        const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(ace);
        PSID sid = const_cast<DWORD *>(&allowed->SidStart);
        if (EqualSid(sid, user->User.Sid) != FALSE) {
            foundUser = true;
        } else if (EqualSid(sid, systemBuffer) != FALSE) {
            foundSystem = true;
        } else {
            valid = false;
        }
    }
    LocalFree(descriptor);
    return valid && foundUser && foundSystem;
}

struct ValidatedStorageObject final
{
    QString path;
    std::unique_ptr<qbrowser_archive_detail::WindowsStableFile> file;
};

bool captureDacl(const QString &path, DaclSnapshot &snapshot)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return false;
    }
    ACL_SIZE_INFORMATION information{};
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool valid = GetAclInformation(dacl,
                                         &information,
                                         static_cast<DWORD>(sizeof(information)),
                                         AclSizeInformation)
            != FALSE
        && information.AclBytesInUse >= sizeof(ACL)
        && GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE;
    if (valid) {
        snapshot.path = path;
        snapshot.acl = QByteArray(reinterpret_cast<const char *>(dacl),
                                  static_cast<qsizetype>(information.AclBytesInUse));
        snapshot.protectedAcl = (control & SE_DACL_PROTECTED) != 0U;
    }
    LocalFree(descriptor);
    return valid;
}

bool restoreDacl(const DaclSnapshot &snapshot)
{
    if (snapshot.acl.size() < static_cast<qsizetype>(sizeof(ACL))) {
        return false;
    }
    QString native = QDir::toNativeSeparators(QFileInfo(snapshot.path).absoluteFilePath());
    const SECURITY_INFORMATION protection = snapshot.protectedAcl
        ? PROTECTED_DACL_SECURITY_INFORMATION
        : UNPROTECTED_DACL_SECURITY_INFORMATION;
    return SetNamedSecurityInfoW(
               reinterpret_cast<LPWSTR>(native.data()),
               SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION | protection,
               nullptr,
               nullptr,
               reinterpret_cast<PACL>(const_cast<char *>(snapshot.acl.constData())),
               nullptr)
        == ERROR_SUCCESS;
}

BrokerResult loadValues(const QString &path,
                        const qint64 quotaBytes,
                        QJsonObject &values,
                        const qbrowser_archive_detail::WindowsStableDirectoryTree &stableRoot)
{
    if (!QFileInfo::exists(path)) {
        values = {};
        return BrokerResult::success();
    }
    const qint64 expected = QFileInfo(path).size();
    if (expected < 0 || expected > quotaBytes) {
        return BrokerResult::failure(QStringLiteral("storage.quota"),
                                     QStringLiteral("Storage quota was exceeded."));
    }
    qbrowser_archive_detail::WindowsStableFile file;
    QByteArray bytes;
    if (!file.openReadLocked(path, stableRoot)
        || !file.readExact(static_cast<quint64>(expected),
                           static_cast<quint64>(quotaBytes),
                           bytes)) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return BrokerResult::failure(QStringLiteral("storage.corrupt"),
                                     QStringLiteral("Storage data is invalid."));
    }
    values = document.object();
    return BrokerResult::success();
}

bool validateExistingLayout(
    const QString &rootDirectory,
    const qint64 quotaBytes,
    const qbrowser_archive_detail::WindowsStableDirectoryTree &stableRoot,
    std::vector<ValidatedStorageObject> &objects)
{
    QDirIterator iterator(rootDirectory,
                          QDir::AllEntries | QDir::Hidden | QDir::System
                              | QDir::NoDotAndDotDot,
                          QDirIterator::NoIteratorFlags);
    qsizetype count = 0;
    qint64 aggregateDataBytes = 0;
    qint64 aggregateLockBytes = 0;
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo information = iterator.fileInfo();
        bool lockFile = false;
        if (!information.isFile() || information.isSymLink()
            || !validStorageObjectName(information.fileName(), lockFile)) {
            return false;
        }
        if (++count > maximumExistingStorageObjects) {
            return false;
        }
        const qint64 maximumBytes = lockFile ? maximumLockBytes : quotaBytes;
        const qint64 expected = information.size();
        if (expected < 0 || expected > maximumBytes) {
            return false;
        }
        qint64 &aggregate = lockFile ? aggregateLockBytes : aggregateDataBytes;
        if (expected > maximumBytes - aggregate) {
            return false;
        }
        aggregate += expected;
        auto file = std::make_unique<qbrowser_archive_detail::WindowsStableFile>();
        QByteArray content;
        if (!file->openReadLocked(information.absoluteFilePath(), stableRoot)
            || !file->readExact(static_cast<quint64>(expected),
                                static_cast<quint64>(maximumBytes),
                                content)
            || !file->isSameIdentityAt(information.absoluteFilePath())
            || !stableRoot.isStable()) {
            return false;
        }
        objects.push_back({information.absoluteFilePath(), std::move(file)});
    }
    return stableRoot.isStable();
}

bool frozenMembershipMatches(
    const QString &rootDirectory,
    const std::vector<ValidatedStorageObject> &objects,
    const qbrowser_archive_detail::WindowsStableDirectoryTree &stableRoot)
{
    QHash<QString, const ValidatedStorageObject *> expected;
    expected.reserve(static_cast<qsizetype>(objects.size()));
    for (const ValidatedStorageObject &object : objects) {
        const QString name = QFileInfo(object.path).fileName();
        if (expected.contains(name)) {
            return false;
        }
        expected.insert(name, &object);
    }

    QSet<QString> seen;
    QDirIterator iterator(rootDirectory,
                          QDir::AllEntries | QDir::Hidden | QDir::System
                              | QDir::NoDotAndDotDot,
                          QDirIterator::NoIteratorFlags);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo information = iterator.fileInfo();
        const QString name = information.fileName();
        const auto object = expected.constFind(name);
        bool lockFile = false;
        const bool identityMatches = object != expected.cend()
            && (*object)->file != nullptr
            && (*object)->file->isSameIdentityAt(information.absoluteFilePath());
        if (object == expected.cend() || seen.contains(name) || !information.isFile()
            || information.isSymLink() || !validStorageObjectName(name, lockFile)
            || !identityMatches || !stableRoot.isStable()) {
            return false;
        }
        seen.insert(name);
    }
    return seen.size() == expected.size() && stableRoot.isStable();
}

bool applyAclsTransactionally(
    const std::vector<ValidatedStorageObject> &objects,
    const qbrowser_archive_detail::WindowsStableDirectoryTree &stableRoot,
    DaclSnapshot originalRoot)
{
    std::vector<DaclSnapshot> snapshots;
    snapshots.reserve(objects.size() + 1U);
    for (const ValidatedStorageObject &object : objects) {
        DaclSnapshot snapshot;
        if (!captureDacl(object.path, snapshot)) {
            (void)restoreDacl(originalRoot);
            return false;
        }
        snapshots.push_back(std::move(snapshot));
    }
    snapshots.push_back(std::move(originalRoot));

    std::vector<bool> dirty(snapshots.size(), false);
    dirty.back() = true; // The membership-freeze DACL already changed the root.
    bool complete = true;
    for (qsizetype index = 0; index < static_cast<qsizetype>(snapshots.size()); ++index) {
        if (!stableRoot.isStable()) {
            complete = false;
            break;
        }
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::storageTestHooks();
        if (hooks.allowAclApply && !hooks.allowAclApply(snapshots[index].path, index)) {
            complete = false;
            break;
        }
#endif
        if (!applyHostOnlyAcl(snapshots[index].path)) {
            complete = false;
            break;
        }
        dirty[static_cast<size_t>(index)] = true;
#ifdef Q_BROWSER_BROKER_TESTING
        if (hooks.allowAclPostcheck
            && !hooks.allowAclPostcheck(snapshots[index].path, index)) {
            complete = false;
            break;
        }
#endif
        if (!hasHostOnlyAcl(snapshots[index].path)) {
            complete = false;
            break;
        }
    }
    if (complete && stableRoot.isStable()) {
        return true;
    }
    bool restored = true;
    for (qsizetype index = static_cast<qsizetype>(snapshots.size()); index > 0; --index) {
        const size_t snapshotIndex = static_cast<size_t>(index - 1);
        if (dirty[snapshotIndex]) {
            restored = restoreDacl(snapshots[snapshotIndex]) && restored;
        }
    }
    (void)restored;
    return false;
}
#else
bool containsSymlinkAncestor(const QString &path)
{
    QString current = QFileInfo(path).absoluteFilePath();
    for (;;) {
        const QFileInfo information(current);
        if (information.isSymLink()) {
            return true;
        }
        const QString parent = information.dir().absolutePath();
        if (parent == current) {
            return false;
        }
        current = parent;
    }
}

BrokerResult loadValues(
    const QString &name,
    const qint64 quotaBytes,
    QJsonObject &values,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    const QByteArray encoded = QFile::encodeName(name);
    struct stat status {};
    if (::fstatat(stableRoot.descriptor(), encoded.constData(), &status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            values = {};
            return BrokerResult::success();
        }
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    if (!S_ISREG(status.st_mode)) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    if (status.st_size < 0 || status.st_size > quotaBytes) {
        return BrokerResult::failure(QStringLiteral("storage.quota"),
                                     QStringLiteral("Storage quota was exceeded."));
    }
    qbrowser_storage_detail::PosixStableFile file;
    QByteArray bytes;
    if (!file.openAt(stableRoot, name, quotaBytes, &bytes)) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return BrokerResult::failure(QStringLiteral("storage.corrupt"),
                                     QStringLiteral("Storage data is invalid."));
    }
    values = document.object();
    return BrokerResult::success();
}

bool publishValues(
    const QString &name,
    const QByteArray &bytes,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    const QString temporaryName = QStringLiteral(".%1.%2.tmp")
        .arg(name, QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QByteArray encodedTemporary = QFile::encodeName(temporaryName);
    const QByteArray encodedTarget = QFile::encodeName(name);
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::openat(
        stableRoot.descriptor(), encodedTemporary.constData(), flags,
        static_cast<mode_t>(0600));
    if (descriptor < 0) {
        return false;
    }

    bool published = false;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            descriptor, bytes.constData() + offset,
            static_cast<size_t>(bytes.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            break;
        }
        offset += static_cast<qsizetype>(written);
    }
    struct stat targetStatus {};
    const int targetResult = ::fstatat(
        stableRoot.descriptor(), encodedTarget.constData(), &targetStatus,
        AT_SYMLINK_NOFOLLOW);
    const bool targetIsSafe = targetResult == 0
        ? S_ISREG(targetStatus.st_mode)
        : errno == ENOENT;
    if (offset == bytes.size() && targetIsSafe
        && ::fchmod(descriptor, static_cast<mode_t>(0600)) == 0
        && ::fsync(descriptor) == 0
        && ::renameat(stableRoot.descriptor(), encodedTemporary.constData(),
                      stableRoot.descriptor(), encodedTarget.constData()) == 0) {
        published = true;
    }
    (void)::close(descriptor);
    if (!published) {
        (void)::unlinkat(
            stableRoot.descriptor(), encodedTemporary.constData(), 0);
    }
    return published;
}

struct ValidatedPosixStorageObject final
{
    QString name;
    QString path;
    std::unique_ptr<qbrowser_storage_detail::PosixStableFile> file;
};

bool readPosixDirectoryNames(
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot,
    std::vector<QString> &names)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int enumerationDescriptor = ::openat(
        stableRoot.descriptor(), ".", flags);
    if (enumerationDescriptor < 0) {
        return false;
    }
    DIR *const directory = ::fdopendir(enumerationDescriptor);
    if (directory == nullptr) {
        (void)::close(enumerationDescriptor);
        return false;
    }

    bool complete = true;
    for (;;) {
        errno = 0;
        const dirent *const entry = ::readdir(directory);
        if (entry == nullptr) {
            complete = errno == 0;
            break;
        }
        const QByteArray encodedName(entry->d_name);
        if (encodedName == QByteArrayLiteral(".")
            || encodedName == QByteArrayLiteral("..")) {
            continue;
        }
        if (names.size()
            >= static_cast<size_t>(maximumExistingStorageObjects)) {
            complete = false;
            break;
        }
        names.emplace_back(QString::fromLatin1(encodedName));
    }
    const bool closed = ::closedir(directory) == 0;
    return complete && closed;
}

bool validateExistingLayout(
    const QString &rootDirectory,
    const qint64 quotaBytes,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot,
    std::vector<ValidatedPosixStorageObject> &objects)
{
    std::vector<QString> names;
    if (!readPosixDirectoryNames(stableRoot, names)) {
        return false;
    }
    qint64 aggregateDataBytes = 0;
    qint64 aggregateLockBytes = 0;
    for (const QString &name : names) {
        bool lockFile = false;
        if (!validStorageObjectName(name, lockFile)) {
            return false;
        }
        const qint64 maximumBytes = lockFile ? maximumLockBytes : quotaBytes;
        const QByteArray encodedName = QFile::encodeName(name);
        struct stat status {};
        if (::fstatat(stableRoot.descriptor(), encodedName.constData(),
                      &status, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(status.st_mode) || status.st_nlink != 1
            || status.st_size < 0 || status.st_size > maximumBytes) {
            return false;
        }
        auto file = std::make_unique<qbrowser_storage_detail::PosixStableFile>();
        if (!file->openAt(stableRoot, name, maximumBytes)
            || !stableRoot.isSameIdentityAt(rootDirectory)) {
            return false;
        }
        const qint64 expected = file->size();
        qint64 &aggregate = lockFile ? aggregateLockBytes : aggregateDataBytes;
        if (expected < 0 || expected > maximumBytes || expected > maximumBytes - aggregate) {
            return false;
        }
        aggregate += expected;
        objects.push_back({name,
                           QDir(rootDirectory).filePath(name),
                           std::move(file)});
    }
    return stableRoot.isSameIdentityAt(rootDirectory);
}

bool posixMembershipMatches(
    const std::vector<ValidatedPosixStorageObject> &objects,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    std::vector<QString> names;
    if (!readPosixDirectoryNames(stableRoot, names)
        || names.size() != objects.size()) {
        return false;
    }
    QHash<QString, const ValidatedPosixStorageObject *> expected;
    expected.reserve(static_cast<qsizetype>(objects.size()));
    for (const ValidatedPosixStorageObject &object : objects) {
        if (object.file == nullptr || expected.contains(object.name)) {
            return false;
        }
        expected.insert(object.name, &object);
    }
    QSet<QString> seen;
    for (const QString &name : names) {
        const auto object = expected.constFind(name);
        if (object == expected.cend() || seen.contains(name)
            || !(*object)->file->isSameIdentityAt(stableRoot, name)) {
            return false;
        }
        seen.insert(name);
    }
    return seen.size() == expected.size();
}

struct PermissionSnapshot final
{
    QString path;
    QString name;
    qbrowser_storage_detail::PosixStableFile *file = nullptr;
    mode_t permissions = 0;
    mode_t required = 0;
};

bool permissionTargetIsStable(
    const PermissionSnapshot &snapshot,
    const QString &rootDirectory,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    return stableRoot.isSameIdentityAt(rootDirectory)
        && (snapshot.file == nullptr
                || snapshot.file->isSameIdentityAt(stableRoot, snapshot.name));
}

bool permissionTargetPermissions(
    const PermissionSnapshot &snapshot,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot,
    mode_t &permissions)
{
    return snapshot.file != nullptr
        ? snapshot.file->permissions(permissions)
        : stableRoot.permissions(permissions);
}

bool setPermissionTargetPermissions(
    const PermissionSnapshot &snapshot,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot,
    const mode_t permissions)
{
    return snapshot.file != nullptr
        ? snapshot.file->setPermissions(permissions)
        : stableRoot.setPermissions(permissions);
}

bool hasRequiredOwnerOnlyPermissions(
    const PermissionSnapshot &snapshot,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    mode_t actual = 0;
    return permissionTargetPermissions(snapshot, stableRoot, actual)
        && actual == snapshot.required;
}

bool applyPermissionsTransactionally(
    const QString &rootDirectory,
    const std::vector<ValidatedPosixStorageObject> &objects,
    const qbrowser_storage_detail::PosixStableDirectory &stableRoot)
{
    if (!stableRoot.isSameIdentityAt(rootDirectory)
        || !posixMembershipMatches(objects, stableRoot)) {
        return false;
    }
    std::vector<PermissionSnapshot> snapshots;
    snapshots.reserve(objects.size() + 1U);
    for (const ValidatedPosixStorageObject &object : objects) {
        mode_t permissions = 0;
        if (!object.file->permissions(permissions)) {
            return false;
        }
        snapshots.push_back(
            {object.path, object.name, object.file.get(), permissions,
             static_cast<mode_t>(0600)});
    }
    mode_t directoryPermissions = 0;
    if (!stableRoot.permissions(directoryPermissions)) {
        return false;
    }
    snapshots.push_back({rootDirectory, {}, nullptr, directoryPermissions,
                         static_cast<mode_t>(0700)});

    std::vector<bool> dirty(snapshots.size(), false);
    bool complete = true;
    for (qsizetype index = 0; index < static_cast<qsizetype>(snapshots.size()); ++index) {
        const PermissionSnapshot &snapshot = snapshots[static_cast<size_t>(index)];
        if (!permissionTargetIsStable(snapshot, rootDirectory, stableRoot)) {
            complete = false;
            break;
        }
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::storageTestHooks();
        if (hooks.allowPermissionApply
            && !hooks.allowPermissionApply(snapshot.path, index)) {
            complete = false;
            break;
        }
#endif
        if (!permissionTargetIsStable(snapshot, rootDirectory, stableRoot)
            || !setPermissionTargetPermissions(
                snapshot, stableRoot, snapshot.required)) {
            complete = false;
            break;
        }
        dirty[static_cast<size_t>(index)] = true;
#ifdef Q_BROWSER_BROKER_TESTING
        if (hooks.allowPermissionPostcheck
            && !hooks.allowPermissionPostcheck(snapshot.path, index)) {
            complete = false;
            break;
        }
#endif
        if (!permissionTargetIsStable(snapshot, rootDirectory, stableRoot)
            || !hasRequiredOwnerOnlyPermissions(snapshot, stableRoot)) {
            complete = false;
            break;
        }
    }
    if (complete && stableRoot.isSameIdentityAt(rootDirectory)
        && posixMembershipMatches(objects, stableRoot)) {
        return true;
    }
    bool restored = true;
    for (qsizetype index = static_cast<qsizetype>(snapshots.size()); index > 0; --index) {
        const size_t snapshotIndex = static_cast<size_t>(index - 1);
        if (dirty[snapshotIndex]) {
            const PermissionSnapshot &snapshot = snapshots[snapshotIndex];
            const bool currentRestored = setPermissionTargetPermissions(
                snapshot, stableRoot, snapshot.permissions);
            restored = currentRestored && restored;
        }
    }
    (void)restored;
    return false;
}
#endif

} // namespace

StorageBroker::StorageBroker(
    EffectiveStoragePolicy policy,
    QString rootDirectory,
    QString canonicalRoot,
    std::shared_ptr<qbrowser_storage_detail::RootTransactionLock> rootTransactionLock)
    : policy_(policy),
      rootDirectory_(std::move(rootDirectory)),
      canonicalRoot_(std::move(canonicalRoot)),
      rootTransactionLock_(std::move(rootTransactionLock))
{
}

StorageBroker::~StorageBroker() = default;

std::unique_ptr<StorageBroker> StorageBroker::create(EffectiveStoragePolicy policy,
                                                     const QString &rootDirectory,
                                                     QString *errorCode)
{
    const auto fail = [&](const QString &code) -> std::unique_ptr<StorageBroker> {
        if (errorCode != nullptr) {
            *errorCode = code;
        }
        return nullptr;
    };
    const QFileInfo rootInfo(rootDirectory);
    if (policy.quotaBytes <= 0 || policy.quotaBytes > 16LL * 1024LL * 1024LL
        || !rootInfo.exists() || !rootInfo.isDir()) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    const QString absoluteRoot = QDir::cleanPath(rootInfo.absoluteFilePath());
    const QString canonicalRoot = canonicalRootKey(rootInfo);
    if (canonicalRoot.isEmpty()) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    auto rootTransactionLock = transactionLockForRoot(canonicalRoot);
    std::unique_lock initializationGuard(rootTransactionLock->mutex, std::defer_lock);
    if (!initializationGuard.try_lock()) {
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::storageTestHooks();
        if (hooks.initializationLockContended) {
            hooks.initializationLockContended(canonicalRoot);
        }
#endif
        if (callerIsGuiThread()) {
            // Direct GUI callers never wait. Host runtimes initialize this
            // broker on their capability worker lane and retry this result.
            return fail(QStringLiteral("storage.busy"));
        }
        initializationGuard.lock();
    }
    const QFileInfo lockedRootInfo(absoluteRoot);
    if (!lockedRootInfo.exists() || !lockedRootInfo.isDir()
        || canonicalRootKey(lockedRootInfo) != canonicalRoot) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    const int interprocessTimeout = callerIsGuiThread()
        ? 0
        : rootTransactionLockTimeoutMilliseconds;
    auto broker = std::unique_ptr<StorageBroker>(
        new StorageBroker(
            policy, absoluteRoot, canonicalRoot, rootTransactionLock));
#ifdef Q_OS_WIN
    qbrowser_storage_detail::WindowsRootTransactionGuard stableInterprocessLock(
        canonicalRoot);
    if (!stableInterprocessLock.tryLock(interprocessTimeout)) {
        return fail(stableInterprocessLock.invalid()
                        ? QStringLiteral("storage.invalid_root")
                        : QStringLiteral("storage.busy"));
    }
    QLockFile interprocessLock(rootTransactionLockPath(canonicalRoot));
    interprocessLock.setStaleLockTime(0);
    if (!interprocessLock.tryLock(interprocessTimeout)) {
        return fail(interprocessLock.error() == QLockFile::LockFailedError
                        ? QStringLiteral("storage.busy")
                        : QStringLiteral("storage.invalid_root"));
    }
#else
    broker->stableRoot_ =
        std::make_unique<qbrowser_storage_detail::PosixStableDirectory>();
    if (containsSymlinkAncestor(absoluteRoot)
        || !broker->stableRoot_->openRoot(absoluteRoot)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    qbrowser_storage_detail::PosixRootTransactionGuard interprocessLock(
        *broker->stableRoot_);
    if (!interprocessLock.tryLock(interprocessTimeout)) {
        return fail(interprocessLock.invalid()
                        ? QStringLiteral("storage.invalid_root")
                        : QStringLiteral("storage.busy"));
    }
    if (!broker->stableRoot_->isSameIdentityAt(absoluteRoot)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#endif
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &initializationHooks = qbrowser_broker_testing::storageTestHooks();
    if (initializationHooks.afterInitializationLockAcquired) {
        initializationHooks.afterInitializationLockAcquired(canonicalRoot);
    }
#endif
#ifdef Q_OS_WIN
    broker->stableRoot_ =
        std::make_unique<qbrowser_archive_detail::WindowsStableDirectoryTree>();
    std::vector<ValidatedStorageObject> objects;
    DaclSnapshot originalRoot;
    if (!broker->stableRoot_->openSharedRoot(absoluteRoot)
        || !broker->stableRoot_->isStable()) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    if (!validateExistingLayout(absoluteRoot,
                                policy.quotaBytes,
                                *broker->stableRoot_,
                                objects)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    if (!broker->stableRoot_->isStable() || !captureDacl(absoluteRoot, originalRoot)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    if (!broker->stableRoot_->isStable() || !freezeDirectoryMembership(originalRoot)) {
        (void)restoreDacl(originalRoot);
        return fail(QStringLiteral("storage.invalid_root"));
    }
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &hooks = qbrowser_broker_testing::storageTestHooks();
    if (hooks.afterMembershipFrozen) {
        hooks.afterMembershipFrozen();
    }
#endif
    if (!frozenMembershipMatches(absoluteRoot, objects, *broker->stableRoot_)) {
        (void)restoreDacl(originalRoot);
        return fail(QStringLiteral("storage.invalid_root"));
    }
    if (!applyAclsTransactionally(objects,
                                  *broker->stableRoot_,
                                  std::move(originalRoot))) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#else
    std::vector<ValidatedPosixStorageObject> objects;
    if (!validateExistingLayout(absoluteRoot, policy.quotaBytes,
                                *broker->stableRoot_, objects)
        || !broker->stableRoot_->isSameIdentityAt(absoluteRoot)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &hooks = qbrowser_broker_testing::storageTestHooks();
    if (hooks.afterMembershipFrozen) {
        hooks.afterMembershipFrozen();
    }
#endif
    if (!broker->stableRoot_->isSameIdentityAt(absoluteRoot)
        || !posixMembershipMatches(objects, *broker->stableRoot_)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    if (!applyPermissionsTransactionally(absoluteRoot, objects, *broker->stableRoot_)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#endif
    if (errorCode != nullptr) {
        errorCode->clear();
    }
    return broker;
}

bool StorageBroker::rootIsStable() const
{
#ifdef Q_OS_WIN
    return stableRoot_ != nullptr && stableRoot_->isStable();
#else
    return stableRoot_ != nullptr && stableRoot_->isSameIdentityAt(rootDirectory_)
        && !containsSymlinkAncestor(rootDirectory_);
#endif
}

BrokerResult StorageBroker::invoke(const QString &operation,
                                   const QJsonObject &payload,
                                   const HostRequestContext &context)
{
    const bool getOrRemove = operation == QStringLiteral("get")
        || operation == QStringLiteral("remove");
    const bool set = operation == QStringLiteral("set");
    const QSet<QString> expected = set
        ? QSet<QString>{QStringLiteral("key"), QStringLiteral("value")}
        : QSet<QString>{QStringLiteral("key")};
    if ((!getOrRemove && !set) || !hasExactKeys(payload, expected)
        || !validKey(payload.value(QStringLiteral("key"))) || context.appIdentity.isEmpty()) {
        return BrokerResult::failure(QStringLiteral("storage.invalid_request"),
                                     QStringLiteral("Storage request is invalid."));
    }
    std::unique_lock transactionGuard(rootTransactionLock_->mutex, std::defer_lock);
    if (!transactionGuard.try_lock()) {
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::storageTestHooks();
        if (hooks.storageOperationLockContended) {
            hooks.storageOperationLockContended(rootDirectory_);
        }
#endif
        transactionGuard.lock();
    }
#ifdef Q_BROWSER_BROKER_TESTING
    const auto &operationHooks = qbrowser_broker_testing::storageTestHooks();
    if (operationHooks.afterStorageOperationLockAcquired) {
        operationHooks.afterStorageOperationLockAcquired(rootDirectory_);
    }
#endif
    if (!rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
#ifdef Q_OS_WIN
    qbrowser_storage_detail::WindowsRootTransactionGuard stableRootLock(
        canonicalRoot_);
    if (!stableRootLock.tryLock(rootTransactionLockTimeoutMilliseconds)) {
        return BrokerResult::failure(
            stableRootLock.invalid()
                ? QStringLiteral("storage.failed")
                : QStringLiteral("storage.busy"),
            stableRootLock.invalid()
                ? QStringLiteral("Storage is unavailable.")
                : QStringLiteral("Storage is busy."));
    }
    QLockFile rootLock(rootTransactionLockPath(canonicalRoot_));
    rootLock.setStaleLockTime(0);
    const bool rootLocked = rootLock.tryLock(
        rootTransactionLockTimeoutMilliseconds);
#else
    qbrowser_storage_detail::PosixRootTransactionGuard rootLock(*stableRoot_);
    const bool rootLocked = rootLock.tryLock(
        rootTransactionLockTimeoutMilliseconds);
#endif
    if (!rootLocked) {
#ifndef Q_OS_WIN
        if (rootLock.invalid()) {
            return BrokerResult::failure(
                QStringLiteral("storage.failed"),
                QStringLiteral("Storage is unavailable."));
        }
#endif
        return BrokerResult::failure(QStringLiteral("storage.busy"),
                                     QStringLiteral("Storage is busy."));
    }
    if (!rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }

#ifdef Q_BROWSER_BROKER_TESTING
    const auto &storageIoHooks = qbrowser_broker_testing::storageTestHooks();
    if (storageIoHooks.beforeStorageIo) {
        storageIoHooks.beforeStorageIo(rootDirectory_);
    }
#endif
#ifdef Q_OS_WIN
    const QString path = storagePath(rootDirectory_, context.appIdentity);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(concurrentUpdateLockTimeoutMilliseconds) || !rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.busy"),
                                     QStringLiteral("Storage is busy."));
    }
#else
    const QString name = storageObjectName(context.appIdentity);
#endif

    QJsonObject values;
#ifdef Q_OS_WIN
    const BrokerResult loaded = loadValues(path, policy_.quotaBytes, values, *stableRoot_);
#else
    const BrokerResult loaded = loadValues(
        name, policy_.quotaBytes, values, *stableRoot_);
#endif
    if (!loaded.ok) {
        return loaded;
    }
    if (!rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    const QString key = payload.value(QStringLiteral("key")).toString();
    if (operation == QStringLiteral("get")) {
        if (!values.contains(key)) {
            return BrokerResult::failure(QStringLiteral("storage.not_found"),
                                         QStringLiteral("Storage value was not found."));
        }
        return BrokerResult::success(
            QJsonObject{{QStringLiteral("value"), values.value(key)}});
    }

    if (set) {
        values.insert(key, payload.value(QStringLiteral("value")));
    } else {
        values.remove(key);
    }
    const QByteArray bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
    if (bytes.size() > policy_.quotaBytes) {
        return BrokerResult::failure(QStringLiteral("storage.quota"),
                                     QStringLiteral("Storage quota was exceeded."));
    }
#ifdef Q_OS_WIN
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()
        || !output.commit() || !rootIsStable()) {
        output.cancelWriting();
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    qbrowser_archive_detail::WindowsStableFile published;
    QByteArray verified;
    if (!published.openReadLocked(path, *stableRoot_)
        || !rootIsStable()
        || !applyHostOnlyAcl(path)
        || !published.readExact(static_cast<quint64>(bytes.size()),
                                static_cast<quint64>(policy_.quotaBytes),
                                verified)
        || verified != bytes) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
#else
    if (!rootIsStable()
        || !publishValues(name, bytes, *stableRoot_)
        || !rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
#endif
    return BrokerResult::success();
}
