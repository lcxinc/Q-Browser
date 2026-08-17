#pragma once

#include "ManifestError.h"

#include <QByteArrayView>
#include <QHash>
#include <QString>

#include <optional>

struct JsonPreflightResult
{
    std::optional<ManifestError> error;
    QHash<QString, QString> numberLexemes;
    QHash<QString, qsizetype> stringLengths;
    QHash<QString, qsizetype> arraySizes;
};

[[nodiscard]] JsonPreflightResult preflightManifestJson(QByteArrayView bytes);
[[nodiscard]] QString manifestJsonPathMember(const QString &base, QStringView member);
[[nodiscard]] bool manifestExactPositiveJsonInteger(QStringView lexeme,
                                                    qint64 maximum,
                                                    qint64 *value = nullptr);
