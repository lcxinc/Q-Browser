#include "Archive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

#include <algorithm>

namespace
{
struct ProcessResult final
{
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray standardOutput;
    QByteArray standardError;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

ProcessResult runCli(const QStringList &arguments)
{
    QProcess process;
    process.setProgram(QString::fromUtf8(Q_BROWSER_PACKAGE_EXE));
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted() || !process.waitForFinished(30000)) {
        return {};
    }
    return {process.exitCode(),
            process.exitStatus(),
            process.readAllStandardOutput(),
            process.readAllStandardError()};
}

QJsonObject parseOneLineJson(const QByteArray &output)
{
    const QList<QByteArray> lines = output.split('\n');
    if (lines.size() != 2 || !lines.back().isEmpty()) {
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(lines.front(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object()
        : QJsonObject{};
}

QByteArray manifest()
{
    return QByteArrayLiteral(R"json({
  "schemaVersion": 1,
  "appId": "com.qbrowser.pilot",
  "version": "1.0.0",
  "entryPoint": "qml/Main.qml",
  "runtime": {"minVersion": "1.0.0", "maxVersion": "1.x"},
  "imports": ["QtQuick"],
  "permissions": {
    "clipboardWrite": false,
    "process": false
  },
  "limits": {"packageBytes": 1048576, "memoryMiB": 128, "processes": 1},
  "routes": ["/login"]
})json");
}

bool createSource(const QString &root)
{
    return QDir().mkpath(root + QStringLiteral("/qml"))
        && writeFile(root + QStringLiteral("/manifest.json"), manifest())
        && writeFile(root + QStringLiteral("/qml/Main.qml"), "import QtQuick\nItem {}\n")
        && writeFile(root + QStringLiteral("/empty.txt"), QByteArray{});
}

QJsonObject expectInspectFailure(
    const QString &package,
    const QString &publicKey,
    const QString &expectedCode)
{
    const ProcessResult result = runCli(
        {QStringLiteral("inspect"),
         QStringLiteral("--package"), package,
         QStringLiteral("--public-key"), publicKey});
    if (result.exitStatus != QProcess::NormalExit || result.exitCode == 0
        || !result.standardError.isEmpty()) {
        return {};
    }
    const QJsonObject object = parseOneLineJson(result.standardOutput);
    if (object.value(QStringLiteral("verified")).toBool(true)
        || object.value(QStringLiteral("errorCode")).toString() != expectedCode) {
        return {};
    }
    return object;
}

#ifdef Q_OS_WIN
bool hasProtectedSinglePrincipalDacl(const QString &path)
{
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &acl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS || acl == nullptr || descriptor == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool protectedDacl = GetSecurityDescriptorControl(
        descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED) != 0U;
    ACL_SIZE_INFORMATION information{};
    const bool singleAce = GetAclInformation(
        acl, &information, sizeof(information), AclSizeInformation)
        && information.AceCount == 1U;
    LocalFree(descriptor);
    return protectedDacl && singleAce;
}
#endif
}

class PackageCliTest final : public QObject
{
    Q_OBJECT

private slots:
    void keygenPackSignInspectRoundTrip();
    void rejectsOverwriteAndProtectsPrivateKey();
    void packAndSignAreReproducible();
    void inspectFailsClosedForTamperingAndWrongKey();
    void inspectRejectsMalformedMetadataAndManifest();
    void rejectsNonCanonicalArchiveMetadata();
    void reportsInvalidCommandLinesWithoutSecrets();
    void developmentKeyDirectoryIgnoresPrivateMaterial();
};

void PackageCliTest::keygenPackSignInspectRoundTrip()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(createSource(source));
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    QCOMPARE(
        runCli({QStringLiteral("keygen"), QStringLiteral("--private-key"), privateKey,
                QStringLiteral("--public-key"), publicKey}).exitCode,
        0);
    QVERIFY(QFileInfo::exists(privateKey));
    QVERIFY(QFileInfo::exists(publicKey));

    const QString packed = temporary.filePath(QStringLiteral("packed.qapkg"));
    QCOMPARE(
        runCli({QStringLiteral("pack"), QStringLiteral("--source"), source,
                QStringLiteral("--output"), packed}).exitCode,
        0);
    const ArchiveSnapshotResult unsignedSnapshot = Archive::snapshot(packed);
    QVERIFY(unsignedSnapshot.hasValue());
    const auto content = std::find_if(
        unsignedSnapshot.files().cbegin(), unsignedSnapshot.files().cend(),
        [](const ArchiveFile &entry) {
            return entry.path == QByteArrayLiteral("metadata/content.sha256");
        });
    QVERIFY(content != unsignedSnapshot.files().cend());
    QCOMPARE(content->contents.size(), 64);

    const QString signedPackage = temporary.filePath(QStringLiteral("signed.qapkg"));
    QCOMPARE(
        runCli({QStringLiteral("sign"), QStringLiteral("--package"), packed,
                QStringLiteral("--private-key"), privateKey,
                QStringLiteral("--output"), signedPackage}).exitCode,
        0);
    const ProcessResult inspected = runCli(
        {QStringLiteral("inspect"), QStringLiteral("--package"), signedPackage,
         QStringLiteral("--public-key"), publicKey});
    QCOMPARE(inspected.exitStatus, QProcess::NormalExit);
    QCOMPARE(inspected.exitCode, 0);
    QVERIFY(inspected.standardError.isEmpty());
    const QJsonObject object = parseOneLineJson(inspected.standardOutput);
    QCOMPARE(object.keys(), QStringList({QStringLiteral("appId"),
                                        QStringLiteral("archiveValid"),
                                        QStringLiteral("contentDigestValid"),
                                        QStringLiteral("errorCode"),
                                        QStringLiteral("verified"),
                                        QStringLiteral("version")}));
    QVERIFY(object.value(QStringLiteral("verified")).toBool());
    QVERIFY(object.value(QStringLiteral("archiveValid")).toBool());
    QVERIFY(object.value(QStringLiteral("contentDigestValid")).toBool());
    QCOMPARE(object.value(QStringLiteral("appId")).toString(), QString("com.qbrowser.pilot"));
    QCOMPARE(object.value(QStringLiteral("version")).toString(), QString("1.0.0"));
    QCOMPARE(object.value(QStringLiteral("errorCode")).toString(), QString("ok"));
}

void PackageCliTest::rejectsOverwriteAndProtectsPrivateKey()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    QCOMPARE(runCli({"keygen", "--private-key", privateKey, "--public-key", publicKey}).exitCode, 0);
    const QByteArray originalPrivate = readFile(privateKey);
    const ProcessResult repeated = runCli(
        {"keygen", "--private-key", privateKey, "--public-key", publicKey});
    QVERIFY(repeated.exitCode != 0);
    QCOMPARE(readFile(privateKey), originalPrivate);
#ifdef Q_OS_WIN
    QVERIFY(hasProtectedSinglePrincipalDacl(privateKey));
#else
    const QFileDevice::Permissions permissions = QFileInfo(privateKey).permissions();
    QCOMPARE(
        permissions,
        QFileDevice::Permissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif
}

void PackageCliTest::packAndSignAreReproducible()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(createSource(source));
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    QCOMPARE(runCli({"keygen", "--private-key", privateKey, "--public-key", publicKey}).exitCode, 0);
    const QString firstPack = temporary.filePath(QStringLiteral("first.qapkg"));
    const QString secondPack = temporary.filePath(QStringLiteral("second.qapkg"));
    QCOMPARE(runCli({"pack", "--source", source, "--output", firstPack}).exitCode, 0);
    QCOMPARE(runCli({"pack", "--source", source, "--output", secondPack}).exitCode, 0);
    QCOMPARE(readFile(firstPack), readFile(secondPack));
    const QByteArray originalPack = readFile(firstPack);
    QVERIFY(runCli({"pack", "--source", source, "--output", firstPack}).exitCode != 0);
    QCOMPARE(readFile(firstPack), originalPack);
    const QString firstSigned = temporary.filePath(QStringLiteral("first-signed.qapkg"));
    const QString secondSigned = temporary.filePath(QStringLiteral("second-signed.qapkg"));
    QCOMPARE(runCli({"sign", "--package", firstPack, "--private-key", privateKey,
                     "--output", firstSigned}).exitCode, 0);
    QCOMPARE(runCli({"sign", "--package", secondPack, "--private-key", privateKey,
                     "--output", secondSigned}).exitCode, 0);
    QCOMPARE(readFile(firstSigned), readFile(secondSigned));
    const QByteArray originalSigned = readFile(firstSigned);
    QVERIFY(runCli({"sign", "--package", firstPack, "--private-key", privateKey,
                    "--output", firstSigned}).exitCode != 0);
    QCOMPARE(readFile(firstSigned), originalSigned);
}

void PackageCliTest::inspectFailsClosedForTamperingAndWrongKey()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(createSource(source));
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    const QString wrongPrivate = temporary.filePath(QStringLiteral("wrong-private.pem"));
    const QString wrongPublic = temporary.filePath(QStringLiteral("wrong-public.pem"));
    QCOMPARE(runCli({"keygen", "--private-key", privateKey, "--public-key", publicKey}).exitCode, 0);
    QCOMPARE(runCli({"keygen", "--private-key", wrongPrivate, "--public-key", wrongPublic}).exitCode, 0);
    const QString packed = temporary.filePath(QStringLiteral("packed.qapkg"));
    const QString signedPackage = temporary.filePath(QStringLiteral("signed.qapkg"));
    QCOMPARE(runCli({"pack", "--source", source, "--output", packed}).exitCode, 0);
    QCOMPARE(runCli({"sign", "--package", packed, "--private-key", privateKey,
                     "--output", signedPackage}).exitCode, 0);
    QVERIFY(!expectInspectFailure(signedPackage, wrongPublic, "signature_mismatch").isEmpty());

    const QVector<ArchiveFile> signedFiles = Archive::snapshot(signedPackage).files();
    QVector<ArchiveFile> files = signedFiles;
    const auto payload = std::find_if(files.begin(), files.end(), [](const ArchiveFile &entry) {
        return entry.path == QByteArrayLiteral("qml/Main.qml");
    });
    QVERIFY(payload != files.end());
    payload->contents.append("// changed");
    const QString changed = temporary.filePath(QStringLiteral("changed.qapkg"));
    QVERIFY(Archive::createFromFiles(files, changed).hasValue());
    const QJsonObject failure = expectInspectFailure(changed, publicKey, "content_digest_mismatch");
    QVERIFY(!failure.isEmpty());
    QVERIFY(failure.value(QStringLiteral("archiveValid")).toBool());
    QVERIFY(!failure.value(QStringLiteral("contentDigestValid")).toBool());

    files = signedFiles;
    auto signature = std::find_if(files.begin(), files.end(), [](const ArchiveFile &entry) {
        return entry.path == QByteArrayLiteral("metadata/signature.ed25519");
    });
    QVERIFY(signature != files.end());
    signature->contents[0] = static_cast<char>(signature->contents.at(0) ^ 1);
    const QString changedSignature = temporary.filePath(QStringLiteral("changed-signature.qapkg"));
    QVERIFY(Archive::createFromFiles(files, changedSignature).hasValue());
    QVERIFY(!expectInspectFailure(changedSignature, publicKey, "signature_mismatch").isEmpty());

    files = signedFiles;
    signature = std::find_if(files.begin(), files.end(), [](const ArchiveFile &entry) {
        return entry.path == QByteArrayLiteral("metadata/signature.ed25519");
    });
    signature->contents.resize(63);
    const QString shortSignature = temporary.filePath(QStringLiteral("short-signature.qapkg"));
    QVERIFY(Archive::createFromFiles(files, shortSignature).hasValue());
    QVERIFY(!expectInspectFailure(shortSignature, publicKey, "signature_invalid").isEmpty());

    files = signedFiles;
    const auto contentHash = std::find_if(files.begin(), files.end(), [](const ArchiveFile &entry) {
        return entry.path == QByteArrayLiteral("metadata/content.sha256");
    });
    QVERIFY(contentHash != files.end());
    contentHash->contents[0] = contentHash->contents.at(0) == '0' ? '1' : '0';
    const QString changedHash = temporary.filePath(QStringLiteral("changed-hash.qapkg"));
    QVERIFY(Archive::createFromFiles(files, changedHash).hasValue());
    QVERIFY(!expectInspectFailure(changedHash, publicKey, "content_digest_mismatch").isEmpty());

    files = signedFiles;
    files.erase(std::remove_if(files.begin(), files.end(), [](const ArchiveFile &entry) {
                    return entry.path == QByteArrayLiteral("metadata/signature.ed25519");
                }), files.end());
    const QString missingSignature = temporary.filePath(QStringLiteral("missing-signature.qapkg"));
    QVERIFY(Archive::createFromFiles(files, missingSignature).hasValue());
    QVERIFY(!expectInspectFailure(missingSignature, publicKey, "signature_missing").isEmpty());
}

void PackageCliTest::inspectRejectsMalformedMetadataAndManifest()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    QCOMPARE(runCli({"keygen", "--private-key", privateKey, "--public-key", publicKey}).exitCode, 0);

    QVector<ArchiveFile> files{{"manifest.json", manifest()}, {"qml/Main.qml", "import QtQuick\nItem {}"},
                               {"metadata/content.sha256", QByteArray(64, 'A')},
                               {"metadata/signature.ed25519", QByteArray(63, '\0')}};
    const QString invalidDigest = temporary.filePath(QStringLiteral("digest.qapkg"));
    QVERIFY(Archive::createFromFiles(files, invalidDigest).hasValue());
    QVERIFY(!expectInspectFailure(invalidDigest, publicKey, "content_digest_invalid").isEmpty());

    files[2].contents = QByteArray(64, '0');
    files[3].contents = QByteArray(63, '\0');
    const QString badSignature = temporary.filePath(QStringLiteral("signature.qapkg"));
    QVERIFY(Archive::createFromFiles(files, badSignature).hasValue());
    const QJsonObject signatureFailure = expectInspectFailure(
        badSignature, publicKey, "content_digest_mismatch");
    QVERIFY(!signatureFailure.isEmpty());

    files[0].contents = "{}";
    const QString badManifest = temporary.filePath(QStringLiteral("manifest.qapkg"));
    QVERIFY(Archive::createFromFiles(files, badManifest).hasValue());
    QVERIFY(!expectInspectFailure(badManifest, publicKey, "manifest_invalid").isEmpty());
}

void PackageCliTest::rejectsNonCanonicalArchiveMetadata()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(createSource(source));
    const QString privateKey = temporary.filePath(QStringLiteral("private.pem"));
    const QString publicKey = temporary.filePath(QStringLiteral("public.pem"));
    const QString packed = temporary.filePath(QStringLiteral("packed.qapkg"));
    const QString signedPackage = temporary.filePath(QStringLiteral("signed.qapkg"));
    QCOMPARE(runCli({"keygen", "--private-key", privateKey, "--public-key", publicKey}).exitCode, 0);
    QCOMPARE(runCli({"pack", "--source", source, "--output", packed}).exitCode, 0);
    QCOMPARE(runCli({"sign", "--package", packed, "--private-key", privateKey,
                     "--output", signedPackage}).exitCode, 0);

    QByteArray bytes = readFile(signedPackage);
    const qsizetype local = bytes.indexOf(QByteArrayLiteral("PK\x03\x04"));
    const qsizetype central = bytes.indexOf(QByteArrayLiteral("PK\x01\x02"));
    QVERIFY(local >= 0);
    QVERIFY(central >= 0);
    bytes[local + 10] = 1;
    bytes[central + 12] = 1;
    const QString nonCanonical = temporary.filePath(QStringLiteral("noncanonical.qapkg"));
    QVERIFY(writeFile(nonCanonical, bytes));
    QVERIFY(Archive::inspect(nonCanonical).hasValue());
    QVERIFY(!expectInspectFailure(nonCanonical, publicKey, "archive_noncanonical").isEmpty());
}

void PackageCliTest::reportsInvalidCommandLinesWithoutSecrets()
{
    const ProcessResult unknown = runCli({QStringLiteral("unknown")});
    QVERIFY(unknown.exitCode != 0);
    QVERIFY(unknown.standardOutput.isEmpty());
    QVERIFY(!unknown.standardError.contains("PRIVATE KEY"));
    const ProcessResult missing = runCli({QStringLiteral("inspect"), QStringLiteral("--package")});
    QVERIFY(missing.exitCode != 0);
    QVERIFY(missing.standardOutput.isEmpty());
    QVERIFY(!missing.standardError.contains("PRIVATE KEY"));
}

void PackageCliTest::developmentKeyDirectoryIgnoresPrivateMaterial()
{
    const QString root = QString::fromUtf8(Q_BROWSER_SOURCE_DIR);
    const QByteArray ignore = readFile(root + QStringLiteral("/keys/dev/.gitignore"));
    QVERIFY(ignore.contains("*"));
    QVERIFY(ignore.contains("!.gitignore"));
    QVERIFY(ignore.contains("!README.md"));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/keys/dev/README.md")));
}

QTEST_APPLESS_MAIN(PackageCliTest)

#include "tst_package_cli.moc"
