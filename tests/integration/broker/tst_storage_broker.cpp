#include "StorageBroker.h"
#include "StorageTestHooks.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <future>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <Aclapi.h>
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
    void rejectsUnexpectedExistingStorageObjects();
    void invalidLayoutDoesNotMutateAcls();
    void rejectsAggregateExistingDataOverQuota();
    void aclMigrationFailureRollsBackEveryObject();
    void aclPostcheckFailureRollsBackCurrentObject();
    void freezesMembershipDuringValidationAndAclMigration();
    void serializesConcurrentQuotaUpdates();
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
    QVERIFY(left.get().ok);
    QVERIFY(right.get().ok);
    QVERIFY(broker->invoke(QStringLiteral("get"), keyPayload(QStringLiteral("left")), context).ok);
    QVERIFY(broker->invoke(QStringLiteral("get"), keyPayload(QStringLiteral("right")), context).ok);
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

QTEST_APPLESS_MAIN(StorageBrokerTest)
#include "tst_storage_broker.moc"
