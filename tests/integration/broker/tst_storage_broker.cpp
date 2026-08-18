#include "StorageBroker.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <future>

#ifdef Q_OS_WIN
#include <qt_windows.h>
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

} // namespace

class StorageBrokerTest final : public QObject
{
    Q_OBJECT

private slots:
    void isolatesDataByHostAssignedIdentity();
    void enforcesQuotaWithoutMutatingExistingValue();
    void rejectsPayloadIdentityAndInvalidKeys();
    void rejectsExistingNamespaceOverQuota();
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

    auto broker = createBroker(EffectiveStoragePolicy{40}, root.path());
    QVERIFY(broker != nullptr);
    const BrokerResult result = broker->invoke(QStringLiteral("get"),
                                              keyPayload(QStringLiteral("key")),
                                              {identity, QStringLiteral("request")});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.quota"));
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
