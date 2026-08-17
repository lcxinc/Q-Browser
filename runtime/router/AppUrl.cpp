#include "AppUrl.h"

#include "NormalizedPath.h"

#include <QChar>
#include <QUrl>

#include <utility>

namespace {

[[nodiscard]] bool isValidExpectedAuthority(const QStringView authority)
{
    if (authority.isEmpty() || authority.size() > 253 || authority.startsWith(u'.')
        || authority.endsWith(u'.')) {
        return false;
    }

    const auto labels = authority.toString().split(u'.', Qt::KeepEmptyParts);
    for (const QString &label : labels) {
        if (label.isEmpty() || label.size() > 63 || label.startsWith(u'-') || label.endsWith(u'-')) {
            return false;
        }
        for (const QChar character : label) {
            const bool lowerLetter = character >= u'a' && character <= u'z';
            const bool digit = character >= u'0' && character <= u'9';
            if (!lowerLetter && !digit && character != u'-') {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool isHexDigit(const QChar character) noexcept
{
    return (character >= u'0' && character <= u'9') || (character >= u'A' && character <= u'F')
        || (character >= u'a' && character <= u'f');
}

[[nodiscard]] bool hasValidPercentEncoding(const QStringView encoded) noexcept
{
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        if (encoded.at(index) != u'%') {
            continue;
        }
        if (index + 2 >= encoded.size() || !isHexDigit(encoded.at(index + 1))
            || !isHexDigit(encoded.at(index + 2))) {
            return false;
        }
        index += 2;
    }
    return true;
}

[[nodiscard]] QString rawPath(const QString &input)
{
    const qsizetype authorityMarker = input.indexOf(QStringLiteral("://"));
    if (authorityMarker < 0) {
        return {};
    }

    const qsizetype authorityStart = authorityMarker + 3;
    qsizetype end = input.size();
    const qsizetype queryStart = input.indexOf(u'?', authorityStart);
    const qsizetype fragmentStart = input.indexOf(u'#', authorityStart);
    if (queryStart >= 0) {
        end = queryStart;
    }
    if (fragmentStart >= 0 && fragmentStart < end) {
        end = fragmentStart;
    }

    const qsizetype pathStart = input.indexOf(u'/', authorityStart);
    if (pathStart < 0 || pathStart >= end) {
        return {};
    }
    return input.mid(pathStart, end - pathStart);
}

[[nodiscard]] QString rawQuery(const QString &input)
{
    const qsizetype queryStart = input.indexOf(u'?');
    if (queryStart < 0) {
        return {};
    }
    const qsizetype fragmentStart = input.indexOf(u'#', queryStart + 1);
    const qsizetype end = fragmentStart < 0 ? input.size() : fragmentStart;
    return input.mid(queryStart + 1, end - queryStart - 1);
}

[[nodiscard]] AppUrlError appUrlError(const NormalizedPathError error) noexcept
{
    switch (error) {
    case NormalizedPathError::None:
        return AppUrlError::None;
    case NormalizedPathError::NotAbsolute:
        return AppUrlError::PathNotAbsolute;
    case NormalizedPathError::MalformedPercentEncoding:
        return AppUrlError::MalformedPercentEncoding;
    case NormalizedPathError::PathTraversal:
        return AppUrlError::PathTraversal;
    case NormalizedPathError::DuplicateSlash:
        return AppUrlError::DuplicateSlash;
    case NormalizedPathError::NonCanonicalEncoding:
    case NormalizedPathError::InvalidUtf8:
    case NormalizedPathError::ControlCharacter:
    case NormalizedPathError::DecodedSeparator:
    case NormalizedPathError::TrailingSlash:
        return AppUrlError::NonNormalizedPath;
    }
    return AppUrlError::NonNormalizedPath;
}

} // namespace

AppUrl AppUrl::parse(const QStringView inputView, const QStringView expectedAuthority)
{
    if (!isValidExpectedAuthority(expectedAuthority)) {
        return AppUrl(AppUrlError::InvalidExpectedAuthority);
    }

    const QString input = inputView.toString();
    const QUrl url(input, QUrl::StrictMode);
    if (url.scheme() != QStringLiteral("app")) {
        return AppUrl(AppUrlError::WrongScheme);
    }
    if (url.isValid()
        && (url.host() != expectedAuthority || !url.userInfo().isEmpty() || url.port() != -1)) {
        return AppUrl(AppUrlError::WrongAuthority);
    }
    if (url.isValid() && url.hasFragment()) {
        return AppUrl(AppUrlError::FragmentNotAllowed);
    }

    const NormalizedPath path = NormalizedPath::parse(rawPath(input));
    if (!path.isValid()) {
        return AppUrl(appUrlError(path.error()));
    }

    const QString query = rawQuery(input);
    if (!hasValidPercentEncoding(query)) {
        return AppUrl(AppUrlError::MalformedPercentEncoding);
    }
    if (!url.isValid()) {
        return AppUrl(AppUrlError::InvalidUrl);
    }

    return AppUrl(AppUrlError::None, path.encoded(), query);
}

bool AppUrl::isValid() const noexcept
{
    return m_error == AppUrlError::None;
}

AppUrlError AppUrl::error() const noexcept
{
    return m_error;
}

QString AppUrl::path() const
{
    return m_path;
}

QString AppUrl::query() const
{
    return m_query;
}

AppUrl::AppUrl(const AppUrlError error, QString path, QString query)
    : m_error(error)
    , m_path(std::move(path))
    , m_query(std::move(query))
{
}
