#include "NormalizedPath.h"

#include <QByteArray>
#include <QChar>
#include <QStringConverter>

#include <utility>

namespace {

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
    return character.category() == QChar::Other_Control;
}

[[nodiscard]] bool isAllowedLiteralPathCharacter(const QChar character) noexcept
{
    if (character.unicode() > 0x7f) {
        return false;
    }
    if (isUnreservedAscii(character.unicode()) || character == u':' || character == u'@') {
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
        return true;
    default:
        return false;
    }
}

[[nodiscard]] NormalizedPathError decodeSegment(const QStringView encoded, QString &decoded)
{
    QByteArray bytes;
    bytes.reserve(encoded.size());
    bool hasNonCanonicalEscape = false;

    for (qsizetype index = 0; index < encoded.size(); ++index) {
        const QChar character = encoded.at(index);
        if (character == u'%') {
            if (index + 2 >= encoded.size()) {
                return NormalizedPathError::MalformedPercentEncoding;
            }
            const QChar high = encoded.at(index + 1);
            const QChar low = encoded.at(index + 2);
            const int highValue = hexValue(high);
            const int lowValue = hexValue(low);
            if (highValue < 0 || lowValue < 0) {
                return NormalizedPathError::MalformedPercentEncoding;
            }

            const int byteValue = (highValue << 4) | lowValue;
            hasNonCanonicalEscape = hasNonCanonicalEscape || (high >= u'a' && high <= u'f')
                || (low >= u'a' && low <= u'f') || isUnreservedAscii(byteValue);
            bytes.append(static_cast<char>(byteValue));
            index += 2;
            continue;
        }

        if (character == u'\\') {
            return NormalizedPathError::DecodedSeparator;
        }
        if (isControlCharacter(character)) {
            return NormalizedPathError::ControlCharacter;
        }
        if (!isAllowedLiteralPathCharacter(character)) {
            return NormalizedPathError::NonCanonicalEncoding;
        }
        bytes.append(static_cast<char>(character.unicode()));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    decoded = decoder(bytes);
    if (decoder.hasError()) {
        return NormalizedPathError::InvalidUtf8;
    }
    if (decoded.contains(u'/') || decoded.contains(u'\\')) {
        return NormalizedPathError::DecodedSeparator;
    }
    for (const QChar character : decoded) {
        if (isControlCharacter(character)) {
            return NormalizedPathError::ControlCharacter;
        }
    }
    if (decoded == QStringLiteral(".") || decoded == QStringLiteral("..")) {
        return NormalizedPathError::PathTraversal;
    }
    if (hasNonCanonicalEscape) {
        return NormalizedPathError::NonCanonicalEncoding;
    }
    return NormalizedPathError::None;
}

} // namespace

NormalizedPath NormalizedPath::parse(const QStringView encodedPath)
{
    if (!encodedPath.startsWith(u'/')) {
        return NormalizedPath(NormalizedPathError::NotAbsolute);
    }
    if (encodedPath.contains(QStringLiteral("//"))) {
        return NormalizedPath(NormalizedPathError::DuplicateSlash);
    }
    if (encodedPath.size() > 1 && encodedPath.endsWith(u'/')) {
        return NormalizedPath(NormalizedPathError::TrailingSlash);
    }
    if (encodedPath == QStringLiteral("/")) {
        return NormalizedPath(NormalizedPathError::None, encodedPath.toString());
    }

    const auto rawSegments = encodedPath.sliced(1).split(u'/', Qt::KeepEmptyParts);
    QStringList decodedSegments;
    decodedSegments.reserve(rawSegments.size());
    for (const QStringView rawSegment : rawSegments) {
        QString decoded;
        const NormalizedPathError error = decodeSegment(rawSegment, decoded);
        if (error != NormalizedPathError::None) {
            return NormalizedPath(error);
        }
        decodedSegments.append(std::move(decoded));
    }

    return NormalizedPath(
        NormalizedPathError::None, encodedPath.toString(), std::move(decodedSegments));
}

bool NormalizedPath::isValid() const noexcept
{
    return m_error == NormalizedPathError::None;
}

NormalizedPathError NormalizedPath::error() const noexcept
{
    return m_error;
}

QString NormalizedPath::encoded() const
{
    return m_encodedPath;
}

QStringList NormalizedPath::decodedSegments() const
{
    return m_decodedSegments;
}

NormalizedPath::NormalizedPath(const NormalizedPathError error,
                               QString encodedPath,
                               QStringList decodedSegments)
    : m_error(error)
    , m_encodedPath(std::move(encodedPath))
    , m_decodedSegments(std::move(decodedSegments))
{
}
