#include "PolicyEngine.h"

#include <QSet>

EffectivePolicy PolicyEngine::intersect(const ManifestPermissions &manifest,
                                        const HostPolicy &host)
{
    EffectivePolicy effective;

    if (!manifest.network.hosts.isEmpty() && !manifest.network.methods.isEmpty()
        && host.network.has_value()) {
        const auto validated = HostPolicy::validatedNetwork(*host.network);
        if (validated.has_value()) {
            const QSet<QString> declaredHosts(manifest.network.hosts.cbegin(),
                                              manifest.network.hosts.cend());
            QSet<HttpMethod> declaredMethods;
            for (const QString &methodName : manifest.network.methods) {
                const auto method = parseHttpMethod(methodName);
                if (method.has_value()) {
                    declaredMethods.insert(*method);
                }
            }

            EffectiveNetworkPolicy network;
            network.maximumRequestBytes = validated->maximumRequestBytes;
            network.maximumResponseBytes = validated->maximumResponseBytes;
            network.timeoutMs = validated->timeoutMs;
            for (const NetworkAllowRule &rule : validated->rules) {
                if (!declaredHosts.contains(rule.host)) {
                    continue;
                }
                NetworkAllowRule intersected = rule;
                intersected.methods.intersect(declaredMethods);
                if (!intersected.methods.isEmpty()) {
                    network.rules.append(std::move(intersected));
                }
            }
            if (!network.rules.isEmpty()) {
                effective.network = std::move(network);
            }
        }
    }

    if (manifest.storage == StoragePermission::AppPrivate && host.storage.has_value()) {
        const auto validated = HostPolicy::validatedStorage(*host.storage);
        if (validated.has_value()) {
            effective.storage = EffectiveStoragePolicy{validated->quotaBytes};
        }
    }

    if (host.clipboard.has_value()) {
        const bool write = manifest.clipboardWrite && host.clipboard->write;
        const bool read = manifest.clipboardRead == ClipboardReadPermission::UserGesture
            && host.clipboard->readWithUserGesture;
        if (write || read) {
            effective.clipboard = EffectiveClipboardPolicy{write, read};
        }
    }

    if (manifest.fileOpen == FileOpenPermission::UserBrokered && host.file.has_value()) {
        const auto validated = HostPolicy::validatedFile(*host.file);
        if (validated.has_value()) {
            effective.file = EffectiveFilePolicy{true, validated->maximumBytes};
        }
    }
    return effective;
}
