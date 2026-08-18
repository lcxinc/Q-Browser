#include "FrameCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStringDecoder>

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
            return result_ == StrictScanResult::DuplicateMember ? result_
                                                                : StrictScanResult::Invalid;
        }
        skipWhitespace();
        return position_ == text_.size() ? result_ : StrictScanResult::Invalid;
    }

private:
    static constexpr qsizetype maximumNesting = 64;

    bool parseValue(const qsizetype nesting)
    {
        if (position_ >= text_.size()) {
            return false;
        }
        const QChar value = text_.at(position_);
        if (value == u'{') {
            return nesting < maximumNesting && parseObject(nesting + 1);
        }
        if (value == u'[') {
            return nesting < maximumNesting && parseArray(nesting + 1);
        }
        if (value == u'"') {
            QString ignored;
            return parseString(ignored);
        }
        const qsizetype start = position_;
        while (position_ < text_.size()) {
            const QChar character = text_.at(position_);
            if (character == u',' || character == u'}' || character == u']'
                || character.isSpace()) {
                break;
            }
            ++position_;
        }
        return position_ > start;
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
        if (position_ >= text_.size() || text_.at(position_) != u'"') {
            return false;
        }
        const qsizetype start = position_++;
        bool escaped = false;
        while (position_ < text_.size()) {
            const QChar character = text_.at(position_++);
            if (escaped) {
                escaped = false;
                continue;
            }
            if (character == u'\\') {
                escaped = true;
                continue;
            }
            if (character == u'"') {
                const QString token = text_.mid(start, position_ - start);
                const QJsonDocument wrapper = QJsonDocument::fromJson(
                    (QByteArrayLiteral("[") + token.toUtf8() + QByteArrayLiteral("]")));
                if (!wrapper.isArray() || wrapper.array().size() != 1
                    || !wrapper.array().first().isString()) {
                    return false;
                }
                decoded = wrapper.array().first().toString();
                return true;
            }
        }
        return false;
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
    if (bytes.size() > maximumQueuedBytes() - queued_.size()) {
        return fail(FrameError::QueueLimitExceeded, QStringLiteral("ipc.frame.queue_limit"));
    }
    queued_.append(bytes.data(), bytes.size());

    QList<QJsonObject> frames;
    while (queued_.size() >= 4) {
        const quint32 payloadSize = decodeLength(queued_);
        if (payloadSize == 0U) {
            return fail(FrameError::ZeroLength, QStringLiteral("ipc.frame.zero_length"));
        }
        if (payloadSize > maximumPayloadBytes()) {
            return fail(FrameError::PayloadTooLarge, QStringLiteral("ipc.frame.payload_too_large"));
        }

        const qsizetype frameSize = 4 + static_cast<qsizetype>(payloadSize);
        if (queued_.size() < frameSize) {
            break;
        }

        const QByteArray payload = queued_.sliced(4, payloadSize);
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder.decode(payload);
        if (decoder.hasError()) {
            return fail(FrameError::InvalidUtf8, QStringLiteral("ipc.frame.invalid_utf8"));
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(decoded.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || document.isNull()) {
            return fail(FrameError::InvalidJson, QStringLiteral("ipc.frame.invalid_json"));
        }
        if (!document.isObject()) {
            return fail(FrameError::RootNotObject, QStringLiteral("ipc.frame.root_not_object"));
        }
        const StrictScanResult strictScan = StrictJsonScanner(decoded).scan();
        if (strictScan == StrictScanResult::DuplicateMember) {
            return fail(FrameError::DuplicateMember,
                        QStringLiteral("ipc.frame.duplicate_member"));
        }
        if (strictScan != StrictScanResult::Valid) {
            return fail(FrameError::InvalidJson, QStringLiteral("ipc.frame.invalid_json"));
        }

        if (frames.size() >= maximumFramesPerFeed()) {
            return fail(FrameError::QueueLimitExceeded,
                        QStringLiteral("ipc.frame.queue_limit"));
        }
        frames.append(document.object());
        queued_.remove(0, frameSize);
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
