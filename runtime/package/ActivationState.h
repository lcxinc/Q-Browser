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

    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] static std::optional<ActivationState> fromJson(
        QByteArrayView bytes);

    friend bool operator==(const ActivationState &, const ActivationState &) = default;
};
