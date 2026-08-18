#include "ActivationState.h"
#include "JsonPreflight.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace
{
constexpr qsizetype MaximumActivationStateBytes = 4096;

bool containsExactlyStateMembers(const QJsonObject &object)
{
    if (object.size() != 3) {
        return false;
    }
    static const QStringList members{
        QStringLiteral("current"),
        QStringLiteral("previous"),
        QStringLiteral("lastKnownGood")};
    for (const QString &member : members) {
        if (!object.value(member).isString()) {
            return false;
        }
    }
    return true;
}

QString persistedTarget(const QString &target)
{
    return target.isEmpty() ? QString{} : QStringLiteral("versions/") + target;
}

std::optional<QString> inMemoryTarget(const QString &target)
{
    if (target.isEmpty()) {
        return QString{};
    }
    constexpr QStringView Prefix(u"versions/");
    if (!target.startsWith(Prefix) || target.size() == Prefix.size()) {
        return std::nullopt;
    }
    return target.sliced(Prefix.size());
}
}

QByteArray ActivationState::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("current"), persistedTarget(current));
    object.insert(QStringLiteral("previous"), persistedTarget(previous));
    object.insert(QStringLiteral("lastKnownGood"), persistedTarget(lastKnownGood));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<ActivationState> ActivationState::fromJson(
    const QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.size() > MaximumActivationStateBytes) {
        return std::nullopt;
    }
    if (preflightManifestJson(bytes).error.has_value()) {
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !containsExactlyStateMembers(document.object())) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const std::optional<QString> current = inMemoryTarget(
        object.value(QStringLiteral("current")).toString());
    const std::optional<QString> previous = inMemoryTarget(
        object.value(QStringLiteral("previous")).toString());
    const std::optional<QString> lastKnownGood = inMemoryTarget(
        object.value(QStringLiteral("lastKnownGood")).toString());
    if (!current.has_value() || !previous.has_value()
        || !lastKnownGood.has_value()) {
        return std::nullopt;
    }
    return ActivationState{*current, *previous, *lastKnownGood};
}
