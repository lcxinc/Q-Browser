#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

enum class FrameStatus {
    NeedMoreData,
    FramesReady,
    Failed,
};

enum class FrameError {
    None,
    ZeroLength,
    PayloadTooLarge,
    QueueLimitExceeded,
    InvalidUtf8,
    InvalidJson,
    RootNotObject,
    DuplicateMember,
};

struct FrameFeedResult {
    FrameStatus status = FrameStatus::NeedMoreData;
    QList<QJsonObject> frames;
    FrameError error = FrameError::None;
    QString errorCode;
};

class FrameCodec final
{
public:
    static constexpr quint32 maximumPayloadBytes() noexcept { return 1024U * 1024U; }
    static constexpr qsizetype maximumFramesPerFeed() noexcept { return 256; }
    static constexpr qsizetype maximumQueuedBytes() noexcept
    {
        return static_cast<qsizetype>(maximumPayloadBytes()) + 4;
    }

    static QByteArray encode(const QJsonObject &object);

    FrameFeedResult feed(QByteArrayView bytes);
    qsizetype queuedBytes() const noexcept;
    bool isFailed() const noexcept;
    FrameError lastError() const noexcept;
    QString lastErrorCode() const;

private:
    FrameFeedResult fail(FrameError error, const QString &code);

    QByteArray queued_;
    FrameError error_ = FrameError::None;
    QString errorCode_;
};
