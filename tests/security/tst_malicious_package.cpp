#include "Archive.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "QmlSourcePolicy.h"
#include "SignatureVerifier.h"

#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <Aclapi.h>
#endif

#include <algorithm>
#include <iterator>
#include <optional>

namespace {
struct RawEntry final
{
    QByteArray name;
    QByteArray data = QByteArrayLiteral("x");
    quint16 method = 0;
    std::optional<quint32> crc;
    std::optional<quint32> compressedSize;
    std::optional<quint32> uncompressedSize;
};

void append16(QByteArray &bytes, const quint16 value)
{
    const quint16 little = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&little),
                 static_cast<qsizetype>(sizeof(little)));
}

void append32(QByteArray &bytes, const quint32 value)
{
    const quint32 little = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&little),
                 static_cast<qsizetype>(sizeof(little)));
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const quint32 mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

QByteArray rawZip(const QVector<RawEntry> &entries)
{
    QByteArray bytes;
    struct Central final {
        RawEntry entry;
        quint32 offset = 0;
        quint32 crc = 0;
        quint32 compressed = 0;
        quint32 uncompressed = 0;
    };
    QVector<Central> central;
    for (const RawEntry &entry : entries) {
        Central item{entry,
                     static_cast<quint32>(bytes.size()),
                     entry.crc.value_or(crc32(entry.data)),
                     entry.compressedSize.value_or(
                         static_cast<quint32>(entry.data.size())),
                     entry.uncompressedSize.value_or(
                         static_cast<quint32>(entry.data.size()))};
        central.push_back(item);
        append32(bytes, 0x04034B50U);
        append16(bytes, 20);
        append16(bytes, 0);
        append16(bytes, entry.method);
        append16(bytes, 0);
        append16(bytes, 0x0021);
        append32(bytes, item.crc);
        append32(bytes, item.compressed);
        append32(bytes, item.uncompressed);
        append16(bytes, static_cast<quint16>(entry.name.size()));
        append16(bytes, 0);
        bytes.append(entry.name);
        bytes.append(entry.data);
    }
    const quint32 centralOffset = static_cast<quint32>(bytes.size());
    for (const Central &item : central) {
        append32(bytes, 0x02014B50U);
        append16(bytes, 0x0314U);
        append16(bytes, 20);
        append16(bytes, 0);
        append16(bytes, item.entry.method);
        append16(bytes, 0);
        append16(bytes, 0x0021);
        append32(bytes, item.crc);
        append32(bytes, item.compressed);
        append32(bytes, item.uncompressed);
        append16(bytes, static_cast<quint16>(item.entry.name.size()));
        append16(bytes, 0);
        append16(bytes, 0);
        append16(bytes, 0);
        append16(bytes, 0);
        append32(bytes, 0x81A40000U);
        append32(bytes, item.offset);
        bytes.append(item.entry.name);
    }
    const quint32 centralSize = static_cast<quint32>(bytes.size()) - centralOffset;
    append32(bytes, 0x06054B50U);
    append16(bytes, 0);
    append16(bytes, 0);
    append16(bytes, static_cast<quint16>(central.size()));
    append16(bytes, static_cast<quint16>(central.size()));
    append32(bytes, centralSize);
    append32(bytes, centralOffset);
    append16(bytes, 0);
    return bytes;
}

QString writeRawZip(QTemporaryDir &temporary,
                    const QString &name,
                    const QVector<RawEntry> &entries)
{
    const QString path = temporary.filePath(name + QStringLiteral(".qapkg"));
    QFile file(path);
    const QByteArray bytes = rawZip(entries);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            && file.write(bytes) == bytes.size()
        ? path : QString{};
}

QByteArray minimalPeImage()
{
    QByteArray image(128, '\0');
    image[0] = 'M';
    image[1] = 'Z';
    qToLittleEndian<quint32>(64U,
                             reinterpret_cast<uchar *>(image.data() + 0x3c));
    image.replace(64, 4, QByteArray("PE\0\0", 4));
    return image;
}

bool isWithinTestOwnedRoot(const QString &root, const QString &candidate)
{
    const QString cleanRoot = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
    const QString cleanCandidate =
        QDir::fromNativeSeparators(
            QDir::cleanPath(QFileInfo(candidate).absoluteFilePath()));
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return cleanCandidate.compare(cleanRoot, sensitivity) == 0
        || cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'), sensitivity);
}

bool restoreWritableEntry(const QString &path, QString *error)
{
#ifdef Q_OS_WIN
    QString native = QDir::toNativeSeparators(path);
    if (!native.startsWith(QStringLiteral("\\\\?\\"))) {
        native = native.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + native.sliced(2)
            : QStringLiteral("\\\\?\\") + native;
    }
    const DWORD aclResult = SetNamedSecurityInfoW(
        reinterpret_cast<LPWSTR>(native.data()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr);
    if (aclResult != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("ACL restore failed for %1: %2")
                         .arg(path).arg(aclResult);
        }
        return false;
    }
    const auto *nativePath = reinterpret_cast<LPCWSTR>(native.utf16());
    const DWORD attributes = GetFileAttributesW(nativePath);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) {
            *error = QStringLiteral("attribute read failed for %1: %2")
                         .arg(path).arg(GetLastError());
        }
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U
        && SetFileAttributesW(nativePath,
                              attributes & ~FILE_ATTRIBUTE_READONLY) == FALSE) {
        if (error != nullptr) {
            *error = QStringLiteral("attribute restore failed for %1: %2")
                         .arg(path).arg(GetLastError());
        }
        return false;
    }
#else
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        if (error != nullptr) {
            *error = QStringLiteral("permissions restore failed for %1").arg(path);
        }
        return false;
    }
#endif
    return true;
}

class TestOwnedTemporaryDir final : public QTemporaryDir
{
public:
    TestOwnedTemporaryDir()
        : ownedRoot_(QDir::fromNativeSeparators(
              QDir::cleanPath(QFileInfo(path()).absoluteFilePath())))
    {
    }

    ~TestOwnedTemporaryDir()
    {
        if (!cleaned_ && QFileInfo::exists(ownedRoot_)) {
            (void)cleanup();
        }
    }

    bool cleanup()
    {
        if (cleaned_) return cleanupError_.isEmpty();
        cleanupError_.clear();
        if (!isValid() || ownedRoot_.isEmpty()
            || QDir::fromNativeSeparators(
                   QDir::cleanPath(QFileInfo(path()).absoluteFilePath())) != ownedRoot_
            || QFileInfo(ownedRoot_).fileName().startsWith(
                   QStringLiteral("tst_malicious_package-")) == false) {
            cleanupError_ = QStringLiteral("temporary root ownership validation failed: %1")
                                .arg(ownedRoot_);
            return false;
        }
        if (!QFileInfo::exists(ownedRoot_)) {
            cleaned_ = true;
            setAutoRemove(false);
            return true;
        }

        QStringList paths{ownedRoot_};
        QDirIterator iterator(
            ownedRoot_,
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString entry = iterator.next();
            if (!isWithinTestOwnedRoot(ownedRoot_, entry)) {
                cleanupError_ = QStringLiteral("temporary entry escaped owned root: %1")
                                    .arg(entry);
                return false;
            }
            paths.push_back(entry);
        }
        std::sort(paths.begin(), paths.end(), [](const QString &left,
                                                 const QString &right) {
            return left.count(QLatin1Char('/')) > right.count(QLatin1Char('/'))
                || (left.count(QLatin1Char('/')) == right.count(QLatin1Char('/'))
                    && left.size() > right.size());
        });
        bool restored = true;
        for (const QString &entry : paths) {
            QString entryError;
            if (!restoreWritableEntry(entry, &entryError)) {
                restored = false;
                if (cleanupError_.isEmpty()) cleanupError_ = entryError;
            }
        }
        const bool removed = QDir(ownedRoot_).removeRecursively();
        if (!removed || QFileInfo::exists(ownedRoot_)) {
            if (cleanupError_.isEmpty()) {
                cleanupError_ = QStringLiteral("temporary root removal failed: %1")
                                    .arg(ownedRoot_);
            }
            return false;
        }
        cleaned_ = true;
        setAutoRemove(false);
        return restored;
    }

    QString cleanupError() const { return cleanupError_; }
    QString ownedRoot() const { return ownedRoot_; }

private:
    QString ownedRoot_;
    QString cleanupError_;
    bool cleaned_ = false;
};

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
    void rejectsSourceImportsMissingFromSignedManifest();
    void distinguishesBenignMzAssetFromRenamedPe();
    void removesImmutableTestOwnedRoot();
};

void MaliciousPackageTest::rejectsWrongKeyAndSignedPayloadTamper()
{
    TestOwnedTemporaryDir temporary;
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
    QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
}

void MaliciousPackageTest::rejectsZipSlipCanonicalCollisionAndResourceBombs()
{
    TestOwnedTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ArchiveLimits limits;
    limits.maximumEntries = 2;
    limits.maximumEntryBytes = 2;
    limits.maximumTotalBytes = 2;
    limits.maximumCompressionRatio = 16;
    const RawEntry one{QByteArrayLiteral("a"), QByteArrayLiteral("a")};
    const RawEntry two{QByteArrayLiteral("b"), QByteArrayLiteral("b")};
    const RawEntry three{QByteArrayLiteral("c"), QByteArrayLiteral("c")};
    RawEntry oversized{QByteArrayLiteral("large"), QByteArrayLiteral("xxx")};
    RawEntry ratio{QByteArrayLiteral("ratio")};
    ratio.data = QByteArray::fromHex("4b4ca43d0000");
    ratio.method = 8;
    ratio.crc = crc32(QByteArray(100, 'a'));
    ratio.uncompressedSize = 100;

    const struct Fixture {
        QString name;
        QVector<RawEntry> entries;
        ArchiveErrorCode expected;
    } fixtures[]{
        {QStringLiteral("traversal"),
         {{QByteArrayLiteral("../outside.txt"), QByteArrayLiteral("x")}},
         ArchiveErrorCode::InvalidEntryPath},
        {QStringLiteral("collision"),
         {{QByteArrayLiteral("qml/Main.qml"), QByteArrayLiteral("a")},
          {QByteArrayLiteral("QML/main.qml"), QByteArrayLiteral("b")}},
         ArchiveErrorCode::DuplicateEntryPath},
        {QStringLiteral("entry-count"), {one, two, three},
         ArchiveErrorCode::EntryCountLimit},
        {QStringLiteral("entry-size"), {oversized},
         ArchiveErrorCode::EntrySizeLimit},
        {QStringLiteral("aggregate-size"), {one, two, three},
         ArchiveErrorCode::TotalSizeLimit},
        {QStringLiteral("ratio"), {ratio},
         ArchiveErrorCode::CompressionRatioLimit},
    };

    const auto keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    for (const Fixture &fixture : fixtures) {
        ArchiveLimits fixtureLimits = limits;
        if (fixture.name == QStringLiteral("aggregate-size")) {
            fixtureLimits.maximumEntries = 3;
        } else if (fixture.name == QStringLiteral("ratio")) {
            fixtureLimits.maximumEntryBytes = 100;
            fixtureLimits.maximumTotalBytes = 100;
        }
        InstallPolicy installPolicy = policy();
        installPolicy.archiveLimits = fixtureLimits;
        PackageInstaller installer(store, keys.value().publicKeyPem,
                                   std::move(installPolicy));
        const QString path = writeRawZip(temporary, fixture.name, fixture.entries);
        QVERIFY2(!path.isEmpty(), qPrintable(fixture.name));
        const ArchiveResult inspected = Archive::inspect(path, fixtureLimits);
        QVERIFY2(!inspected.hasValue(), qPrintable(fixture.name));
        QCOMPARE(inspected.error().code, fixture.expected);
        const auto snapshot = Archive::snapshot(path, fixtureLimits);
        QVERIFY2(!snapshot.hasValue(), qPrintable(fixture.name));
        const ArchiveErrorCode snapshotExpected = fixture.expected;
        QVERIFY2(snapshot.error().code == snapshotExpected,
                 qPrintable(fixture.name + QStringLiteral(": expected ")
                            + QString::number(static_cast<int>(snapshotExpected))
                            + QStringLiteral(", got ")
                            + QString::number(static_cast<int>(snapshot.error().code))));
        const InstallResult installed = installer.install(path);
        QVERIFY2(!installed.succeeded(), qPrintable(fixture.name));
        QCOMPARE(installed.error, InstallError::ArchiveInvalid);
        QVERIFY(store.resolveCurrent(QStringLiteral("company.security")).path.isEmpty());
    }
    QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
}

void MaliciousPackageTest::rejectsNativeCodeForbiddenImportsAndRemoteSources()
{
    TestOwnedTemporaryDir temporary;
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
        {{QByteArrayLiteral("assets/logo.bin"), minimalPeImage()}});
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
    QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
}

void MaliciousPackageTest::rejectsSourceImportsMissingFromSignedManifest()
{
    TestOwnedTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    InstallPolicy installPolicy = policy();
    installPolicy.allowedImports.insert(QStringLiteral("QtQml"));
    installPolicy.allowedImports.insert(QStringLiteral("QtQuick.Controls"));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               std::move(installPolicy));
    const QString baseline = signedPackage(
        temporary, QStringLiteral("1.9.0"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"),
        {QStringLiteral("QtQuick")});
    QVERIFY(installer.install(baseline).succeeded());
    const QString expectedCurrent =
        store.resolveCurrent(QStringLiteral("company.security")).path;
    QVERIFY(!expectedCurrent.isEmpty());
    const struct SourceCase {
        QByteArray mainQml;
        QVector<ArchiveFile> extras;
    } cases[]{
        {QByteArrayLiteral("// import Fake.Module\nimport QtQuick 2.15 as QQ\n"
                           "import QtQml 2.15 as Qml\nQQ.Item {}"), {}},
        {QByteArrayLiteral("import QtQuick\nItem {}"),
         {{QByteArrayLiteral("scripts/escape.Js"),
           QByteArrayLiteral("// .import Fake.Module\n"
                             ".import QtQuick.Controls 2.15 as Controls")}}},
        {QByteArrayLiteral("import QtQuick\nItem {}"),
         {{QByteArrayLiteral("scripts/escape.MJS"),
           QByteArrayLiteral("/* .import Fake.Module */\n"
                             ".import QtQml as Qml")}}},
    };
    for (qsizetype index = 0; index < std::size(cases); ++index) {
        const QString package = signedPackage(
            temporary, QStringLiteral("2.0.%1").arg(index),
            keys.value().privateKeyPem, cases[index].mainQml,
            {QStringLiteral("QtQuick")}, cases[index].extras);
        QVERIFY(!package.isEmpty());
        const InstallResult result = installer.install(package);
        QVERIFY(!result.succeeded());
        QCOMPARE(result.phase, InstallPhase::Preflight);
        QCOMPARE(result.error, InstallError::ImportDenied);
        QCOMPARE(store.resolveCurrent(QStringLiteral("company.security")).path,
                 expectedCurrent);
    }
    QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
}

void MaliciousPackageTest::distinguishesBenignMzAssetFromRenamedPe()
{
    TestOwnedTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const QString benign = signedPackage(
        temporary, QStringLiteral("3.0.0"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"), {QStringLiteral("QtQuick")},
        {{QByteArrayLiteral("assets/monogram.bin"),
          QByteArrayLiteral("MZ is an ordinary resource prefix")}});
    const InstallResult benignResult = installer.install(benign);
    QVERIFY(benignResult.succeeded());
    const QString current = store.resolveCurrent(QStringLiteral("company.security")).path;
    QVERIFY(!current.isEmpty());

    const QString renamed = signedPackage(
        temporary, QStringLiteral("3.0.1"), keys.value().privateKeyPem,
        QByteArrayLiteral("import QtQuick\nItem {}"), {QStringLiteral("QtQuick")},
        {{QByteArrayLiteral("assets/preview.bin"), minimalPeImage()}});
    const InstallResult renamedResult = installer.install(renamed);
    QVERIFY(!renamedResult.succeeded());
    QCOMPARE(renamedResult.error, InstallError::PreflightRejected);
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.security")).path, current);
    QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
}

void MaliciousPackageTest::removesImmutableTestOwnedRoot()
{
    QString testOwnedRoot;
    {
        TestOwnedTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        testOwnedRoot = temporary.path();
        const auto keys = SignatureVerifier::generateKeyPair();
        QVERIFY(keys.hasValue());
        PackageStore store(temporary.filePath(QStringLiteral("store")));
        PackageInstaller installer(store, keys.value().publicKeyPem, policy());
        const QString package = signedPackage(
            temporary, QStringLiteral("4.0.0"), keys.value().privateKeyPem);
        QVERIFY(!package.isEmpty());
        QVERIFY(installer.install(package).succeeded());
        QVERIFY2(temporary.cleanup(), qPrintable(temporary.cleanupError()));
    }
    QVERIFY2(!QFileInfo::exists(testOwnedRoot), qPrintable(testOwnedRoot));
}

QTEST_MAIN(MaliciousPackageTest)
#include "tst_malicious_package.moc"
