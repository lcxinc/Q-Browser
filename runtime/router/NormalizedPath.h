#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>

enum class NormalizedPathError
{
    None,
    NotAbsolute,
    MalformedPercentEncoding,
    NonCanonicalEncoding,
    InvalidUtf8,
    ControlCharacter,
    PathTraversal,
    DecodedSeparator,
    DuplicateSlash,
    TrailingSlash,
};

Q_DECLARE_METATYPE(NormalizedPathError)

class NormalizedPath final
{
public:
    [[nodiscard]] static NormalizedPath parse(QStringView encodedPath);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] NormalizedPathError error() const noexcept;
    [[nodiscard]] QString encoded() const;
    [[nodiscard]] QStringList decodedSegments() const;

private:
    NormalizedPath(NormalizedPathError error,
                   QString encodedPath = {},
                   QStringList decodedSegments = {});

    NormalizedPathError m_error = NormalizedPathError::NotAbsolute;
    QString m_encodedPath;
    QStringList m_decodedSegments;
};
