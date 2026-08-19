#include "Archive.h"
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
#include "ArchiveTestHooks.h"
#endif
#include "ContentDigest.h"
#include "Manifest.h"
#include "QmlSourcePolicy.h"
#include "SignatureVerifier.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>

#include <openssl/crypto.h>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#include <Aclapi.h>
#include <qt_windows.h>
#else
#include "PosixStableIo.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>

namespace
{
constexpr int Success = 0;
constexpr int InvalidArguments = 2;
constexpr int OperationFailed = 3;
constexpr qsizetype MaximumPemBytes = 16 * 1024;

#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
void configureArchiveRaceTestHook()
{
    const QString target = qEnvironmentVariable(
        "Q_BROWSER_TEST_ARCHIVE_RACE_TARGET");
    if (target.isEmpty()) {
        return;
    }
    const QByteArray content = qEnvironmentVariable(
        "Q_BROWSER_TEST_ARCHIVE_RACE_CONTENT").toUtf8();
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforeArchivePublish =
        [expected = QFileInfo(target).absoluteFilePath(), content](
            const QString &, const QString &destination) {
            if (QFileInfo(destination).absoluteFilePath() != expected) {
                return;
            }
            QFile sentinel(destination);
            if (sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                (void)sentinel.write(content);
                (void)sentinel.flush();
            }
        };
    qbrowser_archive_testing::setArchiveTestHooks(std::move(hooks));
}
#endif

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

#ifdef Q_OS_WIN
enum class OwnedKeyStageStatus
{
    Ready,
    ParentUnavailable,
    TemporaryCreateFailed,
    WriteFailed,
    FlushFailed,
};

class PrivateKeySecurity final
{
public:
    ~PrivateKeySecurity()
    {
        if (m_acl != nullptr) {
            LocalFree(m_acl);
        }
    }

    [[nodiscard]] bool initialize()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        DWORD required = 0;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        m_tokenBuffer.resize(static_cast<qsizetype>(required));
        const bool tokenRead = required > 0
            && GetTokenInformation(
                token,
                TokenUser,
                m_tokenBuffer.data(),
                required,
                &required);
        CloseHandle(token);
        if (!tokenRead) {
            return false;
        }
        auto *tokenUser = reinterpret_cast<TOKEN_USER *>(m_tokenBuffer.data());
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE
            | READ_CONTROL | WRITE_DAC;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);
        if (SetEntriesInAclW(1, &access, nullptr, &m_acl) != ERROR_SUCCESS
            || !InitializeSecurityDescriptor(
                &m_descriptor, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(
                &m_descriptor, TRUE, m_acl, FALSE)
            || !SetSecurityDescriptorControl(
                &m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            return false;
        }
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = &m_descriptor;
        return true;
    }

    [[nodiscard]] SECURITY_ATTRIBUTES *attributes() noexcept
    {
        return m_attributes.lpSecurityDescriptor != nullptr
            ? &m_attributes
            : nullptr;
    }

private:
    QByteArray m_tokenBuffer;
    PACL m_acl = nullptr;
    SECURITY_DESCRIPTOR m_descriptor{};
    SECURITY_ATTRIBUTES m_attributes{};
};

class OwnedKeyOutput final
{
public:
    ~OwnedKeyOutput()
    {
        if (!m_committed && m_file.isOpen()) {
            (void)m_file.deleteOwned();
        }
    }

    [[nodiscard]] OwnedKeyStageStatus stage(
        const QString &destination,
        const QByteArray &contents,
        qbrowser_archive_detail::WindowsStableDirectoryTree &directory,
        SECURITY_ATTRIBUTES *securityAttributes)
    {
        m_destination = QFileInfo(destination).absoluteFilePath();
        const QString parent = QFileInfo(m_destination).dir().absolutePath();
        m_directory = &directory;
        if (!m_directory->contains(parent) || !m_directory->isStable()) {
            return OwnedKeyStageStatus::ParentUnavailable;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            m_temporary = QDir(parent).absoluteFilePath(
                QStringLiteral(".qbrowser-key-")
                + QUuid::createUuid().toString(QUuid::Id128)
                + QStringLiteral(".tmp"));
            if (m_file.createOwnedOutput(
                    m_temporary, *m_directory, securityAttributes)) {
                break;
            }
            m_temporary.clear();
        }
        if (m_temporary.isEmpty()) {
            return OwnedKeyStageStatus::TemporaryCreateFailed;
        }
        if (!m_file.writeAll(
                contents.constData(), static_cast<size_t>(contents.size()))) {
            return OwnedKeyStageStatus::WriteFailed;
        }
        return m_file.flush()
            ? OwnedKeyStageStatus::Ready
            : OwnedKeyStageStatus::FlushFailed;
    }

    [[nodiscard]] bool publish()
    {
        if (!probeExclusiveLock(m_destination, m_temporary)) {
            return false;
        }
        injectKeyRace(m_destination);
        return m_directory != nullptr && m_directory->isStable()
            && m_file.publishNoReplace(m_destination, *m_directory);
    }

    void commit() noexcept { m_committed = true; }

private:
    static bool probeExclusiveLock(
        const QString &destination,
        const QString &temporary)
    {
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
        const QString target = qEnvironmentVariable(
            "Q_BROWSER_TEST_KEYGEN_LOCK_TARGET");
        if (target.isEmpty()
            || QFileInfo(target).absoluteFilePath()
                != QFileInfo(destination).absoluteFilePath()) {
            return true;
        }
        const auto cannotOpen = [&](DWORD access) {
            const HANDLE second = CreateFileW(
                reinterpret_cast<LPCWSTR>(temporary.utf16()),
                access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (second == INVALID_HANDLE_VALUE) {
                return true;
            }
            (void)CloseHandle(second);
            return false;
        };
        const bool exclusive = cannotOpen(GENERIC_READ)
            && cannotOpen(GENERIC_WRITE)
            && DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.utf16())) == FALSE;
        const QString markerPath = qEnvironmentVariable(
            "Q_BROWSER_TEST_KEYGEN_LOCK_MARKER");
        QFile marker(markerPath);
        return exclusive && !markerPath.isEmpty()
            && marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            && marker.write("exclusive") == 9 && marker.flush();
#else
        Q_UNUSED(destination);
        Q_UNUSED(temporary);
        return true;
#endif
    }

    static void injectKeyRace(const QString &destination)
    {
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
        const QString target = qEnvironmentVariable(
            "Q_BROWSER_TEST_KEYGEN_RACE_TARGET");
        if (!target.isEmpty()
            && QFileInfo(target).absoluteFilePath()
                == QFileInfo(destination).absoluteFilePath()) {
            QFile sentinel(destination);
            if (sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                const QByteArray content = qEnvironmentVariable(
                    "Q_BROWSER_TEST_KEYGEN_RACE_CONTENT").toUtf8();
                (void)sentinel.write(content);
                (void)sentinel.flush();
            }
        }
#else
        Q_UNUSED(destination);
#endif
    }

    qbrowser_archive_detail::WindowsStableDirectoryTree *m_directory = nullptr;
    qbrowser_archive_detail::WindowsStableFile m_file;
    QString m_destination;
    QString m_temporary;
    bool m_committed = false;
};
#else
enum class OwnedKeyStageStatus
{
    Ready,
    ParentUnavailable,
    TemporaryCreateFailed,
    WriteFailed,
    FlushFailed,
};

class OwnedKeyOutput final
{
public:
    [[nodiscard]] OwnedKeyStageStatus stage(
        const QString &destination,
        const QByteArray &contents,
        mode_t mode)
    {
        m_absoluteDestination = QFileInfo(destination).absoluteFilePath();
        m_destination = QFile::encodeName(QFileInfo(destination).fileName());
        if (!m_directory.openAbsolute(
                QFileInfo(destination).dir().absolutePath())
            || m_destination.isEmpty()
            || m_destination.contains('/')) {
            return OwnedKeyStageStatus::ParentUnavailable;
        }
        if (!m_file.create(m_directory, mode)) {
            return OwnedKeyStageStatus::TemporaryCreateFailed;
        }
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
        if (mode == (S_IRUSR | S_IWUSR)
            && qEnvironmentVariableIsSet(
                "Q_BROWSER_TEST_KEYGEN_RELAX_PRIVATE_MODE")) {
            (void)m_file.setModeExact(
                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
                    | S_IROTH | S_IWOTH);
        }
#endif
        if (!m_file.writeAll(
                contents.constData(), static_cast<size_t>(contents.size()))) {
            return OwnedKeyStageStatus::WriteFailed;
        }
        if (!m_file.setModeExact(mode) || !m_file.flush()) {
            return OwnedKeyStageStatus::FlushFailed;
        }
        return OwnedKeyStageStatus::Ready;
    }

    [[nodiscard]] bool publish()
    {
        injectKeyRace();
        return m_file.publishNoReplace(m_destination, m_directory);
    }

    void commit() noexcept {}

private:
    void injectKeyRace() const
    {
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
        const QString target = qEnvironmentVariable(
            "Q_BROWSER_TEST_KEYGEN_RACE_TARGET");
        if (!target.isEmpty()
            && QFileInfo(target).absoluteFilePath() == m_absoluteDestination) {
            const QByteArray content = qEnvironmentVariable(
                "Q_BROWSER_TEST_KEYGEN_RACE_CONTENT").toUtf8();
            const int marker = ::openat(
                m_directory.fd(),
                m_destination.constData(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                S_IRUSR | S_IWUSR);
            if (marker >= 0) {
                (void)::write(marker, content.constData(), content.size());
                (void)::fsync(marker);
                (void)::close(marker);
            }
        }
#endif
    }

    qbrowser_archive_detail::PosixStableDirectory m_directory;
    qbrowser_archive_detail::PosixOwnedOutput m_file;
    QByteArray m_destination;
    QString m_absoluteDestination;
};
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
    OwnedKeyOutput privateOutput;
    OwnedKeyOutput publicOutput;
#ifdef Q_OS_WIN
    const QString privateParent = QFileInfo(privatePath).dir().absolutePath();
    const QString publicParent = QFileInfo(publicPath).dir().absolutePath();
    qbrowser_archive_detail::WindowsStableDirectoryTree privateDirectory;
    qbrowser_archive_detail::WindowsStableDirectoryTree publicDirectory;
    if (!privateDirectory.openRoot(privateParent)
        || !privateDirectory.isStable()) {
        return commandError(QStringLiteral("private_key_stage_failed"));
    }
    qbrowser_archive_detail::WindowsStableDirectoryTree *publicDirectoryPtr =
        &privateDirectory;
    if (QDir::toNativeSeparators(privateParent).compare(
            QDir::toNativeSeparators(publicParent), Qt::CaseInsensitive) != 0) {
        if (!publicDirectory.openRoot(publicParent)
            || !publicDirectory.isStable()) {
            return commandError(QStringLiteral("public_key_stage_failed"));
        }
        publicDirectoryPtr = &publicDirectory;
    }
    PrivateKeySecurity privateSecurity;
    const OwnedKeyStageStatus privateStage = privateSecurity.initialize()
        ? privateOutput.stage(
              privatePath,
              pair.value().privateKeyPem,
              privateDirectory,
              privateSecurity.attributes())
        : OwnedKeyStageStatus::ParentUnavailable;
#else
    const OwnedKeyStageStatus privateStage = privateOutput.stage(
        privatePath,
        pair.value().privateKeyPem,
        S_IRUSR | S_IWUSR);
#endif
    if (privateStage != OwnedKeyStageStatus::Ready) {
        return commandError(QStringLiteral("private_key_stage_failed"));
    }
    const OwnedKeyStageStatus publicStage = publicOutput.stage(
            publicPath,
            pair.value().publicKeyPem,
#ifdef Q_OS_WIN
            *publicDirectoryPtr,
            nullptr);
#else
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
    if (publicStage != OwnedKeyStageStatus::Ready) {
        return commandError(QStringLiteral("public_key_stage_failed"));
    }
    if (!privateOutput.publish()) {
        return commandError(QStringLiteral("private_key_publish_failed"));
    }
    if (!publicOutput.publish()) {
        return commandError(QStringLiteral("public_key_publish_failed"));
    }
    privateOutput.commit();
    publicOutput.commit();
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
    for (const ArchiveFile &file : std::as_const(files)) {
        if ((file.path.endsWith(".qml") || file.path.endsWith(".js"))
            && !QmlSourcePolicy::violations(file.contents).isEmpty()) {
            return commandError(QStringLiteral("source_policy_failed"));
        }
    }
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
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    if (signatureFile == nullptr) {
        return emitInspection(
            inspectionResult(false, true, false, {}, {},
                             QStringLiteral("signature_missing")),
            false);
    }
    if (signatureFile->contents.size() != 64) {
        return emitInspection(
            inspectionResult(false, true, false, {}, {},
                             QStringLiteral("signature_invalid")),
            false);
    }
    const ContentDigestResult digest = ContentDigest::signedPackage(files);
    const QByteArray publicPem = readBounded(publicKeyPath, MaximumPemBytes);
    if (!digest.hasValue() || publicPem.isEmpty()) {
        return emitInspection(
            inspectionResult(false, true, false, {}, {},
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
            inspectionResult(false, true, false, {}, {}, code),
            false);
    }
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    if (!content.isValid()) {
        const QString code = content.error().code == ContentDigestErrorCode::MissingMetadata
            ? QStringLiteral("content_digest_missing")
            : content.error().code == ContentDigestErrorCode::InvalidMetadata
                ? QStringLiteral("content_digest_invalid")
                : QStringLiteral("content_digest_mismatch");
        return emitInspection(
            inspectionResult(false, true, false, {}, {}, code), false);
    }
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr) {
        return emitInspection(
            inspectionResult(false, true, true, {}, {}, QStringLiteral("manifest_invalid")),
            false);
    }
    const ManifestParseResult manifestResult = Manifest::parse(manifestFile->contents);
    if (!manifestResult.hasValue()) {
        return emitInspection(
            inspectionResult(false, true, true, {}, {}, QStringLiteral("manifest_invalid")),
            false);
    }
    const Manifest &manifest = manifestResult.value();
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
#ifdef Q_BROWSER_PACKAGE_CLI_TESTING
    configureArchiveRaceTestHook();
#endif
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
