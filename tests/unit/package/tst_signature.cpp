#include "Archive.h"
#include "ContentDigest.h"
#include "SignatureVerifier.h"
#include "SignatureTestHooks.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <memory>

namespace
{
#ifdef Q_BROWSER_SIGNATURE_TESTING
class SignatureHookGuard final
{
public:
    explicit SignatureHookGuard(qbrowser_signature_testing::SignatureTestHooks hooks)
    {
        qbrowser_signature_testing::setSignatureTestHooks(std::move(hooks));
    }

    ~SignatureHookGuard()
    {
        qbrowser_signature_testing::resetSignatureTestHooks();
    }

    SignatureHookGuard(const SignatureHookGuard &) = delete;
    SignatureHookGuard &operator=(const SignatureHookGuard &) = delete;
};
#endif

QByteArray hex(const char *value)
{
    return QByteArray::fromHex(QByteArray(value));
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

ArchiveFile file(const char *path, const char *contents)
{
    return {QByteArray(path), QByteArray(contents)};
}

QByteArray rsaPem(bool privateKey)
{
    using Context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
    Context context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *generated = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1
        || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1
        || EVP_PKEY_keygen(context.get(), &generated) != 1) {
        return {};
    }
    Key key(generated, EVP_PKEY_free);
    Bio output(BIO_new(BIO_s_mem()), BIO_free);
    if (!output
        || (privateKey
                ? PEM_write_bio_PrivateKey(
                      output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr)
                : PEM_write_bio_PUBKEY(output.get(), key.get()))
            != 1) {
        return {};
    }
    BUF_MEM *buffer = nullptr;
    BIO_get_mem_ptr(output.get(), &buffer);
    return buffer == nullptr
        ? QByteArray{}
        : QByteArray(buffer->data, static_cast<qsizetype>(buffer->length));
}
}

class SignatureTest final : public QObject
{
    Q_OBJECT

private slots:
    void snapshotsOneValidatedArchiveImage();
    void recreatesDeterministicArchiveFromSnapshot();
    void enforcesArchiveLimitsForInMemoryRepacking();
    void computesDomainSeparatedUnambiguousDigests();
    void excludesOnlyExactMetadataNames();
    void validatesCanonicalPayloadMetadata();
    void rejectsAmbiguousDigestInputs();
    void rejectsNonCanonicalUnicodeDigestPaths();
    void signsAndVerifiesRfc8032Vector();
    void rejectsChangedMessageWrongKeyAndMalformedSizes();
    void generatesStrictPemKeyPairs();
    void rejectsNonEd25519PemAndClearsErrorQueue();
    void cleansesPrivateBioBeforeReleaseOnSuccessAndFailure();
};

void SignatureTest::snapshotsOneValidatedArchiveImage()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    QVERIFY(writeFile(source + QStringLiteral("/z.txt"), "last"));
    QVERIFY(writeFile(source + QStringLiteral("/a.txt"), "first"));
    const QString package = temporary.filePath(QStringLiteral("input.qapkg"));
    QVERIFY(Archive::create(source, package).hasValue());

    const ArchiveSnapshotResult snapshot = Archive::snapshot(package);
    QVERIFY2(snapshot.hasValue(), qPrintable(snapshot.error().message));
    QCOMPARE(snapshot.files().size(), 2);
    QCOMPARE(snapshot.files().at(0), file("a.txt", "first"));
    QCOMPARE(snapshot.files().at(1), file("z.txt", "last"));
}

void SignatureTest::recreatesDeterministicArchiveFromSnapshot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QVector<ArchiveFile> files{
        file("qml/Main.qml", "import QtQuick\n"),
        file("manifest.json", "{}"),
        file("empty.txt", "")};
    const QString first = temporary.filePath(QStringLiteral("first.qapkg"));
    const QString second = temporary.filePath(QStringLiteral("second.qapkg"));
    QVERIFY(Archive::createFromFiles(files, first).hasValue());
    QVERIFY(Archive::createFromFiles(files, second).hasValue());
    QFile firstFile(first);
    QFile secondFile(second);
    QVERIFY(firstFile.open(QIODevice::ReadOnly));
    QVERIFY(secondFile.open(QIODevice::ReadOnly));
    QCOMPARE(firstFile.readAll(), secondFile.readAll());
}

void SignatureTest::enforcesArchiveLimitsForInMemoryRepacking()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString output = temporary.filePath(QStringLiteral("output.qapkg"));
    QCOMPARE(
        Archive::createFromFiles(
            {file("a.txt", "1"), file("a.txt", "2")}, output)
            .error().code,
        ArchiveErrorCode::DuplicateEntryPath);
    QCOMPARE(
        Archive::createFromFiles({file("../a.txt", "1")}, output).error().code,
        ArchiveErrorCode::InvalidEntryPath);
    ArchiveLimits limits;
    limits.maximumEntries = 1;
    QCOMPARE(
        Archive::createFromFiles(
            {file("a.txt", "1"), file("b.txt", "2")}, output, limits)
            .error().code,
        ArchiveErrorCode::EntryCountLimit);
    limits = {};
    limits.maximumEntryBytes = 1;
    QCOMPARE(
        Archive::createFromFiles({file("a.txt", "12")}, output, limits)
            .error().code,
        ArchiveErrorCode::EntrySizeLimit);
    QVERIFY(!QFileInfo::exists(output));
}

void SignatureTest::computesDomainSeparatedUnambiguousDigests()
{
    const QVector<ArchiveFile> files{
        file("ab", "c"),
        file("d", ""),
        file("metadata/content.sha256", "0123456789abcdef")};
    const ContentDigestResult payload = ContentDigest::payload(files);
    const ContentDigestResult signedPackage = ContentDigest::signedPackage(files);
    QVERIFY(payload.hasValue());
    QVERIFY(signedPackage.hasValue());
    QCOMPARE(payload.bytes().size(), 32);
    QCOMPARE(signedPackage.bytes().size(), 32);
    QCOMPARE(
        payload.hex(),
        QByteArray("664fe46ac3037754f3759f968b8db33af539372f09e76b9be0b4aa030e92bfb2"));
    QCOMPARE(
        signedPackage.hex(),
        QByteArray("0316c9424940e8c7c5843125b3800502d839ff68771811cb0bee0ad6c63f2b6d"));
    QVERIFY(payload.bytes() != signedPackage.bytes());

    QVector<ArchiveFile> reversed = files;
    std::reverse(reversed.begin(), reversed.end());
    QCOMPARE(ContentDigest::payload(reversed).bytes(), payload.bytes());
    QCOMPARE(
        ContentDigest::signedPackage(reversed).bytes(), signedPackage.bytes());

    const ContentDigestResult differentBoundary = ContentDigest::payload(
        {file("a", "bc"), file("d", "")});
    QVERIFY(differentBoundary.hasValue());
    QVERIFY(payload.bytes() != differentBoundary.bytes());
}

void SignatureTest::excludesOnlyExactMetadataNames()
{
    const QVector<ArchiveFile> base{file("manifest.json", "{}")};
    const QByteArray payload = ContentDigest::payload(base).bytes();
    const QByteArray signedDigest = ContentDigest::signedPackage(base).bytes();

    QVector<ArchiveFile> withMetadata = base;
    withMetadata.push_back(file("metadata/content.sha256", "0"));
    withMetadata.push_back(file("metadata/signature.ed25519", "signature"));
    QCOMPARE(ContentDigest::payload(withMetadata).bytes(), payload);
    QVERIFY(ContentDigest::signedPackage(withMetadata).bytes() != signedDigest);

    QVector<ArchiveFile> withSimilarNames = base;
    withSimilarNames.push_back(file("metadata/content.sha256.bak", "0"));
    withSimilarNames.push_back(file("metadata/signature.ed25519.bak", "signature"));
    QVERIFY(ContentDigest::payload(withSimilarNames).bytes() != payload);
    QVERIFY(ContentDigest::signedPackage(withSimilarNames).bytes() != signedDigest);
}

void SignatureTest::validatesCanonicalPayloadMetadata()
{
    QVector<ArchiveFile> files{file("manifest.json", "{}"), file("empty.txt", "")};
    const ContentDigestResult payload = ContentDigest::payload(files);
    QVERIFY(payload.hasValue());
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), payload.hex()});
    const ContentDigestValidation valid = ContentDigest::validatePayload(files);
    QVERIFY(valid.isValid());
    QCOMPARE(valid.digest(), payload.bytes());

    files.back().contents = payload.hex().toUpper();
    QCOMPARE(
        ContentDigest::validatePayload(files).error().code,
        ContentDigestErrorCode::InvalidMetadata);
    files.back().contents = QByteArray(64, '0');
    QCOMPARE(
        ContentDigest::validatePayload(files).error().code,
        ContentDigestErrorCode::DigestMismatch);
    files.pop_back();
    QCOMPARE(
        ContentDigest::validatePayload(files).error().code,
        ContentDigestErrorCode::MissingMetadata);
}

void SignatureTest::rejectsAmbiguousDigestInputs()
{
    QCOMPARE(
        ContentDigest::payload({file("a", "1"), file("a", "2")}).error().code,
        ContentDigestErrorCode::DuplicatePath);
    QCOMPARE(
        ContentDigest::payload({file("../a", "1")}).error().code,
        ContentDigestErrorCode::InvalidPath);
}

void SignatureTest::rejectsNonCanonicalUnicodeDigestPaths()
{
    const QByteArray nfdPath = QStringLiteral("e\u0301.txt").toUtf8();
    const QByteArray nfcPath = QStringLiteral("\u00e9.txt").toUtf8();
    QCOMPARE(
        ContentDigest::payload({{nfdPath, QByteArrayLiteral("data")}}).error().code,
        ContentDigestErrorCode::InvalidPath);
    QVERIFY(ContentDigest::payload({{nfcPath, QByteArrayLiteral("data")}}).hasValue());
}

void SignatureTest::signsAndVerifiesRfc8032Vector()
{
    const QByteArray privateSeed = hex(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const Ed25519PublicKey publicKey = *Ed25519PublicKey::fromRaw(hex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    const QByteArray expectedSignature = hex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    const SignatureOperationResult signedMessage =
        SignatureVerifier::signRaw(QByteArray(), privateSeed);
    QVERIFY2(signedMessage.hasValue(), qPrintable(signedMessage.error().message));
    QCOMPARE(signedMessage.value(), expectedSignature);
    const Ed25519Signature signature = *Ed25519Signature::fromRaw(
        signedMessage.value());
    const SignatureVerificationResult verified = SignatureVerifier::verify(
        QByteArray(), publicKey, signature);
    QVERIFY(verified.isVerified());
    QCOMPARE(verified.error().code, SignatureErrorCode::None);
}

void SignatureTest::rejectsChangedMessageWrongKeyAndMalformedSizes()
{
    const SignatureKeyPairResult first = SignatureVerifier::generateKeyPair();
    const SignatureKeyPairResult second = SignatureVerifier::generateKeyPair();
    QVERIFY(first.hasValue());
    QVERIFY(second.hasValue());
    const QByteArray message("canonical digest");
    const SignatureOperationResult signature = SignatureVerifier::signPem(
        message, first.value().privateKeyPem);
    QVERIFY(signature.hasValue());

    const SignatureVerificationResult changed = SignatureVerifier::verifyPem(
        message + '!', first.value().publicKeyPem, signature.value());
    QVERIFY(!changed.isVerified());
    QCOMPARE(changed.error().code, SignatureErrorCode::SignatureMismatch);
    const SignatureVerificationResult wrongKey = SignatureVerifier::verifyPem(
        message, second.value().publicKeyPem, signature.value());
    QVERIFY(!wrongKey.isVerified());
    QCOMPARE(wrongKey.error().code, SignatureErrorCode::SignatureMismatch);
    QCOMPARE(
        SignatureVerifier::verifyPem(
            message, first.value().publicKeyPem, QByteArray(63, '\0'))
            .error().code,
        SignatureErrorCode::InvalidSignatureLength);
    QVERIFY(!Ed25519PublicKey::fromRaw(QByteArray(31, '\0')).has_value());
    QVERIFY(!Ed25519Signature::fromRaw(QByteArray(65, '\0')).has_value());
}

void SignatureTest::generatesStrictPemKeyPairs()
{
    const SignatureKeyPairResult pair = SignatureVerifier::generateKeyPair();
    QVERIFY(pair.hasValue());
    QVERIFY(pair.value().privateKeyPem.contains("BEGIN PRIVATE KEY"));
    QVERIFY(pair.value().publicKeyPem.contains("BEGIN PUBLIC KEY"));
    QVERIFY(!pair.value().privateKeyPem.contains(pair.value().publicKeyRaw.toHex()));
    QCOMPARE(pair.value().publicKeyRaw.size(), 32);

    QCOMPARE(
        SignatureVerifier::signPem("x", pair.value().privateKeyPem + "junk")
            .error().code,
        SignatureErrorCode::InvalidPrivateKey);
    QCOMPARE(
        SignatureVerifier::verifyPem(
            "x", pair.value().publicKeyPem + "junk", QByteArray(64, '\0'))
            .error().code,
        SignatureErrorCode::InvalidPublicKey);
    QCOMPARE(
        SignatureVerifier::signPem("x", QByteArrayLiteral("not a pem"))
            .error().code,
        SignatureErrorCode::InvalidPrivateKey);
}

void SignatureTest::rejectsNonEd25519PemAndClearsErrorQueue()
{
    const QByteArray privatePem = rsaPem(true);
    const QByteArray publicPem = rsaPem(false);
    QVERIFY(!privatePem.isEmpty());
    QVERIFY(!publicPem.isEmpty());
    QCOMPARE(
        SignatureVerifier::signPem("message", privatePem).error().code,
        SignatureErrorCode::InvalidPrivateKey);
    QCOMPARE(ERR_peek_error(), static_cast<unsigned long>(0));
    QCOMPARE(
        SignatureVerifier::verifyPem("message", publicPem, QByteArray(64, '\0'))
            .error().code,
        SignatureErrorCode::InvalidPublicKey);
    QCOMPARE(ERR_peek_error(), static_cast<unsigned long>(0));

    const SignatureKeyPairResult pair = SignatureVerifier::generateKeyPair();
    QVERIFY(pair.hasValue());
    const SignatureOperationResult signature = SignatureVerifier::signPem(
        "message", pair.value().privateKeyPem);
    QVERIFY(signature.hasValue());
    ERR_raise(ERR_LIB_EVP, 1);
    QVERIFY(ERR_peek_error() != 0UL);
    QVERIFY(SignatureVerifier::verifyPem(
                "message", pair.value().publicKeyPem, signature.value())
                .isVerified());
    QCOMPARE(ERR_peek_error(), static_cast<unsigned long>(0));
}

void SignatureTest::cleansesPrivateBioBeforeReleaseOnSuccessAndFailure()
{
#ifdef Q_BROWSER_SIGNATURE_TESTING
    auto runCase = [](bool forceFailure) {
        bool observed = false;
        bool allZero = false;
        qbrowser_signature_testing::SignatureTestHooks hooks;
        hooks.failAfterPrivatePemWrite = forceFailure;
        hooks.afterPrivateBioCleanseBeforeFree =
            [&observed, &allZero](QByteArrayView bytes) {
                observed = true;
                allZero = !bytes.isEmpty()
                    && std::all_of(bytes.cbegin(), bytes.cend(), [](char value) {
                           return value == '\0';
                       });
            };
        SignatureHookGuard guard(std::move(hooks));
        const SignatureKeyPairResult result = SignatureVerifier::generateKeyPair();
        QCOMPARE(result.hasValue(), !forceFailure);
        QVERIFY(observed);
        QVERIFY(allZero);
    };
    runCase(false);
    runCase(true);
#else
    QSKIP("signature test hooks are unavailable");
#endif
}

QTEST_APPLESS_MAIN(SignatureTest)

#include "tst_signature.moc"
