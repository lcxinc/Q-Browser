#include "AppUrl.h"

#include <QChar>
#include <QUrl>

#include <utility>

namespace {

[[nodiscard]] bool isHexDigit(const QChar character) noexcept
{
    const char16_t value = character.unicode();
    return (value >= u'0' && value <= u'9') || (value >= u'A' && value <= u'F')
        || (value >= u'a' && value <= u'f');
}

[[nodiscard]] bool hasValidPercentEncoding(const QString &input) noexcept
{
    for (qsizetype index = 0; index < input.size(); ++index) {
        if (input.at(index) != u'%') {
            continue;
        }
        if (index + 2 >= input.size() || !isHexDigit(input.at(index + 1))
            || !isHexDigit(input.at(index + 2))) {
            return false;
        }
        index += 2;
    }
    return true;
}

[[nodiscard]] int hexValue(const QChar character) noexcept
{
    const char16_t value = character.unicode();
    if (value >= u'0' && value <= u'9') {
        return static_cast<int>(value - u'0');
    }
    if (value >= u'A' && value <= u'F') {
        return static_cast<int>(value - u'A') + 10;
    }
    return static_cast<int>(value - u'a') + 10;
}

[[nodiscard]] bool isUnreservedAscii(const int value) noexcept
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_'
        || value == '~';
}

[[nodiscard]] bool hasNonCanonicalEscape(const QString &path) noexcept
{
    for (qsizetype index = 0; index < path.size(); ++index) {
        if (path.at(index) != u'%') {
            continue;
        }

        const QChar high = path.at(index + 1);
        const QChar low = path.at(index + 2);
        if ((high >= u'a' && high <= u'f') || (low >= u'a' && low <= u'f')) {
            return true;
        }

        const int byteValue = (hexValue(high) << 4) | hexValue(low);
        if (isUnreservedAscii(byteValue)) {
            return true;
        }
        index += 2;
    }
    return false;
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

[[nodiscard]] bool hasTraversalSegment(const QString &encodedPath)
{
    const auto segments = encodedPath.split(u'/', Qt::KeepEmptyParts);
    for (const QString &segment : segments) {
        const QString decoded = QUrl::fromPercentEncoding(segment.toLatin1());
        if (decoded == QStringLiteral(".") || decoded == QStringLiteral("..")) {
            return true;
        }
    }
    return false;
}

} // namespace

AppUrl AppUrl::parse(const QString &input)
{
    if (!hasValidPercentEncoding(input)) {
        return AppUrl(AppUrlError::MalformedPercentEncoding);
    }

    const QUrl url(input, QUrl::StrictMode);
    const QString path = rawPath(input);
    if (url.scheme() == QStringLiteral("app") && path.contains(u'\\')) {
        return AppUrl(AppUrlError::NonNormalizedPath);
    }
    if (!url.isValid()) {
        return AppUrl(AppUrlError::InvalidUrl);
    }
    if (url.scheme() != QStringLiteral("app")) {
        return AppUrl(AppUrlError::WrongScheme);
    }
    if (url.host() != QStringLiteral("pilot") || !url.userInfo().isEmpty() || url.port() != -1) {
        return AppUrl(AppUrlError::WrongAuthority);
    }
    if (url.hasFragment()) {
        return AppUrl(AppUrlError::FragmentNotAllowed);
    }

    if (!path.startsWith(u'/')) {
        return AppUrl(AppUrlError::PathNotAbsolute);
    }
    if (hasTraversalSegment(path)) {
        return AppUrl(AppUrlError::PathTraversal);
    }
    if (path.contains(QStringLiteral("//"))) {
        return AppUrl(AppUrlError::DuplicateSlash);
    }
    if (path.contains(u'\\') || hasNonCanonicalEscape(path)
        || url.path(QUrl::FullyEncoded) != path) {
        return AppUrl(AppUrlError::NonNormalizedPath);
    }

    return AppUrl(AppUrlError::None, path, rawQuery(input));
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
