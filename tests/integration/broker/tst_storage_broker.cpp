#include "StorageBroker.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

QJsonObject setPayload(const QString &key, const QJsonValue &value)
{
    return {{QStringLiteral("key"), key}, {QStringLiteral("value"), value}};
}

QJsonObject keyPayload(const QString &key)
{
    return {{QStringLiteral("key"), key}};
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
};

void StorageBrokerTest::isolatesDataByHostAssignedIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    StorageBroker broker(EffectiveStoragePolicy{1024}, root.path());

    QVERIFY(broker.invoke(QStringLiteral("set"),
                          setPayload(QStringLiteral("theme"), QStringLiteral("dark")),
                          {QStringLiteral("host.app.one"), false})
                .ok);

    BrokerResult result = broker.invoke(QStringLiteral("get"),
                                        keyPayload(QStringLiteral("theme")),
                                        {QStringLiteral("host.app.two"), false});
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.not_found"));

    result = broker.invoke(QStringLiteral("get"),
                           keyPayload(QStringLiteral("theme")),
                           {QStringLiteral("host.app.one"), false});
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("value")).toString(),
             QStringLiteral("dark"));
    QCOMPARE(QDir(root.path()).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
}

void StorageBrokerTest::enforcesQuotaWithoutMutatingExistingValue()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    StorageBroker broker(EffectiveStoragePolicy{40}, root.path());
    const HostRequestContext context{QStringLiteral("host.app"), false};

    QVERIFY(broker.invoke(QStringLiteral("set"),
                          setPayload(QStringLiteral("key"), QStringLiteral("small")),
                          context)
                .ok);
    const BrokerResult denied = broker.invoke(QStringLiteral("set"),
                                              setPayload(QStringLiteral("key"),
                                                         QString(100, u'x')),
                                              context);
    QVERIFY(!denied.ok);
    QCOMPARE(denied.errorCode, QStringLiteral("storage.quota"));

    const BrokerResult existing = broker.invoke(QStringLiteral("get"),
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
    StorageBroker broker(EffectiveStoragePolicy{1024}, root.path());
    const HostRequestContext context{QStringLiteral("host.app"), false};

    QJsonObject spoofed = setPayload(QStringLiteral("key"), QStringLiteral("value"));
    spoofed.insert(QStringLiteral("appIdentity"), QStringLiteral("victim"));
    BrokerResult result = broker.invoke(QStringLiteral("set"), spoofed, context);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.invalid_request"));

    result = broker.invoke(QStringLiteral("set"),
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

    StorageBroker broker(EffectiveStoragePolicy{40}, root.path());
    const BrokerResult result = broker.invoke(QStringLiteral("get"),
                                              keyPayload(QStringLiteral("key")),
                                              {identity, false});

    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("storage.quota"));
}

QTEST_APPLESS_MAIN(StorageBrokerTest)
#include "tst_storage_broker.moc"
