#include "Archive.h"
#include "ArchiveTestHooks.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace
{
QByteArray manifest(const QString &version,
                    const QString &minimumRuntime = QStringLiteral("1.0.0"),
                    const QString &maximumRuntime = QStringLiteral("1.x"),
                    const QStringList &imports = {QStringLiteral("QtQuick")})
{
    QJsonArray importArray;
    for (const QString &name : imports) {
        importArray.append(name);
    }
    const QJsonObject object{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("appId"), QStringLiteral("company.pilot")},
        {QStringLiteral("version"), version},
        {QStringLiteral("entryPoint"), QStringLiteral("qml/Main.qml")},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("minVersion"), minimumRuntime},
                     {QStringLiteral("maxVersion"), maximumRuntime}}},
        {QStringLiteral("imports"), importArray},
        {QStringLiteral("permissions"), QJsonObject{}},
        {QStringLiteral("limits"),
         QJsonObject{{QStringLiteral("packageBytes"), 1048576},
                     {QStringLiteral("memoryMiB"), 128},
                     {QStringLiteral("processes"), 1}}},
        {QStringLiteral("routes"), QJsonArray{QStringLiteral("/")}}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString signedPackage(QTemporaryDir &temporary,
                      const QString &name,
                      const QByteArray &privatePem,
                      QByteArray manifestBytes,
                      const bool corruptSignature = false)
{
    QVector<ArchiveFile> files{
        {QByteArrayLiteral("manifest.json"), std::move(manifestBytes)},
        {QByteArrayLiteral("qml/Main.qml"), QByteArrayLiteral("import QtQuick\nItem {}")}};
    const ContentDigestResult payload = ContentDigest::payload(files);
    if (!payload.hasValue()) {
        return {};
    }
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), payload.hex()});
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    if (!signedDigest.hasValue()) {
        return {};
    }
    const SignatureOperationResult signature = SignatureVerifier::signPem(
        signedDigest.bytes(), privatePem);
    if (!signature.hasValue()) {
        return {};
    }
    QByteArray signatureBytes = signature.value();
    if (corruptSignature) {
        signatureBytes[0] = static_cast<char>(signatureBytes.at(0) ^ 1);
    }
    files.push_back({QByteArrayLiteral("metadata/signature.ed25519"), signatureBytes});
    const QString path = temporary.filePath(name + QStringLiteral(".qapkg"));
    return Archive::createFromFiles(files, path).hasValue() ? path : QString{};
}

InstallPolicy policy()
{
    InstallPolicy result;
    result.runtimeVersion = QStringLiteral("1.2.0");
    result.allowedImports = {QStringLiteral("QtQuick"), QStringLiteral("Company.Design")};
    result.preflight = [](const Manifest &, const QString &) { return true; };
    return result;
}
}

class PackageInstallerTest final : public QObject
{
    Q_OBJECT

private slots:
    void installsActivatesAndRollsBackVerifiedVersions();
    void rejectsInvalidSignatureWithoutChangingCurrent();
    void rejectsIncompatibleRuntimeWithoutChangingCurrent();
    void rejectsDeniedImportWithoutChangingCurrent();
    void rejectsFailedPreflightWithoutChangingCurrent();
};

void PackageInstallerTest::installsActivatesAndRollsBackVerifiedVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());

    const QString first = signedPackage(
        temporary, QStringLiteral("one"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0")));
    const InstallResult installedFirst = installer.install(first);
    QVERIFY2(installedFirst.succeeded(), qPrintable(installedFirst.stableError));
    QCOMPARE(installedFirst.phase, InstallPhase::Complete);
    QCOMPARE(installedFirst.appId, QStringLiteral("company.pilot"));
    QCOMPARE(installedFirst.version, QStringLiteral("1.0.0"));
    QVERIFY(QFileInfo::exists(installedFirst.path + QStringLiteral("/qml/Main.qml")));
    QVERIFY(store.markCurrentLastKnownGood(QStringLiteral("company.pilot")).succeeded());

    const QString second = signedPackage(
        temporary, QStringLiteral("two"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0")));
    const InstallResult installedSecond = installer.install(second);
    QVERIFY2(installedSecond.succeeded(), qPrintable(installedSecond.stableError));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             installedSecond.path);
    QVERIFY(store.rollback(QStringLiteral("company.pilot")).succeeded());
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             installedFirst.path);
}

void PackageInstallerTest::rejectsInvalidSignatureWithoutChangingCurrent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("one"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    const QString marker = store.root() + QStringLiteral("/.staging/do-not-remove/marker");
    QVERIFY(QDir().mkpath(QFileInfo(marker).dir().absolutePath()));
    QFile markerFile(marker);
    QVERIFY(markerFile.open(QIODevice::WriteOnly));
    QCOMPARE(markerFile.write("preserve"), qint64(8));
    markerFile.close();
    const QString rejectedPackage = signedPackage(
        temporary, QStringLiteral("bad-signature"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0")), true);
#ifdef Q_OS_WIN
    bool sawExactStagingLock = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.afterWindowsHandleOpened = [&](const QString &path,
                                         const quint32 access,
                                         const bool) {
        if (QFileInfo(path).fileName().startsWith(QStringLiteral("install-"))
            && (access & DELETE) != 0U) {
            sawExactStagingLock = true;
        }
    };
    qbrowser_archive_testing::setArchiveTestHooks(std::move(hooks));
#endif
    const InstallResult rejected = installer.install(rejectedPackage);
#ifdef Q_OS_WIN
    qbrowser_archive_testing::resetArchiveTestHooks();
    QVERIFY(sawExactStagingLock);
#endif
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Verify);
    QCOMPARE(rejected.error, InstallError::SignatureInvalid);
    QCOMPARE(rejected.stableError, QStringLiteral("signature_invalid"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
    QVERIFY(QFileInfo::exists(marker));
    const QStringList stagingEntries = QDir(store.root() + QStringLiteral("/.staging"))
                                           .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(stagingEntries, QStringList{QStringLiteral("do-not-remove")});
}

void PackageInstallerTest::rejectsIncompatibleRuntimeWithoutChangingCurrent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("one"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("future"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0"), QStringLiteral("2.0.0"),
                 QStringLiteral("2.x"))));
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Verify);
    QCOMPARE(rejected.error, InstallError::RuntimeIncompatible);
    QCOMPARE(rejected.stableError, QStringLiteral("runtime_incompatible"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::rejectsDeniedImportWithoutChangingCurrent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("one"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("denied-import"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0"), QStringLiteral("1.0.0"),
                 QStringLiteral("1.x"), {QStringLiteral("QtQuick"),
                                          QStringLiteral("Company.Runtime")})));
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Verify);
    QCOMPARE(rejected.error, InstallError::ImportDenied);
    QCOMPARE(rejected.stableError, QStringLiteral("import_denied"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::rejectsFailedPreflightWithoutChangingCurrent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("one"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    InstallPolicy rejectingPolicy = policy();
    rejectingPolicy.preflight = [](const Manifest &, const QString &) { return false; };
    PackageInstaller rejecting(store, keys.value().publicKeyPem,
                               std::move(rejectingPolicy));
    const InstallResult rejected = rejecting.install(signedPackage(
        temporary, QStringLiteral("preflight"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0"))));
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Preflight);
    QCOMPARE(rejected.error, InstallError::PreflightRejected);
    QCOMPARE(rejected.stableError, QStringLiteral("preflight_rejected"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

QTEST_APPLESS_MAIN(PackageInstallerTest)

#include "tst_package_installer.moc"
