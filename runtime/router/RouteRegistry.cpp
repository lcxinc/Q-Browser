#include "RouteRegistry.h"

#include "NormalizedPath.h"

#include <QChar>
#include <QSet>

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

RouteAddResult RouteRegistry::add(RouteRecord record)
{
    const QString &pattern = record.pattern;
    if (!pattern.startsWith(u'/') || pattern.contains(u'?') || pattern.contains(u'#')
        || pattern.contains(QStringLiteral("//"))
        || (pattern.size() > 1 && pattern.endsWith(u'/'))) {
        return RouteAddResult::InvalidPattern;
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
                    return RouteAddResult::InvalidPattern;
                }
                parameterNames.insert(name);
                segments.append({true, name});
                normalizedShape += QStringLiteral("/:");
            } else {
                if (!isValidStaticSegment(rawSegment)) {
                    return RouteAddResult::InvalidPattern;
                }
                segments.append({false, rawSegment});
                normalizedShape += u'/' + rawSegment;
            }
        }
    }

    for (const CompiledRoute &existing : m_routes) {
        if (existing.normalizedShape == normalizedShape) {
            return RouteAddResult::DuplicateShape;
        }
    }

    m_routes.append({std::move(record), std::move(segments), std::move(normalizedShape)});
    return RouteAddResult::Added;
}

RouteMatch RouteRegistry::match(const QStringView pathText) const
{
    const NormalizedPath path = NormalizedPath::parse(pathText);
    if (!path.isValid()) {
        return {};
    }
    const QStringList pathSegments = path.decodedSegments();

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
