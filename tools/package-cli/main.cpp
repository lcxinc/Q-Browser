#include "Archive.h"
#include "ContentDigest.h"
#include "Manifest.h"
#include "SignatureVerifier.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <openssl/crypto.h>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

#include <algorithm>

namespace
{
constexpr int Success = 0;
constexpr int InvalidArguments = 2;
constexpr int OperationFailed = 3;
constexpr qsizetype MaximumPemBytes = 16 * 1024;

int commandError(const QString &code)
{
    QTextStream stream(stderr);
    stream << "error: " << code << '\n';
    return OperationFailed;
}

QByteArray readBounded(const QString &path, qsizetype maximum)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximum) {
        return {};
    }
    const QByteArray bytes = file.read(maximum + 1);
    return bytes.size() <= maximum ? bytes : QByteArray{};
}

const ArchiveFile *findFile(const QVector<ArchiveFile> &files, const QByteArray &path)
{
    const auto found = std::find_if(
        files.cbegin(), files.cend(), [&path](const ArchiveFile &entry) {
            return entry.path == path;
        });
    return found == files.cend() ? nullptr : &*found;
}

void removeFile(QVector<ArchiveFile> &files, const QByteArray &path)
{
    files.erase(
        std::remove_if(files.begin(), files.end(), [&path](const ArchiveFile &entry) {
            return entry.path == path;
        }),
        files.end());
}

bool outputIsNew(const QString &path)
{
    return !path.isEmpty() && !QFileInfo::exists(path)
        && QFileInfo(path).dir().exists();
}

bool writeNewFile(
    const QString &path,
    const QByteArray &contents,
    QFileDevice::Permissions permissions)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }
    const bool succeeded = file.setPermissions(permissions)
        && file.write(contents) == contents.size() && file.flush();
    file.close();
    if (!succeeded) {
        (void)QFile::remove(path);
    }
    return succeeded;
}

#ifdef Q_OS_WIN
bool writePrivateKey(const QString &path, const QByteArray &contents)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    QByteArray tokenBuffer(static_cast<qsizetype>(required), Qt::Uninitialized);
    const bool tokenRead = required > 0
        && GetTokenInformation(
            token,
            TokenUser,
            tokenBuffer.data(),
            required,
            &required);
    CloseHandle(token);
    if (!tokenRead) {
        return false;
    }
    auto *tokenUser = reinterpret_cast<TOKEN_USER *>(tokenBuffer.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE
        | READ_CONTROL | WRITE_DAC;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(
            &descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        LocalFree(acl);
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    HANDLE file = CreateFileW(
        reinterpret_cast<LPCWSTR>(absolutePath.utf16()),
        GENERIC_WRITE,
        0,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    LocalFree(acl);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool succeeded = contents.size() <= static_cast<qsizetype>(MAXDWORD)
        && WriteFile(
            file,
            contents.constData(),
            static_cast<DWORD>(contents.size()),
            &written,
            nullptr)
        && written == static_cast<DWORD>(contents.size())
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!succeeded) {
        (void)DeleteFileW(reinterpret_cast<LPCWSTR>(absolutePath.utf16()));
    }
    return succeeded;
}
#else
bool writePrivateKey(const QString &path, const QByteArray &contents)
{
    return writeNewFile(
        path,
        contents,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
#endif

int keygen(const QString &privatePath, const QString &publicPath)
{
    if (!outputIsNew(privatePath) || !outputIsNew(publicPath)
        || QFileInfo(privatePath).absoluteFilePath()
            == QFileInfo(publicPath).absoluteFilePath()) {
        return commandError(QStringLiteral("output_exists"));
    }
    const SignatureKeyPairResult pair = SignatureVerifier::generateKeyPair();
    if (!pair.hasValue()) {
        return commandError(QStringLiteral("crypto_failure"));
    }
    if (!writePrivateKey(privatePath, pair.value().privateKeyPem)) {
        (void)QFile::remove(privatePath);
        return commandError(QStringLiteral("private_key_write_failed"));
    }
    if (!writeNewFile(
            publicPath,
            pair.value().publicKeyPem,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ReadUser | QFileDevice::ReadGroup
                | QFileDevice::ReadOther)) {
        (void)QFile::remove(privatePath);
        return commandError(QStringLiteral("public_key_write_failed"));
    }
    return Success;
}

int pack(const QString &source, const QString &output)
{
    if (!outputIsNew(output)) {
        return commandError(QStringLiteral("output_exists"));
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return commandError(QStringLiteral("snapshot_failed"));
    }
    const QString snapshotPath = temporary.filePath(QStringLiteral("source.qapkg"));
    const ArchiveResult created = Archive::create(source, snapshotPath);
    if (!created.hasValue()) {
        return commandError(QStringLiteral("source_invalid"));
    }
    const ArchiveSnapshotResult snapshot = Archive::snapshot(snapshotPath);
    if (!snapshot.hasValue()) {
        return commandError(QStringLiteral("snapshot_failed"));
    }
    QVector<ArchiveFile> files = snapshot.files();
    removeFile(files, QByteArrayLiteral("metadata/content.sha256"));
    removeFile(files, QByteArrayLiteral("metadata/signature.ed25519"));
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr || !Manifest::parse(manifestFile->contents).hasValue()) {
        return commandError(QStringLiteral("manifest_invalid"));
    }
    const ContentDigestResult digest = ContentDigest::payload(files);
    if (!digest.hasValue()) {
        return commandError(QStringLiteral("content_digest_failed"));
    }
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), digest.hex()});
    const ArchiveResult packed = Archive::createFromFiles(files, output);
    return packed.hasValue() ? Success : commandError(QStringLiteral("package_write_failed"));
}

int signPackage(
    const QString &package,
    const QString &privateKeyPath,
    const QString &output)
{
    if (!outputIsNew(output)
        || QFileInfo(package).absoluteFilePath() == QFileInfo(output).absoluteFilePath()) {
        return commandError(QStringLiteral("output_exists"));
    }
    const ArchiveSnapshotResult snapshot = Archive::snapshot(package);
    if (!snapshot.hasValue()) {
        return commandError(snapshot.error().code == ArchiveErrorCode::NonCanonicalArchive
                                ? QStringLiteral("archive_noncanonical")
                                : QStringLiteral("archive_invalid"));
    }
    QVector<ArchiveFile> files = snapshot.files();
    if (findFile(files, QByteArrayLiteral("metadata/signature.ed25519")) != nullptr) {
        return commandError(QStringLiteral("already_signed"));
    }
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr || !Manifest::parse(manifestFile->contents).hasValue()) {
        return commandError(QStringLiteral("manifest_invalid"));
    }
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    if (!content.isValid()) {
        return commandError(QStringLiteral("content_digest_invalid"));
    }
    const ContentDigestResult digest = ContentDigest::signedPackage(files);
    QByteArray privatePem = readBounded(privateKeyPath, MaximumPemBytes);
    if (!digest.hasValue() || privatePem.isEmpty()) {
        return commandError(QStringLiteral("private_key_invalid"));
    }
    const SignatureOperationResult signature = SignatureVerifier::signPem(
        digest.bytes(), privatePem);
    OPENSSL_cleanse(privatePem.data(), static_cast<size_t>(privatePem.size()));
    privatePem.clear();
    if (!signature.hasValue()) {
        return commandError(QStringLiteral("private_key_invalid"));
    }
    files.push_back(
        {QByteArrayLiteral("metadata/signature.ed25519"), signature.value()});
    const ArchiveResult signedArchive = Archive::createFromFiles(files, output);
    return signedArchive.hasValue()
        ? Success
        : commandError(QStringLiteral("package_write_failed"));
}

QJsonObject inspectionResult(
    bool verified,
    bool archiveValid,
    bool contentDigestValid,
    const QString &appId,
    const QString &version,
    const QString &errorCode)
{
    return {{QStringLiteral("verified"), verified},
            {QStringLiteral("archiveValid"), archiveValid},
            {QStringLiteral("contentDigestValid"), contentDigestValid},
            {QStringLiteral("appId"), appId},
            {QStringLiteral("version"), version},
            {QStringLiteral("errorCode"), errorCode}};
}

int emitInspection(const QJsonObject &object, bool success)
{
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
    return success ? Success : OperationFailed;
}

int inspectPackage(const QString &package, const QString &publicKeyPath)
{
    const ArchiveSnapshotResult snapshot = Archive::snapshot(package);
    if (!snapshot.hasValue()) {
        const QString code = snapshot.error().code == ArchiveErrorCode::NonCanonicalArchive
            ? QStringLiteral("archive_noncanonical")
            : QStringLiteral("archive_invalid");
        return emitInspection(inspectionResult(false, false, false, {}, {}, code), false);
    }
    const QVector<ArchiveFile> &files = snapshot.files();
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr) {
        return emitInspection(
            inspectionResult(false, true, false, {}, {}, QStringLiteral("manifest_invalid")),
            false);
    }
    const ManifestParseResult manifestResult = Manifest::parse(manifestFile->contents);
    if (!manifestResult.hasValue()) {
        return emitInspection(
            inspectionResult(false, true, false, {}, {}, QStringLiteral("manifest_invalid")),
            false);
    }
    const Manifest &manifest = manifestResult.value();
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    if (!content.isValid()) {
        const QString code = content.error().code == ContentDigestErrorCode::MissingMetadata
            ? QStringLiteral("content_digest_missing")
            : content.error().code == ContentDigestErrorCode::InvalidMetadata
                ? QStringLiteral("content_digest_invalid")
                : QStringLiteral("content_digest_mismatch");
        return emitInspection(
            inspectionResult(false, true, false, manifest.appId(), manifest.version(), code),
            false);
    }
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    if (signatureFile == nullptr) {
        return emitInspection(
            inspectionResult(false, true, true, manifest.appId(), manifest.version(),
                             QStringLiteral("signature_missing")),
            false);
    }
    if (signatureFile->contents.size() != 64) {
        return emitInspection(
            inspectionResult(false, true, true, manifest.appId(), manifest.version(),
                             QStringLiteral("signature_invalid")),
            false);
    }
    const ContentDigestResult digest = ContentDigest::signedPackage(files);
    const QByteArray publicPem = readBounded(publicKeyPath, MaximumPemBytes);
    if (!digest.hasValue() || publicPem.isEmpty()) {
        return emitInspection(
            inspectionResult(false, true, true, manifest.appId(), manifest.version(),
                             QStringLiteral("key_invalid")),
            false);
    }
    const SignatureVerificationResult verified = SignatureVerifier::verifyPem(
        digest.bytes(), publicPem, signatureFile->contents);
    if (!verified.isVerified()) {
        const QString code = verified.error().code == SignatureErrorCode::InvalidPublicKey
            ? QStringLiteral("key_invalid")
            : verified.error().code == SignatureErrorCode::SignatureMismatch
                ? QStringLiteral("signature_mismatch")
                : QStringLiteral("signature_invalid");
        return emitInspection(
            inspectionResult(false, true, true, manifest.appId(), manifest.version(), code),
            false);
    }
    return emitInspection(
        inspectionResult(true, true, true, manifest.appId(), manifest.version(),
                         QStringLiteral("ok")),
        true);
}

bool onlyOptions(
    const QCommandLineParser &parser,
    std::initializer_list<const QCommandLineOption *> allowed,
    const QVector<const QCommandLineOption *> &all)
{
    return std::all_of(all.cbegin(), all.cend(), [&](const QCommandLineOption *option) {
        return !parser.isSet(*option)
            || std::find(allowed.begin(), allowed.end(), option) != allowed.end();
    });
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbrowser-package"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Create and verify deterministic Q-Browser packages."));
    const QCommandLineOption help = parser.addHelpOption();
    const QCommandLineOption privateKey(
        QStringLiteral("private-key"), QStringLiteral("Private Ed25519 PEM path."),
        QStringLiteral("path"));
    const QCommandLineOption publicKey(
        QStringLiteral("public-key"), QStringLiteral("Public Ed25519 PEM path."),
        QStringLiteral("path"));
    const QCommandLineOption source(
        QStringLiteral("source"), QStringLiteral("Source directory."), QStringLiteral("dir"));
    const QCommandLineOption output(
        QStringLiteral("output"), QStringLiteral("New output path."), QStringLiteral("path"));
    const QCommandLineOption package(
        QStringLiteral("package"), QStringLiteral("Input qapkg path."), QStringLiteral("path"));
    parser.addOptions({privateKey, publicKey, source, output, package});
    parser.addPositionalArgument(
        QStringLiteral("command"), QStringLiteral("keygen, pack, sign, or inspect"));
    if (!parser.parse(application.arguments())) {
        QTextStream(stderr) << "error: invalid_arguments\n";
        return InvalidArguments;
    }
    if (parser.isSet(help)) {
        QTextStream(stdout) << parser.helpText();
        return Success;
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1) {
        QTextStream(stderr) << "error: invalid_arguments\n";
        return InvalidArguments;
    }
    const QVector<const QCommandLineOption *> all{
        &privateKey, &publicKey, &source, &output, &package};
    const QString &command = positional.front();
    if (command == QStringLiteral("keygen")
        && parser.isSet(privateKey) && parser.isSet(publicKey)
        && onlyOptions(parser, {&privateKey, &publicKey}, all)) {
        return keygen(parser.value(privateKey), parser.value(publicKey));
    }
    if (command == QStringLiteral("pack")
        && parser.isSet(source) && parser.isSet(output)
        && onlyOptions(parser, {&source, &output}, all)) {
        return pack(parser.value(source), parser.value(output));
    }
    if (command == QStringLiteral("sign")
        && parser.isSet(package) && parser.isSet(privateKey) && parser.isSet(output)
        && onlyOptions(parser, {&package, &privateKey, &output}, all)) {
        return signPackage(
            parser.value(package), parser.value(privateKey), parser.value(output));
    }
    if (command == QStringLiteral("inspect")
        && parser.isSet(package) && parser.isSet(publicKey)
        && onlyOptions(parser, {&package, &publicKey}, all)) {
        return inspectPackage(parser.value(package), parser.value(publicKey));
    }
    QTextStream(stderr) << "error: invalid_arguments\n";
    return InvalidArguments;
}
