#pragma once

#include "AuthorityAdmissionToken.h"

#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <memory>

struct TabCapabilityAuthority final
{
    QString tabId;
    quint64 runtimeIncarnation = 0;
    QString appIdentity;
    quint32 workerProcessId = 0;
    quintptr workerWindowId = 0;
    quint64 sessionGeneration = 0;
    quint64 leaseAuthorityEpoch = 0;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !tabId.isEmpty() && tabId.size() <= 256
            && runtimeIncarnation != 0 && !appIdentity.isEmpty()
            && appIdentity.size() <= 256 && workerProcessId != 0
            && workerWindowId != 0 && sessionGeneration != 0
            && leaseAuthorityEpoch != 0;
    }

    friend bool operator==(const TabCapabilityAuthority &,
                           const TabCapabilityAuthority &) = default;
};

// The Host binds the immutable tuple and a fresh nonreusable admission token
// before either the gesture router or capability runtime can observe it.
struct TabCapabilityAdmission final
{
    TabCapabilityAuthority authority;
    std::shared_ptr<AuthorityAdmissionToken> token;
};

Q_DECLARE_METATYPE(TabCapabilityAuthority)
