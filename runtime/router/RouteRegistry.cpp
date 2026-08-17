#include "RouteRegistry.h"

#include <QByteArray>
#include <QChar>
#include <QSet>
#include <QStringConverter>

#include <utility>

namespace {

[[nodiscard]] bool isAsciiLetter(const QChar character) noexcept
{
    return (character >= u'A' && character <= u'Z') || (character >= u'a' && character <= u'z');
}

[[nodiscard]] bool isAsciiDigit(const QChar character) noexcept
{
    return character >= u'0' && character <= u'9';
}

[[nodiscard]] bool isValidParameterName(const QString &name) noexcept
{
    if (name.isEmpty() || (!isAsciiLetter(name.front()) && name.front() != u'_')) {
        return false;
    }
    for (const QChar character : name) {
        if (!isAsciiLetter(character) && !isAsciiDigit(character) && character != u'_') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidStaticSegment(const QString &segment) noexcept
{
    if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
        return false;
    }
    for (const QChar character : segment) {
        if (!isAsciiLetter(character) && !isAsciiDigit(character) && character != u'-'
            && character != u'_' && character != u'.' && character != u'~') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int hexValue(const QChar character) noexcept
{
    if (character >= u'0' && character <= u'9') {
        return static_cast<int>(character.unicode() - u'0');
    }
    if (character >= u'A' && character <= u'F') {
        return static_cast<int>(character.unicode() - u'A') + 10;
    }
    if (character >= u'a' && character <= u'f') {
        return static_cast<int>(character.unicode() - u'a') + 10;
    }
    return -1;
}

[[nodiscard]] bool isUnreservedAscii(const int value) noexcept
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_'
        || value == '~';
}

[[nodiscard]] bool isControlCharacter(const QChar character) noexcept
{
    return character.unicode() <= 0x1f || character.unicode() == 0x7f;
}

[[nodiscard]] bool decodeSegment(const QString &encoded, QString &decoded)
{
    QByteArray bytes;
    bytes.reserve(encoded.size());

    for (qsizetype index = 0; index < encoded.size(); ++index) {
        const QChar character = encoded.at(index);
        if (character == u'%') {
            if (index + 2 >= encoded.size()) {
                return false;
            }
            const QChar high = encoded.at(index + 1);
            const QChar low = encoded.at(index + 2);
            const int highValue = hexValue(high);
            const int lowValue = hexValue(low);
            if (highValue < 0 || lowValue < 0 || (high >= u'a' && high <= u'f')
                || (low >= u'a' && low <= u'f')) {
                return false;
            }
            const int byteValue = (highValue << 4) | lowValue;
            if (isUnreservedAscii(byteValue)) {
                return false;
            }
            bytes.append(static_cast<char>(byteValue));
            index += 2;
            continue;
        }

        if (character.unicode() > 0x7f || isControlCharacter(character) || character == u'\\'
            || character == u' ') {
            return false;
        }
        bytes.append(static_cast<char>(character.unicode()));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    decoded = decoder(bytes);
    if (decoder.hasError() || decoded.contains(u'/') || decoded.contains(u'\\')
        || decoded == QStringLiteral(".") || decoded == QStringLiteral("..")) {
        return false;
    }
    for (const QChar character : decoded) {
        if (isControlCharacter(character)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parsePath(const QString &path, QStringList &segments)
{
    if (!path.startsWith(u'/') || path.contains(u'?') || path.contains(u'#')
        || path.contains(QStringLiteral("//")) || (path.size() > 1 && path.endsWith(u'/'))) {
        return false;
    }
    if (path == QStringLiteral("/")) {
        return true;
    }

    const auto encodedSegments = path.sliced(1).split(u'/', Qt::KeepEmptyParts);
    segments.reserve(encodedSegments.size());
    for (const QString &encoded : encodedSegments) {
        QString decoded;
        if (!decodeSegment(encoded, decoded)) {
            return false;
        }
        segments.append(std::move(decoded));
    }
    return true;
}

template<typename Segment>
[[nodiscard]] bool hasHigherPrecedence(const QVector<Segment> &candidate,
                                       const QVector<Segment> &current) noexcept
{
    for (qsizetype index = 0; index < candidate.size(); ++index) {
        if (candidate.at(index).parameter != current.at(index).parameter) {
            return !candidate.at(index).parameter;
        }
    }
    return false;
}

} // namespace

bool RouteMatch::isValid() const noexcept
{
    return m_valid;
}

RouteMatch::RouteMatch(RouteRecord routeRecord, QHash<QString, QString> routeParameters)
    : record(std::move(routeRecord))
    , parameters(std::move(routeParameters))
    , m_valid(true)
{
}

bool RouteRegistry::add(RouteRecord record)
{
    const QString &pattern = record.pattern;
    if (!pattern.startsWith(u'/') || pattern.contains(u'?') || pattern.contains(u'#')
        || pattern.contains(QStringLiteral("//"))
        || (pattern.size() > 1 && pattern.endsWith(u'/'))) {
        return false;
    }

    QVector<Segment> segments;
    QString normalizedShape;
    if (pattern == QStringLiteral("/")) {
        normalizedShape = pattern;
    } else {
        const auto rawSegments = pattern.sliced(1).split(u'/', Qt::KeepEmptyParts);
        segments.reserve(rawSegments.size());
        QSet<QString> parameterNames;
        for (const QString &rawSegment : rawSegments) {
            if (rawSegment.startsWith(u':')) {
                const QString name = rawSegment.sliced(1);
                if (!isValidParameterName(name) || parameterNames.contains(name)) {
                    return false;
                }
                parameterNames.insert(name);
                segments.append({true, name});
                normalizedShape += QStringLiteral("/:");
            } else {
                if (!isValidStaticSegment(rawSegment)) {
                    return false;
                }
                segments.append({false, rawSegment});
                normalizedShape += u'/' + rawSegment;
            }
        }
    }

    for (const CompiledRoute &existing : m_routes) {
        if (existing.normalizedShape == normalizedShape) {
            return false;
        }
    }

    m_routes.append({std::move(record), std::move(segments), std::move(normalizedShape)});
    return true;
}

RouteMatch RouteRegistry::match(const QString &path) const
{
    QStringList pathSegments;
    if (!parsePath(path, pathSegments)) {
        return {};
    }

    const CompiledRoute *best = nullptr;
    QHash<QString, QString> bestParameters;
    for (const CompiledRoute &candidate : m_routes) {
        if (candidate.segments.size() != pathSegments.size()) {
            continue;
        }

        bool matches = true;
        QHash<QString, QString> parameters;
        for (qsizetype index = 0; index < candidate.segments.size(); ++index) {
            const Segment &segment = candidate.segments.at(index);
            const QString &value = pathSegments.at(index);
            if (segment.parameter) {
                parameters.insert(segment.value, value);
            } else if (segment.value != value) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        if (best == nullptr || hasHigherPrecedence(candidate.segments, best->segments)) {
            best = &candidate;
            bestParameters = std::move(parameters);
        }
    }

    if (best == nullptr) {
        return {};
    }
    return RouteMatch(best->record, std::move(bestParameters));
}
