#include "EffectivePolicy.h"
#include "HostPolicy.h"
#include "Manifest.h"
#include "PolicyEngine.h"

#include <QTest>

namespace {

ManifestPermissions declaredPermissions()
{
    ManifestPermissions permissions;
    permissions.network.hosts = {QStringLiteral("api.example.com"),
                                 QStringLiteral("127.0.0.1")};
    permissions.network.methods = {QStringLiteral("GET"), QStringLiteral("POST")};
    permissions.storage = StoragePermission::AppPrivate;
    permissions.clipboardWrite = true;
    permissions.clipboardRead = ClipboardReadPermission::UserGesture;
    permissions.fileOpen = FileOpenPermission::UserBrokered;
    return permissions;
}

HostPolicy restrictiveHostPolicy()
{
    HostPolicy policy;
    HostNetworkPolicy network;
    network.rules = {
        NetworkAllowRule{QStringLiteral("api.example.com"),
                         QStringLiteral("/v1/orders"),
                         {HttpMethod::Get}},
        NetworkAllowRule{QStringLiteral("unrequested.example.com"),
                         QStringLiteral("/"),
                         {HttpMethod::Post}},
    };
    network.maximumRequestBytes = 1024;
    network.maximumResponseBytes = 4096;
    network.timeoutMs = 750;
    policy.network = network;
    policy.storage = HostStoragePolicy{2048};
    policy.clipboard = HostClipboardPolicy{true, true};
    policy.file = HostFilePolicy{true, 8192};
    return policy;
}

} // namespace

class PolicyEngineTest final : public QObject
{
    Q_OBJECT

private slots:
    void intersectsEveryDeclaredCapability();
    void absentOnEitherSideMeansDeny();
    void hostPolicyRejectsRegexLikeAndInvalidValues();
};

void PolicyEngineTest::intersectsEveryDeclaredCapability()
{
    const EffectivePolicy effective =
        PolicyEngine::intersect(declaredPermissions(), restrictiveHostPolicy());

    QVERIFY(effective.network.has_value());
    QCOMPARE(effective.network->rules.size(), 1);
    QCOMPARE(effective.network->rules.first().host, QStringLiteral("api.example.com"));
    QCOMPARE(effective.network->rules.first().pathPrefix, QStringLiteral("/v1/orders"));
    QCOMPARE(effective.network->rules.first().methods, QSet<HttpMethod>{HttpMethod::Get});
    QCOMPARE(effective.network->maximumRequestBytes, 1024);
    QCOMPARE(effective.network->maximumResponseBytes, 4096);
    QCOMPARE(effective.network->timeoutMs, 750);

    QVERIFY(effective.storage.has_value());
    QCOMPARE(effective.storage->quotaBytes, 2048);
    QVERIFY(effective.clipboard.has_value());
    QVERIFY(effective.clipboard->write);
    QVERIFY(effective.clipboard->readWithUserGesture);
    QVERIFY(effective.file.has_value());
    QVERIFY(effective.file->open);
    QCOMPARE(effective.file->maximumBytes, 8192);
}

void PolicyEngineTest::absentOnEitherSideMeansDeny()
{
    HostPolicy absentHost;
    const EffectivePolicy noHost = PolicyEngine::intersect(declaredPermissions(), absentHost);
    QVERIFY(!noHost.network.has_value());
    QVERIFY(!noHost.storage.has_value());
    QVERIFY(!noHost.clipboard.has_value());
    QVERIFY(!noHost.file.has_value());

    ManifestPermissions absentManifest;
    const EffectivePolicy noManifest =
        PolicyEngine::intersect(absentManifest, restrictiveHostPolicy());
    QVERIFY(!noManifest.network.has_value());
    QVERIFY(!noManifest.storage.has_value());
    QVERIFY(!noManifest.clipboard.has_value());
    QVERIFY(!noManifest.file.has_value());
}

void PolicyEngineTest::hostPolicyRejectsRegexLikeAndInvalidValues()
{
    HostNetworkPolicy invalidNetwork;
    invalidNetwork.rules = {NetworkAllowRule{QStringLiteral(".*\\.example\\.com"),
                                             QStringLiteral("/v1"),
                                             {HttpMethod::Get}}};
    invalidNetwork.maximumRequestBytes = 1;
    invalidNetwork.maximumResponseBytes = 1;
    invalidNetwork.timeoutMs = 1;
    QVERIFY(!HostPolicy::validatedNetwork(invalidNetwork).has_value());

    invalidNetwork.rules.first().host = QStringLiteral("api.example.com");
    invalidNetwork.rules.first().pathPrefix = QStringLiteral("v1");
    QVERIFY(!HostPolicy::validatedNetwork(invalidNetwork).has_value());

    invalidNetwork.rules.first().pathPrefix = QStringLiteral("/v1");
    invalidNetwork.timeoutMs = 0;
    QVERIFY(!HostPolicy::validatedNetwork(invalidNetwork).has_value());

    invalidNetwork.timeoutMs = 1;
    invalidNetwork.rules.first().host = QStringLiteral("127.1");
    QVERIFY(!HostPolicy::validatedNetwork(invalidNetwork).has_value());
}

QTEST_APPLESS_MAIN(PolicyEngineTest)
#include "tst_policy_engine.moc"
