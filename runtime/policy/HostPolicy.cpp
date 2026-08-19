#include "HostPolicy.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace {

constexpr qint64 maximumBrokerBytes = 1024LL * 1024LL;
constexpr qint64 maximumStorageBytes = 16LL * 1024LL * 1024LL;

bool isCanonicalIpv4(const QString &host)
{
    const QStringList parts = host.split(u'.', Qt::KeepEmptyParts);
    if (parts.size() != 4) {
        return false;
    }
    for (const QString &part : parts) {
        if (part.isEmpty() || (part.size() > 1 && part.startsWith(u'0'))
            || !std::ranges::all_of(part, [](const QChar character) {
                   return character >= u'0' && character <= u'9';
               })) {
            return false;
        }
        bool ok = false;
        const int octet = part.toInt(&ok);
        if (!ok || octet > 255) {
            return false;
        }
    }
    return true;
}

bool isCanonicalHostname(const QString &host)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*$)"));
    if (host.size() > 253 || !pattern.match(host).hasMatch()) {
        return false;
    }
    const QStringList labels = host.split(u'.');
    return std::ranges::all_of(labels,
                               [](const QString &label) { return label.size() <= 63; });
}

bool isLegacyIpv4Alias(const QString &host)
{
    const QStringList parts = host.split(u'.', Qt::KeepEmptyParts);
    if (parts.isEmpty() || parts.size() > 4) {
        return false;
    }
    static const QRegularExpression numericComponent(
        QStringLiteral(R"(^(?:[0-9]+|0[xX][0-9A-Fa-f]+)$)"));
    return std::ranges::all_of(parts, [](const QString &part) {
        return numericComponent.match(part).hasMatch();
    });
}

bool validPathPrefix(const QString &path)
{
    if (path.isEmpty() || path.size() > 2048 || !path.startsWith(u'/')
        || path.contains(u'\\') || path.contains(u'?') || path.contains(u'#')) {
        return false;
    }
    const QStringList segments = path.split(u'/', Qt::KeepEmptyParts);
    return std::ranges::none_of(segments, [](const QString &segment) {
        return segment == QStringLiteral(".") || segment == QStringLiteral("..");
    });
}

} // namespace

std::optional<QString> canonicalNetworkHost(const QString &host)
{
    if (host.isEmpty() || host.endsWith(u'.') || host.contains(QChar::Null)) {
        return std::nullopt;
    }
    if (isCanonicalIpv4(host)) {
        return host;
    }
    if (isLegacyIpv4Alias(host)) {
        return std::nullopt;
    }
    const QByteArray aceBytes = QUrl::toAce(host);
    if (aceBytes.isEmpty()) {
        return std::nullopt;
    }
    const QString ace = QString::fromLatin1(aceBytes).toLower();
    if (!isCanonicalHostname(ace)) {
        return std::nullopt;
    }
    return ace;
}

std::optional<HostNetworkPolicy>
HostPolicy::validatedNetwork(const HostNetworkPolicy &candidate)
{
    if (candidate.rules.isEmpty() || candidate.maximumRequestBytes <= 0
        || candidate.maximumRequestBytes > maximumBrokerBytes
        || candidate.maximumResponseBytes <= 0
        || candidate.maximumResponseBytes > maximumBrokerBytes || candidate.timeoutMs <= 0
        || candidate.timeoutMs > 60000) {
        return std::nullopt;
    }

    HostNetworkPolicy validated = candidate;
    QSet<QString> uniqueRules;
    for (NetworkAllowRule &rule : validated.rules) {
        const auto canonicalHost = canonicalNetworkHost(rule.host);
        if (!canonicalHost.has_value() || rule.port == 0 || !validPathPrefix(rule.pathPrefix)
            || rule.methods.isEmpty() || rule.addressClasses.isEmpty()) {
            return std::nullopt;
        }
        rule.host = *canonicalHost;
        const QString key = networkSchemeName(rule.scheme) + u'\n' + rule.host + u'\n'
            + QString::number(rule.port) + u'\n' + rule.pathPrefix;
        if (uniqueRules.contains(key)) {
            return std::nullopt;
        }
        uniqueRules.insert(key);
    }
    return validated;
}

std::optional<HostStoragePolicy>
HostPolicy::validatedStorage(const HostStoragePolicy &candidate)
{
    if (candidate.quotaBytes <= 0 || candidate.quotaBytes > maximumStorageBytes) {
        return std::nullopt;
    }
    return candidate;
}

std::optional<HostFilePolicy> HostPolicy::validatedFile(const HostFilePolicy &candidate)
{
    if (!candidate.open || candidate.maximumBytes <= 0
        || candidate.maximumBytes > maximumBrokerBytes) {
        return std::nullopt;
    }
    return candidate;
}

QString httpMethodName(const HttpMethod method)
{
    switch (method) {
    case HttpMethod::Get:
        return QStringLiteral("GET");
    case HttpMethod::Post:
        return QStringLiteral("POST");
    case HttpMethod::Put:
        return QStringLiteral("PUT");
    case HttpMethod::Patch:
        return QStringLiteral("PATCH");
    }
    return {};
}

std::optional<HttpMethod> parseHttpMethod(const QString &method)
{
    if (method == QStringLiteral("GET")) {
        return HttpMethod::Get;
    }
    if (method == QStringLiteral("POST")) {
        return HttpMethod::Post;
    }
    if (method == QStringLiteral("PUT")) {
        return HttpMethod::Put;
    }
    if (method == QStringLiteral("PATCH")) {
        return HttpMethod::Patch;
    }
    return std::nullopt;
}

QString networkSchemeName(const NetworkScheme scheme)
{
    switch (scheme) {
    case NetworkScheme::Http:
        return QStringLiteral("http");
    case NetworkScheme::Https:
        return QStringLiteral("https");
    }
    return {};
}
