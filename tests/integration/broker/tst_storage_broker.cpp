#include "StorageBroker.h"
#include "StorageTestHooks.h"

#include <QCoreApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <future>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <Aclapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

QJsonObject setPayload(const QString &key, const QJsonValue &value)
{
    return {{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QJsonObject keyPayload(const QString &key)
{
    return {{QStringLiteral("key"), key}};
}

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
}

std::unique_ptr<StorageBroker> createBroker(const EffectiveStoragePolicy policy,
                                            const QString &root)
{
    QString error;
    std::unique_ptr<StorageBroker> broker = StorageBroker::create(policy, root, &error);
    if (broker == nullptr) {
        qWarning("StorageBroker create failed: %s", qPrintable(error));
    }
    return broker;
}

#ifdef Q_OS_WIN
QString rootTransactionMutexName(const QString &root)
{
    QString canonical = QDir::cleanPath(QDir::fromNativeSeparators(
        QFileInfo(root).canonicalFilePath())).toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash(
        canonical.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("Local\\QBrowser.Storage.Root.")
        + QString::fromLatin1(digest);
}

QString rootTransactionLockPath(const QString &root)
{
    const QString canonical = QDir::cleanPath(QDir::fromNativeSeparators(
        QFileInfo(root).canonicalFilePath())).toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash(
        canonical.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QDir::tempPath()).filePath(
        QStringLiteral(".qbrowser-storage-root-%1.lock")
            .arg(QString::fromLatin1(digest)));
}

bool grantWorldAccess(const QString &path)
{
    BYTE worldBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD worldSize = sizeof(worldBuffer);
    if (CreateWellKnownSid(WinWorldSid, nullptr, worldBuffer, &worldSize) == FALSE) {
        return false;
    }
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = GENERIC_ALL;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(worldBuffer);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const DWORD result = SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        acl,
        nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

bool hasProtectedHostOnlyDacl(const QString &path)
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
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool valid = GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE
        && (control & SE_DACL_PROTECTED) != 0U;
    BYTE worldBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD worldSize = sizeof(worldBuffer);
    valid = valid
        && CreateWellKnownSid(WinWorldSid, nullptr, worldBuffer, &worldSize) != FALSE;
    ACL_SIZE_INFORMATION information{};
    valid = valid
        && GetAclInformation(dacl,
                             &information,
                             static_cast<DWORD>(sizeof(information)),
                             AclSizeInformation)
            != FALSE;
    for (DWORD index = 0; valid && index < information.AceCount; ++index) {
        void *ace = nullptr;
        if (GetAce(dacl, index, &ace) == FALSE) {
            valid = false;
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(ace);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(ace);
            if (EqualSid(const_cast<DWORD *>(&allowed->SidStart), worldBuffer) != FALSE) {
                valid = false;
            }
        }
    }
    LocalFree(descriptor);
    return valid;
}

QByteArray aclSnapshot(const QString &path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr) {
        return {};
    }
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION information{};
    const bool valid = GetSecurityDescriptorDacl(descriptor,
                                                 &present,
                                                 &dacl,
                                                 &defaulted)
            != FALSE
        && present != FALSE && dacl != nullptr
        && GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE
        && GetAclInformation(dacl,
                             &information,
                             static_cast<DWORD>(sizeof(information)),
                             AclSizeInformation)
            != FALSE;
    QByteArray snapshot;
    if (valid) {
        snapshot.append(reinterpret_cast<const char *>(&control), sizeof(control));
        snapshot.append(reinterpret_cast<const char *>(dacl),
                        static_cast<qsizetype>(information.AclBytesInUse));
    }
    LocalFree(descriptor);
    return snapshot;
}
#endif

} // namespace

class StorageBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void isolatesDataByHostAssignedIdentity();
    void enforcesQuotaWithoutMutatingExistingValue();
    void rejectsPayloadIdentityAndInvalidKeys();
    void rejectsExistingNamespaceOverQuota();
    void validatesAndTightensExistingNamespaceLayout();
    void descriptorMembershipReconciliationHandlesNonEmptyRoot();
    void rejectsUnexpectedExistingStorageObjects();
    void invalidLayoutDoesNotMutateAcls();
    void rejectsAggregateExistingDataOverQuota();
    void aclMigrationFailureRollsBackEveryObject();
    void aclPostcheckFailureRollsBackCurrentObject();
    void freezesMembershipDuringValidationAndAclMigration();
    void canonicalRootAliasesSerializeInitialization();
    void failedInitializationReleasesRootLockAndRestoresState();
    void sameRootInitializationSerializesWithUpdates();
    void guiInitializationReturnsBusyWhileRootTransactionIsActive();
    void honorsInterprocessRootTransactionLock();
    void holdInterprocessRootTransactionLockForTest();
    void serializesConcurrentQuotaUpdates();
    void rejectsReplacedRootIdentity();
    void initializationRejectsReplacementRootBeforePermissionMigration();
    void rejectsHardLinkedNamespaceFile();
    void storageIoDoesNotFollowReplacedRoot();
    void permissionMigrationDoesNotFollowReplacedMember();
    void rejectsReparseRoot();
    void rejectsReparseNamespaceFile();
};

void StorageBrokerTest::isolatesDataByHostAssignedIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(broker != nullptr);

    QVERIFY(broker->invoke(QStringLiteral("set"),
                          setPayload(QStringLiteral("theme"), QStringLiteral("dark")),
                          {QStringLiteral("host.app.one"), QStringLiteral("request")})
                .ok);

    BrokerResult result = broker->invoke(QStringLiteral("get"),
                                        keyPayload(QStringLiteral("theme")),
                                        {QStringLiteral("host.app.two"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.not_found"));

    result = broker->invoke(QStringLiteral("get"),
                           keyPayload(QStringLiteral("theme")),
                           {QStringLiteral("host.app.one"), QStringLiteral("request")});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("value")).toString(),
             QStringLiteral("dark"));
    QCOMPARE(QDir(root.path()).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
}

void StorageBrokerTest::enforcesQuotaWithoutMutatingExistingValue()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto broker = createBroker(EffectiveStoragePolicy{40}, root.path());
    QVERIFY(broker != nullptr);
    const HostRequestContext context{QStringLiteral("host.app"), QStringLiteral("request")};

    QVERIFY(broker->invoke(QStringLiteral("set"),
                          setPayload(QStringLiteral("key"), QStringLiteral("small")),
                          context)
                .ok);
    const BrokerResult denied = broker->invoke(QStringLiteral("set"),
                                              setPayload(QStringLiteral("key"),
                                                         QString(100, u'x')),
                                              context);
    QVERIFY(!denied.ok);
    QCOMPARE(denied.errorCode, QStringLiteral("storage.quota"));

    const BrokerResult existing = broker->invoke(QStringLiteral("get"),
                                                keyPayload(QStringLiteral("key")),
                                                context);
    QVERIFY(existing.ok);
    QCOMPARE(existing.value.value(QStringLiteral("value")).toString(),
             QStringLiteral("small"));
}

void StorageBrokerTest::rejectsPayloadIdentityAndInvalidKeys()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(broker != nullptr);
    const HostRequestContext context{QStringLiteral("host.app"), QStringLiteral("request")};

    QJsonObject spoofed = setPayload(QStringLiteral("key"), QStringLiteral("value"));
    spoofed.insert(QStringLiteral("appIdentity"), QStringLiteral("victim"));
    BrokerResult result = broker->invoke(QStringLiteral("set"), spoofed, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.invalid_request"));

    result = broker->invoke(QStringLiteral("set"),
                           setPayload(QStringLiteral("../escape"), QStringLiteral("value")),
                           context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.invalid_request"));
    QCOMPARE(QDir(root.path()).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 0);
}

void StorageBrokerTest::rejectsExistingNamespaceOverQuota()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString identity = QStringLiteral("host.app");
    const QString fileName = QString::fromLatin1(
                                 QCryptographicHash::hash(identity.toUtf8(),
                                                          QCryptographicHash::Sha256)
                                     .toHex())
        + QStringLiteral(".json");
    QFile file(QDir(root.path()).filePath(fileName));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray("{\"key\":\"") + QByteArray(100, 'x') + QByteArray("\"}"))
            > 40);
    file.close();

    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{40}, root.path(), &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
}

void StorageBrokerTest::validatesAndTightensExistingNamespaceLayout()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString identity = QStringLiteral("host.app");
    const QString fileName = QString::fromLatin1(
                                 QCryptographicHash::hash(identity.toUtf8(),
                                                          QCryptographicHash::Sha256)
                                     .toHex())
        + QStringLiteral(".json");
    const QString path = QDir(root.path()).filePath(fileName);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{\"theme\":\"dark\"}"), 16);
    file.close();
    QVERIFY(grantWorldAccess(path));
    QVERIFY(!hasProtectedHostOnlyDacl(path));

    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(broker != nullptr);
    QVERIFY(hasProtectedHostOnlyDacl(path));
    const BrokerResult result = broker->invoke(QStringLiteral("get"),
                                               keyPayload(QStringLiteral("theme")),
                                               {identity, QStringLiteral("request")});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("value")).toString(), QStringLiteral("dark"));
#endif
}

void StorageBrokerTest::descriptorMembershipReconciliationHandlesNonEmptyRoot()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX independent directory-stream offset coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString namespacePath = QDir(root.path()).filePath(
        QString(64, u'd') + QStringLiteral(".json"));
    writeFile(namespacePath, QByteArrayLiteral("{}"));
    const QFileDevice::Permissions broadPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    QVERIFY(QFile::setPermissions(namespacePath, broadPermissions));

    QString error;
    auto broker = StorageBroker::create(
        EffectiveStoragePolicy{1024}, root.path(), &error);
    QVERIFY2(broker != nullptr, qPrintable(error));
    const QFileDevice::Permissions migrated =
        QFileInfo(namespacePath).permissions();
    QVERIFY(migrated.testFlag(QFileDevice::ReadOwner));
    QVERIFY(migrated.testFlag(QFileDevice::WriteOwner));
    QVERIFY(!(migrated & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                         | QFileDevice::ReadOther | QFileDevice::WriteOther)));
#endif
}

void StorageBrokerTest::rejectsUnexpectedExistingStorageObjects()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("unexpected")));
    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
}

void StorageBrokerTest::invalidLayoutDoesNotMutateAcls()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL transaction coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString unexpected = QDir(root.path()).filePath(QStringLiteral("unexpected"));
    QVERIFY(QDir().mkdir(unexpected));
    QVERIFY(grantWorldAccess(root.path()));
    QVERIFY(grantWorldAccess(unexpected));
    const QByteArray rootAcl = aclSnapshot(root.path());
    const QByteArray childAcl = aclSnapshot(unexpected);
    QVERIFY(!rootAcl.isEmpty());
    QVERIFY(!childAcl.isEmpty());

    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    QCOMPARE(aclSnapshot(root.path()), rootAcl);
    QCOMPARE(aclSnapshot(unexpected), childAcl);
#endif
}

void StorageBrokerTest::rejectsAggregateExistingDataOverQuota()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (const QString &identity : {QStringLiteral("host.one"), QStringLiteral("host.two")}) {
        const QString name = QString::fromLatin1(
                                 QCryptographicHash::hash(identity.toUtf8(),
                                                          QCryptographicHash::Sha256)
                                     .toHex())
            + QStringLiteral(".json");
        QFile file(QDir(root.path()).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(30, ' ')), 30);
    }
    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{40}, root.path(), &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
}

void StorageBrokerTest::aclMigrationFailureRollsBackEveryObject()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL transaction coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QStringList paths{root.path()};
    for (const QString &identity : {QStringLiteral("host.one"), QStringLiteral("host.two")}) {
        const QString name = QString::fromLatin1(
                                 QCryptographicHash::hash(identity.toUtf8(),
                                                          QCryptographicHash::Sha256)
                                     .toHex())
            + QStringLiteral(".json");
        const QString path = QDir(root.path()).filePath(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("{}"), 2);
        file.close();
        QVERIFY(grantWorldAccess(path));
        paths.append(path);
    }
    QVERIFY(grantWorldAccess(root.path()));
    QHash<QString, QByteArray> before;
    for (const QString &path : paths) {
        before.insert(path, aclSnapshot(path));
        QVERIFY(!before.value(path).isEmpty());
    }
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = [](const QString &, const qsizetype index) {
             return index != 1;
         }});
    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error) == nullptr);
    qbrowser_broker_testing::resetStorageTestHooks();
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    for (const QString &path : paths) {
        QCOMPARE(aclSnapshot(path), before.value(path));
    }
#endif
}

void StorageBrokerTest::aclPostcheckFailureRollsBackCurrentObject()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL transaction coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QStringList paths{root.path()};
    const QString path = QDir(root.path()).filePath(
        QString(64, u'a') + QStringLiteral(".json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{}"), 2);
    file.close();
    paths.prepend(path);
    for (const QString &item : paths) {
        QVERIFY(grantWorldAccess(item));
    }
    QHash<QString, QByteArray> before;
    for (const QString &item : paths) {
        before.insert(item, aclSnapshot(item));
        QVERIFY(!before.value(item).isEmpty());
    }
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = [](const QString &, const qsizetype index) {
             return index != 0;
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    for (const QString &item : paths) {
        QCOMPARE(aclSnapshot(item), before.value(item));
    }
#endif
}

void StorageBrokerTest::freezesMembershipDuringValidationAndAclMigration()
{
#ifndef Q_OS_WIN
    QSKIP("Windows directory membership transaction coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    const QString injected = QDir(root.path()).filePath(
        QString(64, u'b') + QStringLiteral(".json"));
    const QString deletable = QDir(root.path()).filePath(
        QString(64, u'c') + QStringLiteral(".json"));
    const QString movable = QDir(root.path()).filePath(
        QString(64, u'd') + QStringLiteral(".json"));
    for (const QString &path : {deletable, movable}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("{}"), 2);
        file.close();
        QVERIFY(grantWorldAccess(path));
    }
    const QString moved = QDir(destination.path()).filePath(QStringLiteral("moved.json"));
    bool hookCalled = false;
    bool injectionSucceeded = false;
    bool deletionSucceeded = false;
    bool moveSucceeded = false;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = [&] {
             hookCalled = true;
             QFile file(injected);
             injectionSucceeded = file.open(QIODevice::WriteOnly);
             if (injectionSucceeded) {
                 (void)file.write("{}");
             }
             deletionSucceeded = DeleteFileW(
                                      reinterpret_cast<LPCWSTR>(
                                          QDir::toNativeSeparators(deletable).utf16()))
                 != FALSE;
             moveSucceeded = MoveFileExW(
                                   reinterpret_cast<LPCWSTR>(
                                       QDir::toNativeSeparators(movable).utf16()),
                                   reinterpret_cast<LPCWSTR>(
                                       QDir::toNativeSeparators(moved).utf16()),
                                   MOVEFILE_WRITE_THROUGH)
                 != FALSE;
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(hookCalled);
    QVERIFY(!injectionSucceeded);
    QVERIFY(!deletionSucceeded);
    QVERIFY(!moveSucceeded);
    QVERIFY(broker != nullptr);
    QVERIFY(!QFileInfo::exists(injected));
    QVERIFY(QFileInfo::exists(deletable));
    QVERIFY(QFileInfo::exists(movable));
    QVERIFY(!QFileInfo::exists(moved));
#endif
}

void StorageBrokerTest::canonicalRootAliasesSerializeInitialization()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    const QString sibling = QDir(parent.path()).filePath(QStringLiteral("alias-base"));
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(sibling));
    const QString rootAlias = QDir(sibling).filePath(QStringLiteral("../storage"));

    QSemaphore firstEntered;
    QSemaphore releaseFirst;
    QSemaphore initializationContended;
    std::atomic_int entries{0};
    std::atomic_int active{0};
    std::atomic_int maximumActive{0};
    QMutex lockedRootsMutex;
    QStringList lockedRoots;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = [&](const QString &canonicalRoot) {
             const int entry = entries.fetch_add(1) + 1;
             const int nowActive = active.fetch_add(1) + 1;
             int observed = maximumActive.load();
             while (nowActive > observed
                    && !maximumActive.compare_exchange_weak(observed, nowActive)) {
             }
             {
                 QMutexLocker locker(&lockedRootsMutex);
                 lockedRoots.append(canonicalRoot);
             }
             if (entry == 1) {
                 firstEntered.release();
                 releaseFirst.acquire();
             }
             active.fetch_sub(1);
         },
         .initializationLockContended = [&](const QString &) {
             initializationContended.release();
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    auto first = std::async(std::launch::async, [&] {
        QString error;
        auto broker = StorageBroker::create(EffectiveStoragePolicy{1024}, root, &error);
        return std::pair{std::move(broker), error};
    });
    const bool observedFirst = firstEntered.tryAcquire(1, 2'000);
    if (!observedFirst) {
        releaseFirst.release();
        const auto firstResult = first.get();
        Q_UNUSED(firstResult);
        QFAIL("Storage initialization did not expose the root-lock hook");
    }

    auto second = std::async(std::launch::async, [&] {
        QString error;
        auto broker = StorageBroker::create(EffectiveStoragePolicy{1024}, rootAlias, &error);
        return std::pair{std::move(broker), error};
    });
    const bool secondContended = initializationContended.tryAcquire(1, 2'000);
    releaseFirst.release();
    auto firstResult = first.get();
    auto secondResult = second.get();

    QVERIFY2(secondContended, "Canonical aliases did not contend on one root lock");
    QVERIFY2(firstResult.first != nullptr, qPrintable(firstResult.second));
    QVERIFY2(secondResult.first != nullptr, qPrintable(secondResult.second));
    QCOMPARE(entries.load(), 2);
    QCOMPARE(maximumActive.load(), 1);
    QCOMPARE(lockedRoots.size(), 2);
    QCOMPARE(lockedRoots.at(0), lockedRoots.at(1));
}

void StorageBrokerTest::failedInitializationReleasesRootLockAndRestoresState()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString namespacePath = QDir(root.path()).filePath(
        QString(64, u'a') + QStringLiteral(".json"));
    QFile file(namespacePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{}"), 2);
    file.close();
#ifdef Q_OS_WIN
    QVERIFY(grantWorldAccess(namespacePath));
    QVERIFY(grantWorldAccess(root.path()));
    const QByteArray rootBefore = aclSnapshot(root.path());
    const QByteArray namespaceBefore = aclSnapshot(namespacePath);
    QVERIFY(!rootBefore.isEmpty());
    QVERIFY(!namespaceBefore.isEmpty());
#else
    const QFileDevice::Permissions broadFilePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    const QFileDevice::Permissions broadDirectoryPermissions =
        broadFilePermissions | QFileDevice::ExeOwner | QFileDevice::ExeGroup
        | QFileDevice::ExeOther;
    QVERIFY(QFile::setPermissions(namespacePath, broadFilePermissions));
    QVERIFY(QFile::setPermissions(root.path(), broadDirectoryPermissions));
    const QFileDevice::Permissions rootBefore = QFileInfo(root.path()).permissions();
    const QFileDevice::Permissions namespaceBefore =
        QFileInfo(namespacePath).permissions();
#endif

    std::atomic_int lockEntries{0};
    std::atomic_bool failFirstApply{true};
#ifdef Q_OS_WIN
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = [&](const QString &path, const qsizetype) {
             return path != root.path() || !failFirstApply.exchange(false);
         },
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = [&](const QString &) {
             lockEntries.fetch_add(1);
         }});
#else
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = [&](const QString &) {
             lockEntries.fetch_add(1);
         },
         .initializationLockContended = {},
         .storageOperationLockContended = {},
         .afterStorageOperationLockAcquired = {},
         .allowPermissionApply = [&](const QString &path, const qsizetype) {
             return path != root.path() || !failFirstApply.exchange(false);
         }});
#endif
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    QString firstError;
    auto first = StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &firstError);
    QVERIFY(first == nullptr);
    QCOMPARE(firstError, QStringLiteral("storage.invalid_root"));
#ifdef Q_OS_WIN
    QVERIFY(!QFileInfo::exists(rootTransactionLockPath(root.path())));
    QCOMPARE(aclSnapshot(root.path()), rootBefore);
    QCOMPARE(aclSnapshot(namespacePath), namespaceBefore);
#else
    QVERIFY(!QFileInfo::exists(
        QDir(root.path()).filePath(
            QStringLiteral(".qbrowser-storage-root.lock"))));
    QCOMPARE(QFileInfo(root.path()).permissions(), rootBefore);
    QCOMPARE(QFileInfo(namespacePath).permissions(), namespaceBefore);
#endif

    QString secondError;
    auto second = StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &secondError);
    QVERIFY2(second != nullptr, qPrintable(secondError));
    QCOMPARE(lockEntries.load(), 2);
}

void StorageBrokerTest::sameRootInitializationSerializesWithUpdates()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto existing = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(existing != nullptr);
    auto retiredSibling = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(retiredSibling != nullptr);
    retiredSibling.reset();
    const HostRequestContext context{QStringLiteral("host.app"),
                                     QStringLiteral("request")};

    QSemaphore initializationEntered;
    QSemaphore releaseInitialization;
    QSemaphore operationContended;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = [&](const QString &) {
             initializationEntered.release();
             releaseInitialization.acquire();
         },
         .initializationLockContended = {},
         .storageOperationLockContended = [&](const QString &) {
             operationContended.release();
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    auto initializer = std::async(std::launch::async, [&] {
        QString error;
        auto broker = StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error);
        return std::pair{std::move(broker), error};
    });
    const bool observedInitialization = initializationEntered.tryAcquire(1, 2'000);
    if (!observedInitialization) {
        releaseInitialization.release();
        const auto initialized = initializer.get();
        Q_UNUSED(initialized);
        QFAIL("Storage initialization did not expose the root-lock hook");
    }

    auto update = std::async(std::launch::async, [&] {
        return existing->invoke(QStringLiteral("set"),
                                setPayload(QStringLiteral("theme"), QStringLiteral("dark")),
                                context);
    });
    const bool updateContended = operationContended.tryAcquire(1, 2'000);
    releaseInitialization.release();
    auto initialized = initializer.get();
    const BrokerResult updateResult = update.get();

    QVERIFY2(updateContended, "A storage update bypassed same-root initialization");
    QVERIFY2(initialized.first != nullptr, qPrintable(initialized.second));
    QVERIFY2(updateResult.ok, qPrintable(updateResult.errorCode));
    const BrokerResult value = initialized.first->invoke(
        QStringLiteral("get"), keyPayload(QStringLiteral("theme")), context);
    QVERIFY2(value.ok, qPrintable(value.errorCode));
    QCOMPARE(value.value.value(QStringLiteral("value")).toString(), QStringLiteral("dark"));
}

void StorageBrokerTest::guiInitializationReturnsBusyWhileRootTransactionIsActive()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto existing = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(existing != nullptr);
    const HostRequestContext context{QStringLiteral("host.app"),
                                     QStringLiteral("request")};

    QSemaphore operationEntered;
    QSemaphore releaseOperation;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = {},
         .initializationLockContended = {},
         .storageOperationLockContended = {},
         .afterStorageOperationLockAcquired = [&](const QString &) {
             operationEntered.release();
             releaseOperation.acquire();
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    auto update = std::async(std::launch::async, [&] {
        return existing->invoke(QStringLiteral("set"),
                                setPayload(QStringLiteral("key"), QStringLiteral("value")),
                                context);
    });
    const bool observedOperation = operationEntered.tryAcquire(1, 2'000);
    if (!observedOperation) {
        releaseOperation.release();
        const BrokerResult updateResult = update.get();
        Q_UNUSED(updateResult);
        QFAIL("Storage operation did not expose the root-lock hook");
    }

    QSemaphore createReturned;
    auto watchdog = std::async(std::launch::async, [&] {
        if (!createReturned.tryAcquire(1, 1'000)) {
            releaseOperation.release();
        }
    });
    QString error;
    auto sibling = StorageBroker::create(EffectiveStoragePolicy{1024}, root.path(), &error);
    createReturned.release();
    releaseOperation.release();
    watchdog.get();
    const BrokerResult updateResult = update.get();

    QVERIFY(sibling == nullptr);
    QCOMPARE(error, QStringLiteral("storage.busy"));
    QVERIFY2(updateResult.ok, qPrintable(updateResult.errorCode));
}

void StorageBrokerTest::honorsInterprocessRootTransactionLock()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkdir(root));
    const QString ready = QDir(parent.path()).filePath(QStringLiteral("lock-ready"));
    const QString release = QDir(parent.path()).filePath(
        QStringLiteral("lock-release"));
    QProcess holder;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("Q_BROWSER_STORAGE_LOCK_ROOT"), root);
    environment.insert(QStringLiteral("Q_BROWSER_STORAGE_LOCK_READY"), ready);
    environment.insert(QStringLiteral("Q_BROWSER_STORAGE_LOCK_RELEASE"), release);
    holder.setProcessEnvironment(environment);
    holder.setProgram(QCoreApplication::applicationFilePath());
    holder.setArguments({QStringLiteral("holdInterprocessRootTransactionLockForTest")});
    holder.start();
    QVERIFY(holder.waitForStarted(2'000));
    const auto stopHolder = qScopeGuard([&holder] {
        if (holder.state() != QProcess::NotRunning) {
            holder.kill();
            (void)holder.waitForFinished(2'000);
        }
    });
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready), 2'000);

    QString error;
    auto broker = StorageBroker::create(EffectiveStoragePolicy{1024}, root, &error);
    QVERIFY(broker == nullptr);
    QCOMPARE(error, QStringLiteral("storage.busy"));
    writeFile(release, QByteArrayLiteral("release"));
    QVERIFY(holder.waitForFinished(5'000));
    QCOMPARE(holder.exitStatus(), QProcess::NormalExit);
    QCOMPARE(holder.exitCode(), 0);
}

void StorageBrokerTest::holdInterprocessRootTransactionLockForTest()
{
    const QString root = qEnvironmentVariable("Q_BROWSER_STORAGE_LOCK_ROOT");
    const QString ready = qEnvironmentVariable("Q_BROWSER_STORAGE_LOCK_READY");
    const QString release = qEnvironmentVariable(
        "Q_BROWSER_STORAGE_LOCK_RELEASE");
    if (root.isEmpty() || ready.isEmpty() || release.isEmpty()) {
        QSKIP("interprocess storage-lock helper only");
    }
#ifdef Q_OS_WIN
    const QString mutexName = rootTransactionMutexName(root);
    HANDLE mutex = CreateMutexW(
        nullptr, FALSE,
        reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    QVERIFY(mutex != nullptr);
    const DWORD waitResult = WaitForSingleObject(mutex, 0);
    QVERIFY(waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);
    const auto releaseMutex = qScopeGuard([mutex] {
        (void)ReleaseMutex(mutex);
        CloseHandle(mutex);
    });
    QLockFile lock(rootTransactionLockPath(root));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock());
#else
    const QByteArray encoded = QFile::encodeName(root);
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(encoded.constData(), flags);
    QVERIFY(descriptor >= 0);
    const auto closeDescriptor = qScopeGuard([descriptor] {
        (void)::flock(descriptor, LOCK_UN);
        (void)::close(descriptor);
    });
    QVERIFY(::flock(descriptor, LOCK_EX | LOCK_NB) == 0);
#endif
    writeFile(ready, QByteArrayLiteral("ready"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(release), 5'000);
}

void StorageBrokerTest::serializesConcurrentQuotaUpdates()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(broker != nullptr);
    const HostRequestContext context{QStringLiteral("host.app"),
                                     QStringLiteral("request")};

    auto left = std::async(std::launch::async, [&] {
        return broker->invoke(QStringLiteral("set"),
                             setPayload(QStringLiteral("left"), 1),
                             context);
    });
    auto right = std::async(std::launch::async, [&] {
        return broker->invoke(QStringLiteral("set"),
                              setPayload(QStringLiteral("right"), 2),
                              context);
    });
    const BrokerResult leftResult = left.get();
    const BrokerResult rightResult = right.get();
    QVERIFY2(leftResult.ok, qPrintable(leftResult.errorCode));
    QVERIFY2(rightResult.ok, qPrintable(rightResult.errorCode));
    QVERIFY(broker->invoke(QStringLiteral("get"), keyPayload(QStringLiteral("left")), context).ok);
    QVERIFY(broker->invoke(QStringLiteral("get"), keyPayload(QStringLiteral("right")), context).ok);
}

void StorageBrokerTest::rejectsReplacedRootIdentity()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX device/inode replacement coverage");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    const QString retired = QDir(parent.path()).filePath(QStringLiteral("retired"));
    QVERIFY(QDir().mkdir(root));
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root);
    QVERIFY(broker != nullptr);

    QVERIFY(QDir().rename(root, retired));
    QVERIFY(QDir().mkdir(root));
    const BrokerResult result = broker->invoke(
        QStringLiteral("get"),
        keyPayload(QStringLiteral("key")),
        {QStringLiteral("host.app"), QStringLiteral("request")});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.failed"));
#endif
}

void StorageBrokerTest::initializationRejectsReplacementRootBeforePermissionMigration()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX descriptor-anchored membership coverage");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    const QString replacement = QDir(parent.path()).filePath(
        QStringLiteral("replacement"));
    const QString retired = QDir(parent.path()).filePath(
        QStringLiteral("retired"));
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(replacement));
    const QString originalNamespace = QDir(root).filePath(
        QString(64, u'a') + QStringLiteral(".json"));
    const QString replacementNamespace = QDir(replacement).filePath(
        QString(64, u'b') + QStringLiteral(".json"));
    writeFile(originalNamespace, QByteArrayLiteral("{}"));
    writeFile(replacementNamespace, QByteArrayLiteral("{}"));
    const QFileDevice::Permissions broadPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    QVERIFY(QFile::setPermissions(originalNamespace, broadPermissions));
    QVERIFY(QFile::setPermissions(replacementNamespace, broadPermissions));
    const QFileDevice::Permissions originalBefore =
        QFileInfo(originalNamespace).permissions();
    const QFileDevice::Permissions replacementBefore =
        QFileInfo(replacementNamespace).permissions();

    bool hookCalled = false;
    bool swapSucceeded = false;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = [&] {
             hookCalled = true;
             swapSucceeded = QDir().rename(root, retired)
                 && QDir().rename(replacement, root);
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    QString error;
    auto broker = StorageBroker::create(
        EffectiveStoragePolicy{1024}, root, &error);
    QVERIFY(hookCalled);
    QVERIFY(swapSucceeded);
    QVERIFY(broker == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    QCOMPARE(QFileInfo(QDir(retired).filePath(
                           QFileInfo(originalNamespace).fileName()))
                 .permissions(),
             originalBefore);
    QCOMPARE(QFileInfo(QDir(root).filePath(
                           QFileInfo(replacementNamespace).fileName()))
                 .permissions(),
             replacementBefore);
#endif
}

void StorageBrokerTest::rejectsHardLinkedNamespaceFile()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX hard-link permission-alias coverage");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkdir(root));
    const QString outside = QDir(parent.path()).filePath(
        QStringLiteral("outside.json"));
    writeFile(outside, QByteArrayLiteral("{}"));
    const QFileDevice::Permissions broadPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    QVERIFY(QFile::setPermissions(outside, broadPermissions));
    const QFileDevice::Permissions outsideBefore =
        QFileInfo(outside).permissions();
    const QString linkedNamespace = QDir(root).filePath(
        QString(64, u'c') + QStringLiteral(".json"));
    const QByteArray encodedOutside = QFile::encodeName(outside);
    const QByteArray encodedNamespace = QFile::encodeName(linkedNamespace);
    QVERIFY(::link(encodedOutside.constData(), encodedNamespace.constData()) == 0);

    QString error;
    auto broker = StorageBroker::create(
        EffectiveStoragePolicy{1024}, root, &error);
    QVERIFY(broker == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    QCOMPARE(QFileInfo(outside).permissions(), outsideBefore);
    QFile outsideFile(outside);
    QVERIFY(outsideFile.open(QIODevice::ReadOnly));
    QCOMPARE(outsideFile.readAll(), QByteArrayLiteral("{}"));
#endif
}

void StorageBrokerTest::storageIoDoesNotFollowReplacedRoot()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX directory-fd storage I/O coverage");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    const QString retired = QDir(parent.path()).filePath(QStringLiteral("retired"));
    QVERIFY(QDir().mkdir(root));
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root);
    QVERIFY(broker != nullptr);
    const HostRequestContext context{QStringLiteral("host.app"),
                                     QStringLiteral("request")};
    QVERIFY(broker->invoke(
        QStringLiteral("set"),
        setPayload(QStringLiteral("key"), QStringLiteral("original")),
        context).ok);
    const QString dataName = QString::fromLatin1(
        QCryptographicHash::hash(context.appIdentity.toUtf8(),
                                 QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".json");

    QByteArray replacementBytes = QByteArrayLiteral(
        "{\"key\":\"replacement\"}");
    bool replaced = false;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = {},
         .initializationLockContended = {},
         .storageOperationLockContended = {},
         .afterStorageOperationLockAcquired = {},
         .beforeStorageIo = [&](const QString &) {
             replaced = QDir().rename(root, retired)
                 && QDir().mkdir(root);
             if (replaced) {
                 writeFile(QDir(root).filePath(dataName), replacementBytes);
             }
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    const BrokerResult result = broker->invoke(
        QStringLiteral("set"),
        setPayload(QStringLiteral("key"), QStringLiteral("mutated")),
        context);
    QVERIFY(replaced);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.failed"));
    QFile replacement(QDir(root).filePath(dataName));
    QVERIFY(replacement.open(QIODevice::ReadOnly));
    QCOMPARE(replacement.readAll(), replacementBytes);
    QCOMPARE(QDir(root).entryList(QDir::Files | QDir::Hidden),
             QStringList{dataName});
#endif
}

void StorageBrokerTest::permissionMigrationDoesNotFollowReplacedMember()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX descriptor-anchored permission migration coverage");
#else
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString root = QDir(parent.path()).filePath(QStringLiteral("storage"));
    QVERIFY(QDir().mkdir(root));
    const QString namespacePath = QDir(root).filePath(
        QString(64, u'a') + QStringLiteral(".json"));
    const QString outsidePath = QDir(parent.path()).filePath(
        QStringLiteral("outside.json"));
    writeFile(namespacePath, QByteArrayLiteral("{}"));
    writeFile(outsidePath, QByteArrayLiteral("{}"));
    const QFileDevice::Permissions broadFilePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    const QFileDevice::Permissions broadDirectoryPermissions =
        broadFilePermissions | QFileDevice::ExeOwner | QFileDevice::ExeGroup
        | QFileDevice::ExeOther;
    QVERIFY(QFile::setPermissions(namespacePath, broadFilePermissions));
    QVERIFY(QFile::setPermissions(outsidePath, broadFilePermissions));
    QVERIFY(QFile::setPermissions(root, broadDirectoryPermissions));
    const QFileDevice::Permissions outsideBefore =
        QFileInfo(outsidePath).permissions();

    bool replaced = false;
    qbrowser_broker_testing::setStorageTestHooks(
        {.afterMembershipFrozen = {},
         .allowAclApply = {},
         .allowAclPostcheck = {},
         .afterInitializationLockAcquired = {},
         .initializationLockContended = {},
         .storageOperationLockContended = {},
         .afterStorageOperationLockAcquired = {},
         .allowPermissionApply = [&](const QString &path, const qsizetype) {
             if (path == namespacePath) {
                 replaced = QFile::remove(namespacePath)
                     && QFile::link(outsidePath, namespacePath);
             }
             return true;
         }});
    const auto reset = qScopeGuard([] {
        qbrowser_broker_testing::resetStorageTestHooks();
    });

    QString error;
    auto broker = StorageBroker::create(EffectiveStoragePolicy{1024}, root, &error);
    QVERIFY(broker == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
    QVERIFY(replaced);
    QCOMPARE(QFileInfo(outsidePath).permissions(), outsideBefore);
#endif
}

void StorageBrokerTest::rejectsReparseRoot()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString target = QDir(parent.path()).filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkdir(target));
    const QString link = QDir(parent.path()).filePath(QStringLiteral("linked-root"));
#ifdef Q_OS_WIN
    const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),
                            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                            flags)
        == FALSE) {
        QSKIP("Directory symlink creation is unavailable");
    }
#else
    if (!QFile::link(target, link)) {
        QSKIP("Directory symlink creation is unavailable");
    }
#endif
    QString error;
    QVERIFY(StorageBroker::create(EffectiveStoragePolicy{1024}, link, &error) == nullptr);
    QCOMPARE(error, QStringLiteral("storage.invalid_root"));
#ifdef Q_OS_WIN
    QVERIFY(RemoveDirectoryW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()))
            != FALSE);
#endif
}

void StorageBrokerTest::rejectsReparseNamespaceFile()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable-handle replacement coverage");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto broker = createBroker(EffectiveStoragePolicy{1024}, root.path());
    QVERIFY(broker != nullptr);
    const QString identity = QStringLiteral("host.app");
    const QString namespaceName = QString::fromLatin1(
                                      QCryptographicHash::hash(identity.toUtf8(),
                                                               QCryptographicHash::Sha256)
                                          .toHex())
        + QStringLiteral(".json");
    const QString outside = QDir(root.path()).filePath(QStringLiteral("outside.json"));
    QFile outsideFile(outside);
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    QCOMPARE(outsideFile.write("{\"secret\":true}"), 15);
    outsideFile.close();
    const QString replacement = QDir(root.path()).filePath(namespaceName);
    if (CreateSymbolicLinkW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(replacement).utf16()),
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(outside).utf16()),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        == FALSE) {
        QSKIP("File symlink creation is unavailable");
    }

    const BrokerResult result = broker->invoke(
        QStringLiteral("get"),
        keyPayload(QStringLiteral("secret")),
        {identity, QStringLiteral("request")});
    QCOMPARE(result.errorCode, QStringLiteral("storage.failed"));
    QVERIFY(DeleteFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(replacement).utf16()))
            != FALSE);
#endif
}

QTEST_GUILESS_MAIN(StorageBrokerTest)
#include "tst_storage_broker.moc"
