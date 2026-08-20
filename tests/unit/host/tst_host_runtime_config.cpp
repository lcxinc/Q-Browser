#include "HostRuntimeConfig.h"
#include "SignatureVerifier.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace
{
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}

qsizetype argumentIndex(const QStringList &arguments, const QString &prefix)
{
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        if (arguments[index].startsWith(prefix)) return index;
    }
    return -1;
}

struct ValidArguments final
{
    QTemporaryDir root;
    QString store;
    QString sandboxTemp;
    QString runtime;
    QString telemetry;
    QString worker;
    QString publicKey;
    QStringList values;

    ValidArguments()
    {
        const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
        if (!root.isValid() || !keys.hasValue()) return;
        store = root.filePath(QStringLiteral("store"));
        sandboxTemp = root.filePath(QStringLiteral("sandbox-temp"));
        runtime = root.filePath(QStringLiteral("runtime"));
        telemetry = root.filePath(QStringLiteral("telemetry"));
        worker = QDir(runtime).filePath(QStringLiteral("qbrowser-worker.exe"));
        publicKey = root.filePath(QStringLiteral("trusted-public.pem"));
        if (!QDir().mkpath(store) || !QDir().mkpath(sandboxTemp)
            || !QDir().mkpath(runtime) || !QDir().mkpath(telemetry)
            || !writeFile(worker, QByteArrayLiteral("worker"))
            || !writeFile(publicKey, keys.value().publicKeyPem)) {
            values.clear();
            return;
        }
        values = {
            QStringLiteral("--package-mode"),
            QStringLiteral("--app-id=com.qbrowser.runtime"),
            QStringLiteral("--trusted-public-key=") + publicKey,
            QStringLiteral("--package-store=") + store,
            QStringLiteral("--sandbox-temp=") + sandboxTemp,
            QStringLiteral("--runtime-root=") + runtime,
            QStringLiteral("--worker-executable=") + worker,
            QStringLiteral("--telemetry-directory=") + telemetry,
        };
    }
};
}

class HostRuntimeConfigTest final : public QObject
{
    Q_OBJECT

private slots:
    void requiresExplicitMode();
    void acceptsOnlyExplicitTrustedShellWithoutPackageAuthority();
    void loadsCompletePackageAuthorityFromAbsolutePaths();
    void rejectsAmbientOrOverlappingAuthority();
    void rejectsNonLoopbackMockOrigin();
};

void HostRuntimeConfigTest::requiresExplicitMode()
{
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments({});
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::ModeRequired);
    QCOMPARE(result.stableError, QStringLiteral("host.config.mode_required"));
}

void HostRuntimeConfigTest::acceptsOnlyExplicitTrustedShellWithoutPackageAuthority()
{
    const HostRuntimeConfigResult shell = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell")});
    QVERIFY2(shell.value.has_value(), qPrintable(shell.stableError));
    QCOMPARE(shell.value->mode(), HostRuntimeMode::TrustedShell);
    QVERIFY(shell.value->appId().isEmpty());
    QVERIFY(shell.value->trustedPublicKeyPem().isEmpty());

    const HostRuntimeConfigResult mixed = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell"), QStringLiteral("--package-mode")});
    QVERIFY(!mixed.value.has_value());
    QCOMPARE(mixed.error, HostRuntimeConfigError::AmbiguousMode);
}

void HostRuntimeConfigTest::loadsCompletePackageAuthorityFromAbsolutePaths()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QStringList configured = arguments.values;
    configured.push_back(
        QStringLiteral("--mock-origin=http://127.0.0.1:53111/"));
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        configured);
    QVERIFY2(result.value.has_value(), qPrintable(result.stableError));
    QCOMPARE(result.value->mode(), HostRuntimeMode::Package);
    QCOMPARE(result.value->appId(), QStringLiteral("com.qbrowser.runtime"));
    QVERIFY(result.value->trustedPublicKeyPem().startsWith(
        QByteArrayLiteral("-----BEGIN PUBLIC KEY-----")));
    QCOMPARE(result.value->packageStoreRoot(), QDir(arguments.store).canonicalPath());
    QCOMPARE(result.value->sandboxTempRoot(),
             QDir(arguments.sandboxTemp).canonicalPath());
    QCOMPARE(result.value->workerExecutable(),
             QFileInfo(arguments.worker).canonicalFilePath());
    QCOMPARE(result.value->immutableRuntimeRoots(),
             QStringList{QDir(arguments.runtime).canonicalPath()});
    QVERIFY(!result.value->installPackage().has_value());
    QCOMPARE(result.value->mockOrigin(),
             QUrl(QStringLiteral("http://127.0.0.1:53111/")));
}

void HostRuntimeConfigTest::rejectsNonLoopbackMockOrigin()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    arguments.values.push_back(
        QStringLiteral("--mock-origin=http://example.com:4173"));
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values);
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.stableError,
             QStringLiteral("host.config.invalid_mock_origin"));
}

void HostRuntimeConfigTest::rejectsAmbientOrOverlappingAuthority()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QStringList ambient = arguments.values;
    const qsizetype workerIndex = argumentIndex(
        ambient, QStringLiteral("--worker-executable="));
    QVERIFY(workerIndex >= 0);
    ambient[workerIndex] = QStringLiteral("--worker-executable=qbrowser-worker.exe");
    const HostRuntimeConfigResult ambientResult = HostRuntimeConfig::fromArguments(
        ambient);
    QVERIFY(!ambientResult.value.has_value());
    QCOMPARE(ambientResult.error, HostRuntimeConfigError::UnsafePath);

    QStringList overlapping = arguments.values;
    const qsizetype tempIndex = argumentIndex(
        overlapping, QStringLiteral("--sandbox-temp="));
    QVERIFY(tempIndex >= 0);
    overlapping[tempIndex] = QStringLiteral("--sandbox-temp=") + arguments.store;
    const HostRuntimeConfigResult overlapResult = HostRuntimeConfig::fromArguments(
        overlapping);
    QVERIFY(!overlapResult.value.has_value());
    QCOMPARE(overlapResult.error, HostRuntimeConfigError::OverlappingRoots);
}

QTEST_APPLESS_MAIN(HostRuntimeConfigTest)

#include "tst_host_runtime_config.moc"
