#pragma once

#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

enum class HttpMethod {
    Get,
    Post,
    Put,
};

struct NetworkAllowRule {
    QString host;
    QString pathPrefix;
    QSet<HttpMethod> methods;
};

struct HostNetworkPolicy {
    QVector<NetworkAllowRule> rules;
    qint64 maximumRequestBytes = 0;
    qint64 maximumResponseBytes = 0;
    int timeoutMs = 0;
};

struct HostStoragePolicy {
    qint64 quotaBytes = 0;
};

struct HostClipboardPolicy {
    bool write = false;
    bool readWithUserGesture = false;
};

struct HostFilePolicy {
    bool open = false;
    qint64 maximumBytes = 0;
};

class HostPolicy final
{
public:
    std::optional<HostNetworkPolicy> network;
    std::optional<HostStoragePolicy> storage;
    std::optional<HostClipboardPolicy> clipboard;
    std::optional<HostFilePolicy> file;

    [[nodiscard]] static std::optional<HostNetworkPolicy>
    validatedNetwork(const HostNetworkPolicy &candidate);
    [[nodiscard]] static std::optional<HostStoragePolicy>
    validatedStorage(const HostStoragePolicy &candidate);
    [[nodiscard]] static std::optional<HostFilePolicy>
    validatedFile(const HostFilePolicy &candidate);
};

[[nodiscard]] QString httpMethodName(HttpMethod method);
[[nodiscard]] std::optional<HttpMethod> parseHttpMethod(const QString &method);
