#include "PendingCapabilityQueue.h"

#include "ProtocolMessage.h"

#include <QJsonDocument>

#include <algorithm>

PendingCapabilityQueue::PendingCapabilityQueue(const qsizetype maximumRequests,
                                               const qsizetype maximumBytes)
    : maximumRequests_(std::max<qsizetype>(1, maximumRequests)),
      maximumBytes_(std::max<qsizetype>(1, maximumBytes))
{
}

PendingCapabilityPushResult PendingCapabilityQueue::enqueue(
    PendingCapabilityRequest request)
{
    const auto message = ProtocolMessage::request(request.requestId,
                                                  request.capability,
                                                  request.operation,
                                                  request.payload);
    if (!message.has_value()) {
        return PendingCapabilityPushResult::InvalidRequest;
    }
    if (requestIds_.contains(request.requestId)) {
        return PendingCapabilityPushResult::DuplicateRequestId;
    }
    const qsizetype bytes = QJsonDocument(message->toJson())
                                .toJson(QJsonDocument::Compact).size();
    if (entries_.size() >= maximumRequests_ || bytes > maximumBytes_
        || queuedBytes_ > maximumBytes_ - bytes) {
        return PendingCapabilityPushResult::LimitExceeded;
    }
    queuedBytes_ += bytes;
    requestIds_.insert(request.requestId);
    entries_.enqueue({std::move(request), bytes});
    return PendingCapabilityPushResult::Accepted;
}

std::optional<PendingCapabilityRequest> PendingCapabilityQueue::takeNext()
{
    if (entries_.isEmpty()) {
        return std::nullopt;
    }
    Entry entry = entries_.dequeue();
    queuedBytes_ -= entry.bytes;
    requestIds_.remove(entry.request.requestId);
    return std::move(entry.request);
}

qsizetype PendingCapabilityQueue::size() const noexcept { return entries_.size(); }
qsizetype PendingCapabilityQueue::queuedBytes() const noexcept { return queuedBytes_; }

void PendingCapabilityQueue::clear()
{
    entries_.clear();
    requestIds_.clear();
    queuedBytes_ = 0;
}
