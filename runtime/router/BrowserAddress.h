#pragma once

#include <QMetaType>
#include <QString>
#include <QStringView>

enum class BrowserAddressKind
{
    Invalid,
    NewTab,
    App,
};

Q_DECLARE_METATYPE(BrowserAddressKind)

enum class BrowserAddressError
{
    None,
    InvalidUrl,
    WrongScheme,
    WrongAuthority,
    UnknownHostPage,
    AppUrlInvalid,
    TooLong,
};

Q_DECLARE_METATYPE(BrowserAddressError)

class BrowserAddress final
{
public:
    [[nodiscard]] static BrowserAddress parse(QStringView input,
                                              QStringView appAuthority = u"pilot");

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] BrowserAddressKind kind() const noexcept;
    [[nodiscard]] BrowserAddressError error() const noexcept;
    [[nodiscard]] QString canonical() const;
    [[nodiscard]] QString appPath() const;
    [[nodiscard]] QString appQuery() const;

private:
    BrowserAddress(BrowserAddressError error,
                   BrowserAddressKind kind = BrowserAddressKind::Invalid,
                   QString canonical = {},
                   QString appPath = {},
                   QString appQuery = {});

    BrowserAddressError m_error = BrowserAddressError::InvalidUrl;
    BrowserAddressKind m_kind = BrowserAddressKind::Invalid;
    QString m_canonical;
    QString m_appPath;
    QString m_appQuery;
};
