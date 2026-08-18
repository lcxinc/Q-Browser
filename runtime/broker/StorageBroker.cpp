#include "StorageBroker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    if (!broker->stableRoot_->openRoot(absoluteRoot) || !applyHostOnlyAcl(absoluteRoot)
        || !broker->stableRoot_->isStable()) {
        return fail(QStringLiteral("storage.invalid_root"));
    }
#else
    if (containsSymlinkAncestor(absoluteRoot)
        || !QFile::setPermissions(absoluteRoot,
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
