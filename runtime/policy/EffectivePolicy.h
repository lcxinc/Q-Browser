#pragma once

#include "HostPolicy.h"

#include <optional>

struct EffectiveNetworkPolicy {
    QVector<NetworkAllowRule> rules;
    qint64 maximumRequestBytes = 0;
    qint64 maximumResponseBytes = 0;
    int timeoutMs = 0;
};

struct EffectiveStoragePolicy {
    qint64 quotaBytes = 0;
};

struct EffectiveClipboardPolicy {
    bool write = false;
    bool readWithUserGesture = false;
};

struct EffectiveFilePolicy {
    bool open = false;
    qint64 maximumBytes = 0;
};

struct EffectivePolicy {
    std::optional<EffectiveNetworkPolicy> network;
    std::optional<EffectiveStoragePolicy> storage;
    std::optional<EffectiveClipboardPolicy> clipboard;
    std::optional<EffectiveFilePolicy> file;
};
