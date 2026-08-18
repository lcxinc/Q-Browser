#include "StorageBroker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <utility>

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

} // namespace

StorageBroker::StorageBroker(EffectiveStoragePolicy policy, QString rootDirectory)
    : policy_(policy), rootDirectory_(std::move(rootDirectory))
{
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
    if (!QDir().mkpath(rootDirectory_)) {
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }

    const QString path = storagePath(rootDirectory_, context.appIdentity);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(1000)) {
        return BrokerResult::failure(QStringLiteral("storage.busy"),
                                     QStringLiteral("Storage is busy."));
    }

    QJsonObject values;
    const BrokerResult loaded = loadValues(path, policy_.quotaBytes, values);
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
        || !output.commit()) {
        output.cancelWriting();
        return BrokerResult::failure(QStringLiteral("storage.failed"),
                                     QStringLiteral("Storage is unavailable."));
    }
    return BrokerResult::success();
}
