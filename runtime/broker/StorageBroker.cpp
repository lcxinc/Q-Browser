#include "StorageBroker.h"
#include "StorageTestHooks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#include <Aclapi.h>
#include <vector>
#endif

namespace {

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

QString storagePath(const QString &rootDirectory, const QString &identity)
{
    const QByteArray digest = QCryptographicHash::hash(identity.toUtf8(),
                                                       QCryptographicHash::Sha256)
                                  .toHex();
    return QDir(rootDirectory).filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

constexpr qsizetype maximumExistingStorageObjects = 8192;
constexpr qint64 maximumLockBytes = 64 * 1024;

bool validStorageObjectName(const QString &name, bool &lockFile)
{
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

struct DaclSnapshot final
{
    QString path;
    QByteArray acl;
    bool protectedAcl = false;
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
        if (++count > maximumExistingStorageObjects) {
            return false;
        }
        const QFileInfo information = iterator.fileInfo();
        bool lockFile = false;
        if (!information.isFile() || information.isSymLink()
            || !validStorageObjectName(information.fileName(), lockFile)) {
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

bool applyAclsTransactionally(
    const QString &rootDirectory,
    const std::vector<ValidatedStorageObject> &objects,
    const qbrowser_archive_detail::WindowsStableDirectoryTree &stableRoot)
{
    std::vector<DaclSnapshot> snapshots;
    snapshots.reserve(objects.size() + 1U);
    for (const ValidatedStorageObject &object : objects) {
        DaclSnapshot snapshot;
        if (!captureDacl(object.path, snapshot)) {
            return false;
        }
        snapshots.push_back(std::move(snapshot));
    }
    DaclSnapshot rootSnapshot;
    if (!captureDacl(rootDirectory, rootSnapshot)) {
        return false;
    }
    snapshots.push_back(std::move(rootSnapshot));

    qsizetype applied = 0;
    for (; applied < static_cast<qsizetype>(snapshots.size()); ++applied) {
#ifdef Q_BROWSER_BROKER_TESTING
        const auto &hooks = qbrowser_broker_testing::storageTestHooks();
        if (hooks.allowAclApply && !hooks.allowAclApply(snapshots[applied].path, applied)) {
            break;
        }
#endif
        if (!applyHostOnlyAcl(snapshots[applied].path)
            || !hasHostOnlyAcl(snapshots[applied].path)) {
            break;
        }
    }
    if (applied == static_cast<qsizetype>(snapshots.size()) && stableRoot.isStable()) {
        return true;
    }
    bool restored = true;
    for (qsizetype index = applied; index > 0; --index) {
        restored = restoreDacl(snapshots[static_cast<size_t>(index - 1)]) && restored;
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

BrokerResult loadValues(const QString &path, const qint64 quotaBytes, QJsonObject &values)
{
    QFile file(path);
    if (!file.exists()) {
        values = {};
        return BrokerResult::success();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    const qsizetype readLimit = static_cast<qsizetype>(quotaBytes);
    const QByteArray bytes = file.read(readLimit + 1);
    if (bytes.size() > readLimit) {
        return BrokerResult::failure(QStringLiteral("storage.quota"),
                                     QStringLiteral("Storage quota was exceeded."));
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

bool validateExistingLayout(const QString &rootDirectory, const qint64 quotaBytes)
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
        if (++count > maximumExistingStorageObjects) {
            return false;
        }
        const QFileInfo information = iterator.fileInfo();
        bool lockFile = false;
        if (!information.isFile() || information.isSymLink()
            || !validStorageObjectName(information.fileName(), lockFile)) {
            return false;
        }
        const qint64 maximumBytes = lockFile ? maximumLockBytes : quotaBytes;
        const qint64 expected = information.size();
        qint64 &aggregate = lockFile ? aggregateLockBytes : aggregateDataBytes;
        if (expected < 0 || expected > maximumBytes || expected > maximumBytes - aggregate) {
            return false;
        }
        aggregate += expected;
        QFile file(information.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)
            || file.read(static_cast<qsizetype>(maximumBytes) + 1).size()
                > maximumBytes) {
            return false;
        }
    }
    return true;
}
#endif

} // namespace

StorageBroker::StorageBroker(EffectiveStoragePolicy policy, QString rootDirectory)
    : policy_(policy), rootDirectory_(std::move(rootDirectory))
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
    auto broker = std::unique_ptr<StorageBroker>(new StorageBroker(policy, absoluteRoot));
#ifdef Q_OS_WIN
    broker->stableRoot_ =
        std::make_unique<qbrowser_archive_detail::WindowsStableDirectoryTree>();
    std::vector<ValidatedStorageObject> objects;
    if (!broker->stableRoot_->openRoot(absoluteRoot)
        || !broker->stableRoot_->isStable()
        || !validateExistingLayout(absoluteRoot,
                                   policy.quotaBytes,
                                   *broker->stableRoot_,
                                   objects)
        || !applyAclsTransactionally(absoluteRoot, objects, *broker->stableRoot_)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#else
    if (containsSymlinkAncestor(absoluteRoot)
        || !validateExistingLayout(absoluteRoot, policy.quotaBytes)) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
    QDirIterator permissionIterator(absoluteRoot,
                                    QDir::Files | QDir::Hidden | QDir::System,
                                    QDirIterator::NoIteratorFlags);
    while (permissionIterator.hasNext()) {
        permissionIterator.next();
        if (!QFile::setPermissions(permissionIterator.filePath(),
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            return fail(QStringLiteral("storage.invalid_root"));
        }
    }
    if (!QFile::setPermissions(absoluteRoot,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner)) {
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
    return QFileInfo(rootDirectory_).isDir() && !containsSymlinkAncestor(rootDirectory_);
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
    if (!rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }

    const QString path = storagePath(rootDirectory_, context.appIdentity);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(1000) || !rootIsStable()) {
        return BrokerResult::failure(QStringLiteral("storage.busy"),
                                     QStringLiteral("Storage is busy."));
    }

    QJsonObject values;
#ifdef Q_OS_WIN
    const BrokerResult loaded = loadValues(path, policy_.quotaBytes, values, *stableRoot_);
#else
    const BrokerResult loaded = loadValues(path, policy_.quotaBytes, values);
#endif
    if (!loaded.ok) {
        return loaded;
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
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()
        || !output.commit() || !rootIsStable()) {
        output.cancelWriting();
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableFile published;
    QByteArray verified;
    if (!published.openReadLocked(path, *stableRoot_)
        || !applyHostOnlyAcl(path)
        || !published.readExact(static_cast<quint64>(bytes.size()),
                                static_cast<quint64>(policy_.quotaBytes),
                                verified)
        || verified != bytes) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
#endif
    return BrokerResult::success();
}
