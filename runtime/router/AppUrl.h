#pragma once

#include <QMetaType>
#include <QString>

enum class AppUrlError
{
    None,
    InvalidUrl,
    WrongScheme,
    WrongAuthority,
    MalformedPercentEncoding,
    PathNotAbsolute,
    PathTraversal,
    DuplicateSlash,
    FragmentNotAllowed,
    NonNormalizedPath,
};

Q_DECLARE_METATYPE(AppUrlError)

class AppUrl final
{
public:
    [[nodiscard]] static AppUrl parse(const QString &input);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] AppUrlError error() const noexcept;
    [[nodiscard]] QString path() const;
    [[nodiscard]] QString query() const;

private:
    AppUrl(AppUrlError error, QString path = {}, QString query = {});

    AppUrlError m_error = AppUrlError::InvalidUrl;
    QString m_path;
    QString m_query;
};
