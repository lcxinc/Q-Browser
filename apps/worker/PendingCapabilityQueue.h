#pragma once

#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QString>

#include <optional>

struct PendingCapabilityRequest final {
    QString requestId;
    QString capability;
    QString operation;
    QJsonObject payload;
};

enum class PendingCapabilityPushResult {
    Accepted,
    InvalidRequest,
    DuplicateRequestId,
    LimitExceeded,
};

class PendingCapabilityQueue final
{
public:
    explicit PendingCapabilityQueue(qsizetype maximumRequests = 64,
                                    qsizetype maximumBytes = 512 * 1024);

    PendingCapabilityPushResult enqueue(PendingCapabilityRequest request);
    std::optional<PendingCapabilityRequest> takeNext();
    qsizetype size() const noexcept;
    qsizetype queuedBytes() const noexcept;
    void clear();

private:
    struct Entry final {
        PendingCapabilityRequest request;
        qsizetype bytes = 0;
    };

    qsizetype maximumRequests_;
    qsizetype maximumBytes_;
    qsizetype queuedBytes_ = 0;
    QQueue<Entry> entries_;
    QSet<QString> requestIds_;
};
