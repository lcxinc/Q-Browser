#include "BrowserAddress.h"

#include "AppUrl.h"

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QStringConverter>

#include <utility>

namespace {

constexpr qsizetype MaximumAddressBytes = 2048;

[[nodiscard]] int uppercaseHexValue(const QChar character) noexcept
{
    if (character >= u'0' && character <= u'9') {
        return static_cast<int>(character.unicode() - u'0');
    }
    if (character >= u'A' && character <= u'F') {
        return static_cast<int>(character.unicode() - u'A') + 10;
    }
    return -1;
}

[[nodiscard]] bool isUnreservedAscii(const int value) noexcept
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_'
        || value == '~';
}

[[nodiscard]] bool isAllowedLiteralQueryCharacter(const QChar character) noexcept
{
    if (character.unicode() > 0x7f || character == u'%') {
        return false;
    }
    if (isUnreservedAscii(character.unicode())) {
        return true;
    }
    switch (character.unicode()) {
    case u'!':
    case u'$':
    case u'&':
    case u'\'':
    case u'(':
    case u')':
    case u'*':
    case u'+':
    case u',':
    case u';':
    case u'=':
    case u':':
    case u'@':
    case u'/':
    case u'?':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isReservedAscii(const int value) noexcept
{
    switch (value) {
    case ':':
    case '/':
    case '?':
    case '#':
    case '[':
    case ']':
    case '@':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isBidiControl(const char32_t codePoint) noexcept
{
    return codePoint == 0x061c || (codePoint >= 0x200e && codePoint <= 0x200f)
        || (codePoint >= 0x202a && codePoint <= 0x202e)
        || (codePoint >= 0x2066 && codePoint <= 0x2069);
}

[[nodiscard]] bool isSafeDecodedUnicode(const QString &decoded)
{
    for (const QChar character : decoded) {
        if (character.category() == QChar::Other_Control || character.isSpace()) {
            return false;
        }
    }
    for (const char32_t codePoint : decoded.toUcs4()) {
        if (isBidiControl(codePoint)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasCanonicalQuery(const QStringView query)
{
    if (query.isEmpty()) {
        return false;
    }

    for (qsizetype index = 0; index < query.size();) {
        const QChar character = query.at(index);
        if (character != u'%') {
            if (!isAllowedLiteralQueryCharacter(character)) {
                return false;
            }
            ++index;
            continue;
        }

        if (index + 2 >= query.size()) {
            return false;
        }
        const int high = uppercaseHexValue(query.at(index + 1));
        const int low = uppercaseHexValue(query.at(index + 2));
        if (high < 0 || low < 0) {
            return false;
        }

        const int byteValue = (high << 4) | low;
        if (byteValue < 0x80) {
            if (!isReservedAscii(byteValue) && byteValue != '%') {
                return false;
            }
            index += 3;
            continue;
        }

        QByteArray encodedBytes;
        while (index + 2 < query.size() && query.at(index) == u'%') {
            const int sequenceHigh = uppercaseHexValue(query.at(index + 1));
            const int sequenceLow = uppercaseHexValue(query.at(index + 2));
            if (sequenceHigh < 0 || sequenceLow < 0) {
                return false;
            }
            const int sequenceByte = (sequenceHigh << 4) | sequenceLow;
            if (sequenceByte < 0x80) {
                break;
            }
            encodedBytes.append(static_cast<char>(sequenceByte));
            index += 3;
        }

        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(encodedBytes);
        if (decoder.hasError() || !isSafeDecodedUnicode(decoded)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QStringView rawAuthority(const QStringView input)
{
    constexpr QStringView prefix = u"app://";
    if (!input.startsWith(prefix)) {
        return {};
    }

    const qsizetype authorityStart = prefix.size();
    qsizetype authorityEnd = input.size();
    for (qsizetype index = authorityStart; index < input.size(); ++index) {
        const QChar character = input.at(index);
        if (character == u'/' || character == u'?' || character == u'#') {
            authorityEnd = index;
            break;
        }
    }
    return input.sliced(authorityStart, authorityEnd - authorityStart);
}

[[nodiscard]] BrowserAddressError browserError(const AppUrlError error) noexcept
{
    switch (error) {
    case AppUrlError::None:
        return BrowserAddressError::None;
    case AppUrlError::WrongScheme:
        return BrowserAddressError::WrongScheme;
    case AppUrlError::InvalidExpectedAuthority:
    case AppUrlError::WrongAuthority:
        return BrowserAddressError::WrongAuthority;
    case AppUrlError::InvalidUrl:
    case AppUrlError::MalformedPercentEncoding:
    case AppUrlError::PathNotAbsolute:
    case AppUrlError::PathTraversal:
    case AppUrlError::DuplicateSlash:
    case AppUrlError::FragmentNotAllowed:
    case AppUrlError::NonNormalizedPath:
        return BrowserAddressError::AppUrlInvalid;
    }
    return BrowserAddressError::AppUrlInvalid;
}

} // namespace

BrowserAddress BrowserAddress::parse(const QStringView input, const QStringView appAuthority)
{
    if (input.size() > MaximumAddressBytes || input.toUtf8().size() > MaximumAddressBytes) {
        return BrowserAddress(BrowserAddressError::TooLong);
    }

    const qsizetype schemeEnd = input.indexOf(u':');
    if (schemeEnd <= 0) {
        return BrowserAddress(BrowserAddressError::InvalidUrl);
    }
    const QStringView scheme = input.first(schemeEnd);

    if (scheme == u"qbrowser") {
        if (input != u"qbrowser://newtab") {
            return BrowserAddress(BrowserAddressError::UnknownHostPage);
        }
        return BrowserAddress(BrowserAddressError::None,
                              BrowserAddressKind::NewTab,
                              QStringLiteral("qbrowser://newtab"));
    }
    if (scheme != u"app") {
        return BrowserAddress(BrowserAddressError::WrongScheme);
    }

    const AppUrl appUrl = AppUrl::parse(input, appAuthority);
    if (appUrl.error() == AppUrlError::InvalidExpectedAuthority) {
        return BrowserAddress(BrowserAddressError::WrongAuthority);
    }
    if (rawAuthority(input) != appAuthority) {
        return BrowserAddress(BrowserAddressError::WrongAuthority);
    }
    if (!appUrl.isValid()) {
        return BrowserAddress(browserError(appUrl.error()));
    }

    const bool hasQuery = input.indexOf(u'?') >= 0;
    const QString query = appUrl.query();
    if (hasQuery && !hasCanonicalQuery(query)) {
        return BrowserAddress(BrowserAddressError::AppUrlInvalid);
    }

    QString canonical = QStringLiteral("app://");
    canonical.append(appAuthority);
    canonical.append(appUrl.path());
    if (hasQuery) {
        canonical.append(u'?');
        canonical.append(query);
    }
    if (input != canonical) {
        return BrowserAddress(BrowserAddressError::AppUrlInvalid);
    }

    return BrowserAddress(BrowserAddressError::None,
                          BrowserAddressKind::App,
                          std::move(canonical),
                          appUrl.path(),
                          query);
}

bool BrowserAddress::isValid() const noexcept
{
    return m_error == BrowserAddressError::None;
}

BrowserAddressKind BrowserAddress::kind() const noexcept
{
    return m_kind;
}

BrowserAddressError BrowserAddress::error() const noexcept
{
    return m_error;
}

QString BrowserAddress::canonical() const
{
    return m_canonical;
}

QString BrowserAddress::appPath() const
{
    return m_appPath;
}

QString BrowserAddress::appQuery() const
{
    return m_appQuery;
}

BrowserAddress::BrowserAddress(const BrowserAddressError error,
                               const BrowserAddressKind kind,
                               QString canonical,
                               QString appPath,
                               QString appQuery)
    : m_error(error)
    , m_kind(kind)
    , m_canonical(std::move(canonical))
    , m_appPath(std::move(appPath))
    , m_appQuery(std::move(appQuery))
{
}
