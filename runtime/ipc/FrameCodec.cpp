#include "FrameCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>

namespace {

quint32 decodeLength(const QByteArray &bytes)
{
    return (static_cast<quint32>(static_cast<quint8>(bytes[0])) << 24U)
           | (static_cast<quint32>(static_cast<quint8>(bytes[1])) << 16U)
           | (static_cast<quint32>(static_cast<quint8>(bytes[2])) << 8U)
           | static_cast<quint32>(static_cast<quint8>(bytes[3]));
}

enum class StrictScanResult {
    Valid,
    DuplicateMember,
    ResourceLimit,
    Invalid,
};

class StrictJsonScanner final
{
public:
    explicit StrictJsonScanner(QString text) : text_(std::move(text)) {}

    StrictScanResult scan()
    {
        skipWhitespace();
        if (!parseValue(0)) {
            return result_ == StrictScanResult::Valid ? StrictScanResult::Invalid : result_;
        }
        skipWhitespace();
        return position_ == text_.size() ? result_ : StrictScanResult::Invalid;
    }

private:
    bool parseValue(const qsizetype nesting)
    {
        if (position_ >= text_.size()) {
            return false;
        }
        const QChar value = text_.at(position_);
        if (value == u'{') {
            if (nesting >= FrameCodec::maximumJsonNesting()) {
                result_ = StrictScanResult::ResourceLimit;
                return false;
            }
            return parseObject(nesting + 1);
        }
        if (value == u'[') {
            if (nesting >= FrameCodec::maximumJsonNesting()) {
                result_ = StrictScanResult::ResourceLimit;
                return false;
            }
            return parseArray(nesting + 1);
        }
        if (value == u'"') {
            QString ignored;
            return parseString(ignored);
        }
        if (value == u'-' || (value >= u'0' && value <= u'9')) {
            return parseNumber();
        }
        return consumeKeyword(QStringView(u"true"))
            || consumeKeyword(QStringView(u"false"))
            || consumeKeyword(QStringView(u"null"));
    }

    bool parseObject(const qsizetype nesting)
    {
        ++position_;
        skipWhitespace();
        if (consume(u'}')) {
            return true;
        }
        QSet<QString> members;
        for (;;) {
            QString member;
            if (!parseString(member)) {
                return false;
            }
            if (members.contains(member)) {
                result_ = StrictScanResult::DuplicateMember;
                return false;
            }
            members.insert(member);
            if (!countAggregateEntry()) {
                return false;
            }
            skipWhitespace();
            if (!consume(u':')) {
                return false;
            }
            skipWhitespace();
            if (!parseValue(nesting)) {
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
    }

    bool parseArray(const qsizetype nesting)
    {
        ++position_;
        skipWhitespace();
        if (consume(u']')) {
            return true;
        }
        for (;;) {
            if (!countAggregateEntry()) {
                return false;
            }
            if (!parseValue(nesting)) {
                return false;
            }
            skipWhitespace();
            if (consume(u']')) {
                return true;
            }
            if (!consume(u',')) {
                return false;
            }
            skipWhitespace();
        }
    }

    bool parseString(QString &decoded)
    {
        if (!consume(u'"')) {
            return false;
        }
        while (position_ < text_.size()) {
            const QChar character = text_.at(position_++);
            if (character == u'"') {
                return true;
            }
            if (character.unicode() < 0x20) {
                return false;
            }
            if (character != u'\\') {
                if (character.isHighSurrogate()) {
                    if (position_ >= text_.size()
                        || !text_.at(position_).isLowSurrogate()) {
                        return false;
                    }
                    decoded.append(character);
                    decoded.append(text_.at(position_++));
                } else if (character.isLowSurrogate()) {
                    return false;
                } else {
                    decoded.append(character);
                }
                continue;
            }
            if (position_ >= text_.size()) {
                return false;
            }
            const QChar escape = text_.at(position_++);
            switch (escape.unicode()) {
            case '"':
            case '\\':
            case '/':
                decoded.append(escape);
                break;
            case 'b':
                decoded.append(u'\b');
                break;
            case 'f':
                decoded.append(u'\f');
                break;
            case 'n':
                decoded.append(u'\n');
                break;
            case 'r':
                decoded.append(u'\r');
                break;
            case 't':
                decoded.append(u'\t');
                break;
            case 'u': {
                ushort codeUnit = 0;
                if (!parseHexCodeUnit(codeUnit)) {
                    return false;
                }
                const QChar unicode(codeUnit);
                if (unicode.isHighSurrogate()) {
                    if (position_ + 2 > text_.size() || text_.at(position_) != u'\\'
                        || text_.at(position_ + 1) != u'u') {
                        return false;
                    }
                    position_ += 2;
                    ushort lowCodeUnit = 0;
                    if (!parseHexCodeUnit(lowCodeUnit)
                        || !QChar(lowCodeUnit).isLowSurrogate()) {
                        return false;
                    }
                    decoded.append(unicode);
                    decoded.append(QChar(lowCodeUnit));
                } else if (unicode.isLowSurrogate()) {
                    return false;
                } else {
                    decoded.append(unicode);
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
        if (position_ + 4 > text_.size()) {
            return false;
        }
        ushort decoded = 0;
        for (qsizetype index = 0; index < 4; ++index) {
            const QChar character = text_.at(position_ + index);
            ushort digit = 0;
            if (character >= u'0' && character <= u'9') {
                digit = static_cast<ushort>(character.unicode() - u'0');
            } else if (character >= u'a' && character <= u'f') {
                digit = static_cast<ushort>(character.unicode() - u'a' + 10);
            } else if (character >= u'A' && character <= u'F') {
                digit = static_cast<ushort>(character.unicode() - u'A' + 10);
            } else {
                return false;
            }
            decoded = static_cast<ushort>((decoded << 4U) | digit);
        }
        position_ += 4;
        value = decoded;
        return true;
    }

    bool parseNumber()
    {
        consume(u'-');
        if (position_ >= text_.size()) {
            return false;
        }
        if (text_.at(position_) == u'0') {
            ++position_;
            if (position_ < text_.size() && text_.at(position_).isDigit()) {
                return false;
            }
        } else if (text_.at(position_) >= u'1' && text_.at(position_) <= u'9') {
            do {
                ++position_;
            } while (position_ < text_.size() && text_.at(position_) >= u'0'
                     && text_.at(position_) <= u'9');
        } else {
            return false;
        }
        if (consume(u'.')) {
            const qsizetype start = position_;
            while (position_ < text_.size() && text_.at(position_) >= u'0'
                   && text_.at(position_) <= u'9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        if (position_ < text_.size()
            && (text_.at(position_) == u'e' || text_.at(position_) == u'E')) {
            ++position_;
            if (position_ < text_.size()
                && (text_.at(position_) == u'+' || text_.at(position_) == u'-')) {
                ++position_;
            }
            const qsizetype start = position_;
            while (position_ < text_.size() && text_.at(position_) >= u'0'
                   && text_.at(position_) <= u'9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        return true;
    }

    bool consumeKeyword(const QStringView keyword)
    {
        if (QStringView(text_).mid(position_, keyword.size()) != keyword) {
            return false;
        }
        position_ += keyword.size();
        return true;
    }

    bool countAggregateEntry()
    {
        if (aggregateEntries_ >= FrameCodec::maximumJsonAggregateEntries()) {
            result_ = StrictScanResult::ResourceLimit;
            return false;
        }
        ++aggregateEntries_;
        return true;
    }

    void skipWhitespace()
    {
        while (position_ < text_.size()) {
            const QChar character = text_.at(position_);
            if (character != u' ' && character != u'\t' && character != u'\n'
                && character != u'\r') {
                break;
            }
            ++position_;
        }
    }

    bool consume(const QChar expected)
    {
        if (position_ >= text_.size() || text_.at(position_) != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    QString text_;
    qsizetype position_ = 0;
    qsizetype aggregateEntries_ = 0;
    StrictScanResult result_ = StrictScanResult::Valid;
};

} // namespace

QByteArray FrameCodec::encode(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > static_cast<qsizetype>(maximumPayloadBytes())) {
        return {};
    }

    const auto size = static_cast<quint32>(payload.size());
    QByteArray encoded(4, Qt::Uninitialized);
    encoded[0] = static_cast<char>((size >> 24U) & 0xffU);
    encoded[1] = static_cast<char>((size >> 16U) & 0xffU);
    encoded[2] = static_cast<char>((size >> 8U) & 0xffU);
    encoded[3] = static_cast<char>(size & 0xffU);
    encoded.append(payload);
    return encoded;
}

FrameFeedResult FrameCodec::feed(const QByteArrayView bytes)
{
    if (isFailed()) {
        return {FrameStatus::Failed, {}, error_, errorCode_};
    }

    QList<QJsonObject> frames;
    qsizetype inputPosition = 0;
    for (;;) {
        if (queued_.size() < 4 && inputPosition < bytes.size()) {
            const qsizetype appended = std::min(4 - queued_.size(),
                                                bytes.size() - inputPosition);
            queued_.append(bytes.data() + inputPosition, appended);
            inputPosition += appended;
        }
        if (queued_.size() < 4) {
            break;
        }

        const quint32 payloadSize = decodeLength(queued_);
        if (payloadSize == 0U) {
            return fail(FrameError::ZeroLength, QStringLiteral("ipc.frame.zero_length"));
        }
        if (payloadSize > maximumPayloadBytes()) {
            return fail(FrameError::PayloadTooLarge, QStringLiteral("ipc.frame.payload_too_large"));
        }

        const qsizetype frameSize = 4 + static_cast<qsizetype>(payloadSize);
        if (queued_.size() < frameSize && inputPosition < bytes.size()) {
            const qsizetype appended = std::min(frameSize - queued_.size(),
                                                bytes.size() - inputPosition);
            queued_.append(bytes.data() + inputPosition, appended);
            inputPosition += appended;
        }
        if (queued_.size() < frameSize) {
            break;
        }

        if (frames.size() >= maximumFramesPerFeed()) {
            return fail(FrameError::QueueLimitExceeded,
                        QStringLiteral("ipc.frame.queue_limit"));
        }

        const QByteArray payload = queued_.sliced(4, payloadSize);
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder.decode(payload);
        if (decoder.hasError()) {
            return fail(FrameError::InvalidUtf8, QStringLiteral("ipc.frame.invalid_utf8"));
        }

        const StrictScanResult strictScan = StrictJsonScanner(decoded).scan();
        if (strictScan == StrictScanResult::DuplicateMember) {
            return fail(FrameError::DuplicateMember,
                        QStringLiteral("ipc.frame.duplicate_member"));
        }
        if (strictScan == StrictScanResult::ResourceLimit) {
            return fail(FrameError::JsonResourceLimit,
                        QStringLiteral("ipc.frame.json_resource_limit"));
        }
        if (strictScan != StrictScanResult::Valid) {
            return fail(FrameError::InvalidJson, QStringLiteral("ipc.frame.invalid_json"));
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(decoded.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || document.isNull()) {
            return fail(FrameError::InvalidJson, QStringLiteral("ipc.frame.invalid_json"));
        }
        if (!document.isObject()) {
            return fail(FrameError::RootNotObject, QStringLiteral("ipc.frame.root_not_object"));
        }

        frames.append(document.object());
        queued_.remove(0, frameSize);

        if (inputPosition >= bytes.size() && queued_.isEmpty()) {
            break;
        }
    }

    return {frames.isEmpty() ? FrameStatus::NeedMoreData : FrameStatus::FramesReady,
            frames,
            FrameError::None,
            {}};
}

qsizetype FrameCodec::queuedBytes() const noexcept
{
    return queued_.size();
}

bool FrameCodec::isFailed() const noexcept
{
    return error_ != FrameError::None;
}

FrameError FrameCodec::lastError() const noexcept
{
    return error_;
}

QString FrameCodec::lastErrorCode() const
{
    return errorCode_;
}

FrameFeedResult FrameCodec::fail(const FrameError error, const QString &code)
{
    error_ = error;
    errorCode_ = code;
    queued_.clear();
    return {FrameStatus::Failed, {}, error_, errorCode_};
}
