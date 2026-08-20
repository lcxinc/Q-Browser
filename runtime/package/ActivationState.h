#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

struct ActivationState final
{
    QString current;
    QString previous;
    QString lastKnownGood;
    qint64 generation = 0;

    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] static std::optional<ActivationState> fromJson(
        QByteArrayView bytes);

    friend bool operator==(const ActivationState &, const ActivationState &) = default;
};

struct ActivationBinding final
{
    QString currentDirectory;
    QByteArray versionDigestHex;
    qint64 generation = 0;

    friend bool operator==(const ActivationBinding &,
                           const ActivationBinding &) = default;
};
