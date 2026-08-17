#include "JsonPreflight.h"

#include "ManifestResourceLimits.h"

#include <QSet>
#include <QStringDecoder>

#include <algorithm>

namespace {

constexpr qsizetype MaxJsonNestingDepth = 128;

bool isAsciiIdentifierStart(const QChar character)
{
    return (character >= u'A' && character <= u'Z')
        || (character >= u'a' && character <= u'z') || character == u'_';
}

bool isAsciiIdentifierContinue(const QChar character)
{
    return isAsciiIdentifierStart(character)
        || (character >= u'0' && character <= u'9');
}

bool isHexDigit(const QChar character)
{
    return (character >= u'0' && character <= u'9')
        || (character >= u'a' && character <= u'f')
        || (character >= u'A' && character <= u'F');
}

ushort hexValue(const QChar character)
{
    if (character >= u'0' && character <= u'9') {
        return static_cast<ushort>(character.unicode() - u'0');
    }
    if (character >= u'a' && character <= u'f') {
        return static_cast<ushort>(character.unicode() - u'a' + 10);
    }
    return static_cast<ushort>(character.unicode() - u'A' + 10);
}

bool isJsonWhitespace(const QChar character)
{
    return character == u' ' || character == u'\t' || character == u'\n'
        || character == u'\r';
}

bool shouldRecordNumber(const QString &path)
{
    return path == QStringLiteral("$.schemaVersion")
        || path == QStringLiteral("$.limits.packageBytes")
        || path == QStringLiteral("$.limits.memoryMiB")
        || path == QStringLiteral("$.limits.processes");
}

bool shouldRecordStringLength(const QString &path)
{
    if (path == QStringLiteral("$.version") || path == QStringLiteral("$.entryPoint")
        || path == QStringLiteral("$.runtime.minVersion")
        || path == QStringLiteral("$.runtime.maxVersion")) {
        return true;
    }
    constexpr QStringView routePrefix(u"$.routes[");
    if (!path.startsWith(routePrefix) || !path.endsWith(u']')) {
        return false;
    }
    bool ok = false;
    const qsizetype index = QStringView(path)
                                .sliced(routePrefix.size(), path.size() - routePrefix.size() - 1)
                                .toLongLong(&ok);
    return ok && index < ManifestResourceLimits::MaxRoutes;
}

bool shouldRecordArraySize(const QString &path)
{
    return path == QStringLiteral("$.imports")
        || path == QStringLiteral("$.permissions.network.hosts")
        || path == QStringLiteral("$.permissions.network.methods")
        || path == QStringLiteral("$.routes");
}

qsizetype unicodeScalarCount(const QStringView value)
{
    qsizetype count = value.size();
    for (qsizetype index = 0; index + 1 < value.size(); ++index) {
        if (value.at(index).isHighSurrogate() && value.at(index + 1).isLowSurrogate()) {
            --count;
            ++index;
        }
    }
    return count;
}

class StrictJsonScanner final
{
public:
    explicit StrictJsonScanner(QString text)
        : m_text(std::move(text))
    {
        if (!m_text.isEmpty() && m_text.front() == QChar::ByteOrderMark) {
            ++m_position;
        }
    }

    JsonPreflightResult scan()
    {
        skipWhitespace();
        if (!parseValue(QStringLiteral("$"), 0)) {
            setSyntaxError();
            return std::move(m_result);
        }
        skipWhitespace();
        if (m_position != m_text.size()) {
            setSyntaxError();
        }
        return std::move(m_result);
    }

private:
    bool parseValue(const QString &path, const qsizetype depth)
    {
        if (depth > MaxJsonNestingDepth || m_position >= m_text.size()) {
            return false;
        }
        const QChar current = m_text.at(m_position);
        if (current == u'{') {
            return parseObject(path, depth);
        }
        if (current == u'[') {
            return parseArray(path, depth);
        }
        if (current == u'"') {
            QString value;
            if (!parseString(value)) {
                return false;
            }
            if (shouldRecordStringLength(path)) {
                m_result.stringLengths.insert(path, unicodeScalarCount(value));
            }
            return true;
        }
        if (current == u'-' || (current >= u'0' && current <= u'9')) {
            return parseNumber(path);
        }
        return consumeKeyword(QStringView(u"true")) || consumeKeyword(QStringView(u"false"))
            || consumeKeyword(QStringView(u"null"));
    }

    bool parseObject(const QString &path, const qsizetype depth)
    {
        ++m_position;
        skipWhitespace();
        if (consume(u'}')) {
            return true;
        }

        QSet<QString> members;
        while (m_position < m_text.size()) {
            QString member;
            if (!parseString(member)) {
                return false;
            }
            const QString memberPath = manifestJsonPathMember(path, member);
            if (members.contains(member)) {
                m_result.error = ManifestError{ManifestErrorCode::InvalidJson,
                                               memberPath,
                                               QStringLiteral("duplicate JSON member")};
                return false;
            }
            members.insert(member);
            skipWhitespace();
            if (!consume(u':')) {
                return false;
            }
            skipWhitespace();
            if (!parseValue(memberPath, depth + 1)) {
                return false;
            }
            skipWhitespace();
            if (consume(u'}')) {
                return true;
            }
            if (!consume(u',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(const QString &path, const qsizetype depth)
    {
        ++m_position;
        skipWhitespace();
        qsizetype count = 0;
        if (consume(u']')) {
            if (shouldRecordArraySize(path)) {
                m_result.arraySizes.insert(path, count);
            }
            return true;
        }

        while (m_position < m_text.size()) {
            const QString itemPath = path + QStringLiteral("[%1]").arg(count);
            if (!parseValue(itemPath, depth + 1)) {
                return false;
            }
            ++count;
            skipWhitespace();
            if (consume(u']')) {
                if (shouldRecordArraySize(path)) {
                    m_result.arraySizes.insert(path, count);
                }
                return true;
            }
            if (!consume(u',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseString(QString &value)
    {
        if (!consume(u'"')) {
            return false;
        }
        while (m_position < m_text.size()) {
            const QChar character = m_text.at(m_position++);
            if (character == u'"') {
                return true;
            }
            if (character.unicode() < 0x20) {
                return false;
            }
            if (character != u'\\') {
                if (character.isHighSurrogate()) {
                    if (m_position >= m_text.size()
                        || !m_text.at(m_position).isLowSurrogate()) {
                        return false;
                    }
                    value.append(character);
                    value.append(m_text.at(m_position++));
                } else if (character.isLowSurrogate()) {
                    return false;
                } else {
                    value.append(character);
                }
                continue;
            }
            if (m_position >= m_text.size()) {
                return false;
            }
            const QChar escape = m_text.at(m_position++);
            switch (escape.unicode()) {
            case '"':
            case '\\':
            case '/':
                value.append(escape);
                break;
            case 'b':
                value.append(u'\b');
                break;
            case 'f':
                value.append(u'\f');
                break;
            case 'n':
                value.append(u'\n');
                break;
            case 'r':
                value.append(u'\r');
                break;
            case 't':
                value.append(u'\t');
                break;
            case 'u': {
                ushort codeUnit = 0;
                if (!parseHexCodeUnit(codeUnit)) {
                    return false;
                }
                const QChar decoded(codeUnit);
                if (decoded.isHighSurrogate()) {
                    if (m_position + 2 > m_text.size() || m_text.at(m_position) != u'\\'
                        || m_text.at(m_position + 1) != u'u') {
                        return false;
                    }
                    m_position += 2;
                    ushort lowCodeUnit = 0;
                    if (!parseHexCodeUnit(lowCodeUnit) || !QChar(lowCodeUnit).isLowSurrogate()) {
                        return false;
                    }
                    value.append(decoded);
                    value.append(QChar(lowCodeUnit));
                } else if (decoded.isLowSurrogate()) {
                    return false;
                } else {
                    value.append(decoded);
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parseHexCodeUnit(ushort &value)
    {
        if (m_position + 4 > m_text.size()) {
            return false;
        }
        ushort decoded = 0;
        for (qsizetype index = 0; index < 4; ++index) {
            const QChar character = m_text.at(m_position + index);
            if (!isHexDigit(character)) {
                return false;
            }
            decoded = static_cast<ushort>((decoded << 4) | hexValue(character));
        }
        m_position += 4;
        value = decoded;
        return true;
    }

    bool parseNumber(const QString &path)
    {
        const qsizetype start = m_position;
        consume(u'-');
        if (m_position >= m_text.size()) {
            return false;
        }
        if (m_text.at(m_position) == u'0') {
            ++m_position;
            if (m_position < m_text.size() && m_text.at(m_position).isDigit()) {
                return false;
            }
        } else if (m_text.at(m_position) >= u'1' && m_text.at(m_position) <= u'9') {
            do {
                ++m_position;
            } while (m_position < m_text.size() && m_text.at(m_position) >= u'0'
                     && m_text.at(m_position) <= u'9');
        } else {
            return false;
        }
        if (consume(u'.')) {
            const qsizetype fractionStart = m_position;
            while (m_position < m_text.size() && m_text.at(m_position) >= u'0'
                   && m_text.at(m_position) <= u'9') {
                ++m_position;
            }
            if (m_position == fractionStart) {
                return false;
            }
        }
        if (m_position < m_text.size()
            && (m_text.at(m_position) == u'e' || m_text.at(m_position) == u'E')) {
            ++m_position;
            if (m_position < m_text.size()
                && (m_text.at(m_position) == u'+' || m_text.at(m_position) == u'-')) {
                ++m_position;
            }
            const qsizetype exponentStart = m_position;
            while (m_position < m_text.size() && m_text.at(m_position) >= u'0'
                   && m_text.at(m_position) <= u'9') {
                ++m_position;
            }
            if (m_position == exponentStart) {
                return false;
            }
        }
        if (shouldRecordNumber(path)) {
            m_result.numberLexemes.insert(path, m_text.mid(start, m_position - start));
        }
        return true;
    }

    bool consumeKeyword(const QStringView keyword)
    {
        if (QStringView(m_text).mid(m_position, keyword.size()) != keyword) {
            return false;
        }
        m_position += keyword.size();
        return true;
    }

    bool consume(const QChar expected)
    {
        if (m_position >= m_text.size() || m_text.at(m_position) != expected) {
            return false;
        }
        ++m_position;
        return true;
    }

    void skipWhitespace()
    {
        while (m_position < m_text.size() && isJsonWhitespace(m_text.at(m_position))) {
            ++m_position;
        }
    }

    void setSyntaxError()
    {
        if (!m_result.error.has_value()) {
            m_result.error = ManifestError{ManifestErrorCode::InvalidJson,
                                           QStringLiteral("$"),
                                           QStringLiteral("invalid JSON")};
        }
    }

    QString m_text;
    qsizetype m_position = 0;
    JsonPreflightResult m_result;
};

} // namespace

JsonPreflightResult preflightManifestJson(const QByteArrayView bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        JsonPreflightResult result;
        result.error = ManifestError{ManifestErrorCode::InvalidJson,
                                     QStringLiteral("$"),
                                     QStringLiteral("invalid JSON")};
        return result;
    }
    return StrictJsonScanner(std::move(text)).scan();
}

QString manifestJsonPathMember(const QString &base, const QStringView member)
{
    const bool isIdentifier = !member.isEmpty() && isAsciiIdentifierStart(member.front())
        && std::ranges::all_of(member.mid(1), isAsciiIdentifierContinue);
    if (isIdentifier) {
        return base + u'.' + member;
    }

    QString escaped;
    escaped.reserve(member.size());
    for (const QChar character : member) {
        switch (character.unicode()) {
        case '"':
            escaped.append(QStringLiteral("\\\""));
            break;
        case '\\':
            escaped.append(QStringLiteral("\\\\"));
            break;
        case '\b':
            escaped.append(QStringLiteral("\\b"));
            break;
        case '\f':
            escaped.append(QStringLiteral("\\f"));
            break;
        case '\n':
            escaped.append(QStringLiteral("\\n"));
            break;
        case '\r':
            escaped.append(QStringLiteral("\\r"));
            break;
        case '\t':
            escaped.append(QStringLiteral("\\t"));
            break;
        default:
            if (character.unicode() < 0x20
                || (character.unicode() >= 0x7f && character.unicode() <= 0x9f)
                || character == QChar(0x2028) || character == QChar(0x2029)) {
                escaped.append(QStringLiteral("\\u%1").arg(
                    static_cast<uint>(character.unicode()), 4, 16, QLatin1Char('0')));
            } else {
                escaped.append(character);
            }
            break;
        }
    }
    return base + QStringLiteral("[\"") + escaped + QStringLiteral("\"]");
}

bool manifestExactPositiveJsonInteger(const QStringView lexeme,
                                      const qint64 maximum,
                                      qint64 *const value)
{
    if (lexeme.isEmpty() || lexeme.front() == u'-' || maximum <= 0) {
        return false;
    }

    qsizetype position = 0;
    QString digits;
    digits.reserve(lexeme.size());
    while (position < lexeme.size() && lexeme.at(position) >= u'0'
           && lexeme.at(position) <= u'9') {
        digits.append(lexeme.at(position++));
    }

    qsizetype fractionDigits = 0;
    if (position < lexeme.size() && lexeme.at(position) == u'.') {
        ++position;
        const qsizetype fractionStart = position;
        while (position < lexeme.size() && lexeme.at(position) >= u'0'
               && lexeme.at(position) <= u'9') {
            digits.append(lexeme.at(position++));
        }
        fractionDigits = position - fractionStart;
    }

    qint64 exponent = 0;
    if (position < lexeme.size()
        && (lexeme.at(position) == u'e' || lexeme.at(position) == u'E')) {
        ++position;
        bool negativeExponent = false;
        if (position < lexeme.size()
            && (lexeme.at(position) == u'+' || lexeme.at(position) == u'-')) {
            negativeExponent = lexeme.at(position) == u'-';
            ++position;
        }
        constexpr qint64 ExponentSaturation = 2'000'000;
        while (position < lexeme.size()) {
            const int digit = lexeme.at(position++).unicode() - u'0';
            exponent = std::min(ExponentSaturation, exponent * 10 + digit);
        }
        if (negativeExponent) {
            exponent = -exponent;
        }
    }

    const qsizetype firstNonZero = std::ranges::find_if(digits, [](const QChar digit) {
        return digit != u'0';
    }) - digits.cbegin();
    if (firstNonZero == digits.size()) {
        return false;
    }

    const qint64 decimalShift = exponent - fractionDigits;
    qsizetype retainedEnd = digits.size();
    if (decimalShift < 0) {
        const qint64 removed = -decimalShift;
        if (removed > retainedEnd) {
            return false;
        }
        const qsizetype removedStart = retainedEnd - static_cast<qsizetype>(removed);
        if (!std::ranges::all_of(QStringView(digits).mid(removedStart),
                                 [](const QChar digit) { return digit == u'0'; })) {
            return false;
        }
        retainedEnd = removedStart;
    }

    const qint64 appendedZeros = std::max<qint64>(decimalShift, 0);
    const qint64 integerDigits = retainedEnd - firstNonZero + appendedZeros;
    const QString maximumText = QString::number(maximum);
    if (integerDigits > maximumText.size()) {
        return false;
    }

    QString normalized = digits.mid(firstNonZero, retainedEnd - firstNonZero);
    normalized.append(QString(static_cast<qsizetype>(appendedZeros), u'0'));
    if (normalized.size() == maximumText.size() && normalized > maximumText) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = normalized.toLongLong(&ok);
    if (!ok || parsed <= 0 || parsed > maximum) {
        return false;
    }
    if (value != nullptr) {
        *value = parsed;
    }
    return true;
}
