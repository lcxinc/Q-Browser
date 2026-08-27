#include "PackageInstaller.h"

#include "Archive.h"
#include "ArchiveTestHooks.h"
#include "CanonicalArchivePath.h"
#include "ContentDigest.h"
#include "PackageStore.h"
#include "PackageInstallerTestHooks.h"
#include "QmlSourcePolicy.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#endif

namespace
{
#ifdef Q_OS_WIN
QString stagingMemberKey(
    const QString &root,
    const QString &path,
    const bool directory)
{
    return (directory ? QStringLiteral("d:") : QStringLiteral("f:"))
        + QDir::fromNativeSeparators(QDir(root).relativeFilePath(path))
              .toCaseFolded();
}

void addStagingParentDirectories(
    const QString &prefix,
    const QString &relativeFile,
    QSet<QString> &members)
{
    QString parent = QFileInfo(relativeFile).path();
    while (!parent.isEmpty() && parent != QLatin1String(".")) {
        members.insert(QStringLiteral("d:")
                       + (prefix + QLatin1Char('/') + parent).toCaseFolded());
        parent = QFileInfo(parent).path();
    }
}

bool deleteOwnedStagingTree(
    const QString &root,
    qbrowser_archive_detail::WindowsStableDirectoryTree &tree,
    const QSet<QString> &expectedMembers)
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
    if (qbrowser_archive_testing::archiveTestHooks().beforeFailureCleanup) {
        qbrowser_archive_testing::archiveTestHooks().beforeFailureCleanup(root);
    }
#endif
    QStringList files;
    qsizetype scannedMembers = 0;
    QDirIterator iterator(
        root,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            return false;
        }
        const QString key = stagingMemberKey(root, path, info.isDir());
        if (scannedMembers >= expectedMembers.size()
            || !expectedMembers.contains(key)) {
            return false;
        }
        ++scannedMembers;
        if (info.isDir()) {
            if (!tree.addExistingDirectory(path)) {
                return false;
            }
        } else if (info.isFile()) {
            files.push_back(path);
        } else {
            return false;
        }
    }
    if (!tree.isStable()) {
        return false;
    }
    for (const QString &path : files) {
        qbrowser_archive_detail::WindowsStableFile owned;
        if (!owned.openForDelete(path, tree) || !owned.deleteOwned()) {
            return false;
        }
    }
    return tree.deleteHeldTree();
}

class WindowsStagingCleanup final
{
public:
    WindowsStagingCleanup(
        QString root,
        qbrowser_archive_detail::WindowsStableDirectoryTree &tree)
        : m_root(std::move(root))
        , m_tree(tree)
    {
        m_expectedMembers.insert(QStringLiteral("f:candidate.qapkg"));
    }

    ~WindowsStagingCleanup()
    {
        (void)deleteOwnedStagingTree(m_root, m_tree, m_expectedMembers);
    }

    WindowsStagingCleanup(const WindowsStagingCleanup &) = delete;
    WindowsStagingCleanup &operator=(const WindowsStagingCleanup &) = delete;

    [[nodiscard]] bool setAuthenticatedSnapshot(
        const QVector<ArchiveFile> &files,
        const ArchiveLimits &limits)
    {
        if (static_cast<quint64>(files.size()) > limits.maximumEntries) {
            return false;
        }
        QSet<QString> expected{QStringLiteral("f:candidate.qapkg"),
                               QStringLiteral("d:preflight"),
                               QStringLiteral("d:candidate")};
        for (const ArchiveFile &file : files) {
            if (!qbrowser_archive_detail::validateArchivePath(file.path, limits)
                     .has_value()) {
                return false;
            }
            const QString relative = QString::fromUtf8(file.path);
            for (const QString &prefix : {QStringLiteral("preflight"),
                                          QStringLiteral("candidate")}) {
                expected.insert(QStringLiteral("f:")
                                + (prefix + QLatin1Char('/') + relative)
                                      .toCaseFolded());
                addStagingParentDirectories(prefix, relative, expected);
            }
        }
        m_expectedMembers = std::move(expected);
        return true;
    }

private:
    QString m_root;
    qbrowser_archive_detail::WindowsStableDirectoryTree &m_tree;
    QSet<QString> m_expectedMembers;
};
#endif

InstallResult failure(const InstallPhase phase,
                      const InstallError error,
                      const QString &stableError)
{
    return {phase, error, stableError, {}, {}, {}, {}, {}, std::nullopt};
}

const ArchiveFile *findFile(const QVector<ArchiveFile> &files, const QByteArray &path)
{
    const auto found = std::find_if(
        files.cbegin(), files.cend(), [&path](const ArchiveFile &file) {
            return file.path == path;
        });
    return found == files.cend() ? nullptr : &*found;
}

bool copyBounded(const QString &sourcePath,
                 const QString &destinationPath,
                 const quint64 maximumBytes)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly) || source.size() < 0
        || static_cast<quint64>(source.size()) > maximumBytes) {
        return false;
    }
    const QByteArray bytes = source.read(static_cast<qint64>(maximumBytes) + 1);
    if (bytes.size() != source.size()
        || static_cast<quint64>(bytes.size()) > maximumBytes) {
        return false;
    }
    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)
        || destination.write(bytes) != bytes.size()
        || !destination.commit()) {
        destination.cancelWriting();
        return false;
    }
    return true;
}

class EmptyStagingParentCleanup final
{
public:
    EmptyStagingParentCleanup(QString stagingParent,
                              const bool storeRootExisted,
                              const bool stagingParentExisted)
        : stagingParent_(std::move(stagingParent))
        , storeRoot_(QFileInfo(stagingParent_).dir().absolutePath())
        , storeRootExisted_(storeRootExisted)
        , stagingParentExisted_(stagingParentExisted)
    {
    }

    ~EmptyStagingParentCleanup()
    {
        if (!stagingParentExisted_) {
            (void)QDir().rmdir(stagingParent_);
        }
        if (!storeRootExisted_) {
            (void)QDir().rmdir(storeRoot_);
        }
    }

private:
    QString stagingParent_;
    QString storeRoot_;
    bool storeRootExisted_ = false;
    bool stagingParentExisted_ = false;
};

std::optional<QVersionNumber> strictRuntimeVersion(const QString &version)
{
    const qsizetype suffix = version.indexOf(QLatin1Char('-'));
    const QString core = suffix < 0 ? version : version.first(suffix);
    const QVersionNumber parsed = QVersionNumber::fromString(core);
    if (parsed.segmentCount() != 3 || parsed.toString() != core) {
        return std::nullopt;
    }
    return parsed;
}

bool runtimeIsCompatible(const QString &runtimeVersion,
                         const RuntimeCompatibility &compatibility)
{
    const std::optional<QVersionNumber> runtime = strictRuntimeVersion(runtimeVersion);
    const std::optional<QVersionNumber> minimum = strictRuntimeVersion(
        compatibility.minVersion);
    bool maximumMajorOk = false;
    const int maximumMajor = compatibility.maxVersion.first(
        compatibility.maxVersion.indexOf(QLatin1Char('.'))).toInt(&maximumMajorOk);
    return runtime.has_value() && minimum.has_value() && maximumMajorOk
        && QVersionNumber::compare(*runtime, *minimum) >= 0
        && runtime->majorVersion() <= maximumMajor;
}

bool importsAreAllowed(const QStringList &imports, const QSet<QString> &allowed)
{
    return std::ranges::all_of(imports, [&allowed](const QString &name) {
        return allowed.contains(name);
    });
}

bool sourcesPassPolicy(const QVector<ArchiveFile> &files)
{
    return std::ranges::all_of(files, [](const ArchiveFile &file) {
        if (!QmlSourcePolicy::isQmlSourcePath(file.path)) {
            return true;
        }
        return QmlSourcePolicy::violations(file.contents).isEmpty();
    });
}

bool sourceImportsAreAllowed(const QVector<ArchiveFile> &files,
                             const QStringList &declaredImports,
                             const QSet<QString> &allowedImports)
{
    return std::ranges::all_of(files, [&](const ArchiveFile &file) {
        if (!QmlSourcePolicy::isQmlSourcePath(file.path)) return true;
        return std::ranges::all_of(
            QmlSourcePolicy::staticImports(file.contents),
            [&](const QString &module) {
                return declaredImports.contains(module)
                    && allowedImports.contains(module);
            });
    });
}

bool looksLikePortableExecutable(const QByteArray &contents)
{
    constexpr qsizetype dosHeaderBytes = 0x40;
    constexpr qsizetype peOffsetField = 0x3c;
    if (contents.size() < dosHeaderBytes
        || !contents.startsWith(QByteArrayLiteral("MZ"))) {
        return false;
    }
    const quint32 peOffset = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(contents.constData() + peOffsetField));
    return peOffset >= static_cast<quint32>(dosHeaderBytes)
        && peOffset <= static_cast<quint32>(contents.size() - 4)
        && QByteArrayView(contents).sliced(static_cast<qsizetype>(peOffset), 4)
               == QByteArrayView("PE\0\0", 4);
}

bool packageMembersPassPolicy(const QVector<ArchiveFile> &files)
{
    static const QSet<QString> forbiddenExecutableExtensions{
        QStringLiteral("com"), QStringLiteral("cpl"), QStringLiteral("dll"),
        QStringLiteral("drv"), QStringLiteral("dylib"), QStringLiteral("exe"),
        QStringLiteral("ocx"), QStringLiteral("scr"), QStringLiteral("so"),
        QStringLiteral("sys")};
    return std::ranges::none_of(files, [](const ArchiveFile &file) {
        const QString suffix = QFileInfo(QString::fromUtf8(file.path))
                                   .suffix().toCaseFolded();
        const QByteArray &contents = file.contents;
        const bool signatureMetadata =
            file.path == QByteArrayLiteral("metadata/signature.ed25519");
        const bool executableMagic = !signatureMetadata
            && (looksLikePortableExecutable(contents)
                || contents.startsWith(QByteArrayLiteral("\x7f" "ELF")));
        return forbiddenExecutableExtensions.contains(suffix) || executableMagic;
    });
}

bool permissionsMatch(const ManifestPermissions &left,
                      const ManifestPermissions &right)
{
    return left.network.hosts == right.network.hosts
        && left.network.methods == right.network.methods
        && left.storage == right.storage
        && left.clipboardWrite == right.clipboardWrite
        && left.clipboardRead == right.clipboardRead
        && left.fileOpen == right.fileOpen;
}

bool validDigestHex(const QByteArray &digest)
{
    return digest.size() == 64
        && std::ranges::all_of(digest, [](const char value) {
               return (value >= '0' && value <= '9')
                   || (value >= 'a' && value <= 'f');
           });
}

bool sameCanonicalDirectory(const QString &expected,
                            const QString &leased)
{
    const QFileInfo expectedInfo(expected);
    const QFileInfo leasedInfo(leased);
    const QString expectedCanonical = expectedInfo.canonicalFilePath();
    const QString leasedCanonical = leasedInfo.canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    return expectedInfo.isDir() && !expectedInfo.isSymLink()
        && leasedInfo.isDir() && !leasedInfo.isSymLink()
        && !expectedCanonical.isEmpty() && !leasedCanonical.isEmpty()
        && expectedCanonical.compare(leasedCanonical, pathCase) == 0
        && QDir::cleanPath(leasedInfo.absoluteFilePath())
               .compare(QDir::cleanPath(leasedCanonical), pathCase)
            == 0;
}

InstallResult validateInstalledFiles(
    QVector<ArchiveFile> files,
    const QString &appId,
    const QString &versionDirectory,
    const QString &root,
    const QByteArray &trustedPublicKeyPem,
    const InstallPolicy &policy)
{
    std::ranges::sort(files, [](const ArchiveFile &left,
                               const ArchiveFile &right) {
        return qbrowser_archive_detail::archivePathBytewiseLess(
            left.path, right.path);
    });
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    const ArchiveFile *manifestFile = findFile(
        files, QByteArrayLiteral("manifest.json"));
    if (signatureFile == nullptr || signatureFile->contents.size() != 64
        || !signedDigest.hasValue()
        || !SignatureVerifier::verifyPem(
                signedDigest.bytes(), trustedPublicKeyPem,
                signatureFile->contents).isVerified()
        || !content.isValid() || manifestFile == nullptr) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    const ManifestParseResult parsed = Manifest::parse(manifestFile->contents);
    const QByteArray entryPoint = parsed.hasValue()
        ? parsed.value().entryPoint().toUtf8() : QByteArray{};
    if (!parsed.hasValue() || parsed.value().appId() != appId
        || !runtimeIsCompatible(policy.runtimeVersion, parsed.value().runtime())
        || !importsAreAllowed(parsed.value().imports(), policy.allowedImports)
        || !sourceImportsAreAllowed(files, parsed.value().imports(),
                                    policy.allowedImports)
        || !sourcesPassPolicy(files) || !packageMembersPassPolicy(files)
        || versionDirectory != parsed.value().version() + QLatin1Char('-')
               + QString::fromLatin1(content.digest().toHex())
        || findFile(files, entryPoint) == nullptr) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    return {InstallPhase::Complete, InstallError::None, {}, appId,
            parsed.value().version(), root, parsed.value().entryPoint(),
            parsed.value().permissions(), std::nullopt, {}};
}

std::optional<QString> authenticatedManifestAppId(
    const QString &packagePath,
    const QByteArray &trustedPublicKeyPem,
    const ArchiveLimits &limits)
{
    const ArchiveSnapshotResult snapshot = Archive::snapshot(packagePath, limits);
    if (!snapshot.hasValue()) return std::nullopt;
    const QVector<ArchiveFile> &files = snapshot.files();
    const ArchiveFile *signature = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    const ArchiveFile *manifest = findFile(
        files, QByteArrayLiteral("manifest.json"));
    const ContentDigestResult digest = ContentDigest::signedPackage(files);
    if (signature == nullptr || signature->contents.size() != 64
        || manifest == nullptr || !digest.hasValue()
        || !SignatureVerifier::verifyPem(
                digest.bytes(), trustedPublicKeyPem, signature->contents)
                .isVerified()) {
        return std::nullopt;
    }
    const ManifestParseResult parsed = Manifest::parse(manifest->contents);
    return parsed.hasValue()
        ? std::optional<QString>(parsed.value().appId())
        : std::nullopt;
}

QString immutableMemberKey(const QString &root,
                           const QString &path,
                           const bool directory)
{
    QString relative = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(path));
#ifdef Q_OS_WIN
    relative = relative.toCaseFolded();
#endif
    return (directory ? QStringLiteral("d:") : QStringLiteral("f:"))
        + relative;
}

int relativePathDepth(const QString &root, const QString &path)
{
    return QDir::fromNativeSeparators(QDir(root).relativeFilePath(path))
        .count(QLatin1Char('/'));
}

bool installedFileIsReadOnly(const QString &path)
{
#ifdef Q_OS_WIN
    const auto native = qbrowser_archive_detail::windowsApiPath(path);
    if (!native.has_value()) return false;
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(native->utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U
        && (attributes & FILE_ATTRIBUTE_READONLY) != 0U;
#else
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink()
        && !info.permission(QFileDevice::WriteOwner
                            | QFileDevice::WriteUser
                            | QFileDevice::WriteGroup
                            | QFileDevice::WriteOther);
#endif
}

#ifdef Q_OS_WIN
struct ImmutableDirectoryMembershipSeal final
{
    qbrowser_archive_detail::UniqueWindowsHandle handle;
    QByteArray originalSecurity;
    bool originalDaclProtected = false;
    bool active = false;

    ImmutableDirectoryMembershipSeal() = default;
    ~ImmutableDirectoryMembershipSeal();
    ImmutableDirectoryMembershipSeal(
        const ImmutableDirectoryMembershipSeal &) = delete;
    ImmutableDirectoryMembershipSeal &operator=(
        const ImmutableDirectoryMembershipSeal &) = delete;
    ImmutableDirectoryMembershipSeal(
        ImmutableDirectoryMembershipSeal &&other) noexcept
        : handle(std::move(other.handle))
        , originalSecurity(std::move(other.originalSecurity))
        , originalDaclProtected(other.originalDaclProtected)
        , active(std::exchange(other.active, false))
    {
    }
    ImmutableDirectoryMembershipSeal &operator=(
        ImmutableDirectoryMembershipSeal &&other) noexcept;

    void restore() noexcept;
};

bool directoryMembershipDenyPresent(
    PACL dacl,
    PSID sid,
    const ACCESS_MASK required)
{
    if (dacl == nullptr || sid == nullptr) return false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void *rawAce = nullptr;
        if (GetAce(dacl, index, &rawAce) == FALSE || rawAce == nullptr) {
            return false;
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        if (header->AceType != ACCESS_DENIED_ACE_TYPE) continue;
        const auto *ace = static_cast<const ACCESS_DENIED_ACE *>(rawAce);
        PSID aceSid = const_cast<DWORD *>(&ace->SidStart);
        if (EqualSid(aceSid, sid) != FALSE
            && (ace->Mask & required) == required) {
            return true;
        }
    }
    return false;
}

bool immutableDirectoryMembershipSealIsIntact(
    const ImmutableDirectoryMembershipSeal &seal)
{
    if (!seal.active || !seal.handle.isValid()) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(
            seal.handle.get(), FileAttributeTagInfo, &attributes,
            static_cast<DWORD>(sizeof(attributes))) == FALSE
        || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security = GetSecurityInfo(
        seal.handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (security != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    BYTE worldBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD worldBytes = sizeof(worldBuffer);
    BYTE ownerRightsBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD ownerRightsBytes = sizeof(ownerRightsBuffer);
    constexpr ACCESS_MASK directoryMutationRights = FILE_ADD_FILE
        | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES
        | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    const bool intact = GetSecurityDescriptorControl(
                            descriptor, &control, &revision) != FALSE
        && (control & SE_DACL_PROTECTED) != 0
        && CreateWellKnownSid(
               WinWorldSid, nullptr, worldBuffer, &worldBytes) != FALSE
        && CreateWellKnownSid(
               WinCreatorOwnerRightsSid, nullptr, ownerRightsBuffer,
               &ownerRightsBytes) != FALSE
        && directoryMembershipDenyPresent(
               dacl, worldBuffer, directoryMutationRights)
        && directoryMembershipDenyPresent(
               dacl, ownerRightsBuffer, WRITE_DAC | WRITE_OWNER);
    LocalFree(descriptor);
    return intact;
}

void ImmutableDirectoryMembershipSeal::restore() noexcept
{
    if (!active || !handle.isValid() || originalSecurity.isEmpty()) return;
    auto *descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        originalSecurity.data());
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    if (GetSecurityDescriptorDacl(
            descriptor, &daclPresent, &dacl, &daclDefaulted) != FALSE) {
        const SECURITY_INFORMATION protection = originalDaclProtected
            ? PROTECTED_DACL_SECURITY_INFORMATION
            : UNPROTECTED_DACL_SECURITY_INFORMATION;
        (void)SetSecurityInfo(
            handle.get(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | protection,
            nullptr, nullptr, daclPresent != FALSE ? dacl : nullptr, nullptr);
    }
    active = false;
    originalSecurity.clear();
}

ImmutableDirectoryMembershipSeal::~ImmutableDirectoryMembershipSeal()
{
    restore();
}

ImmutableDirectoryMembershipSeal &
ImmutableDirectoryMembershipSeal::operator=(
    ImmutableDirectoryMembershipSeal &&other) noexcept
{
    if (this != &other) {
        restore();
        handle = std::move(other.handle);
        originalSecurity = std::move(other.originalSecurity);
        originalDaclProtected = other.originalDaclProtected;
        active = std::exchange(other.active, false);
    }
    return *this;
}

bool sealImmutableDirectoryMembership(
    const QString &path,
    const qbrowser_archive_detail::WindowsStableDirectoryTree &tree,
    ImmutableDirectoryMembershipSeal &seal)
{
    const auto native = qbrowser_archive_detail::windowsApiPath(path);
    if (!native.has_value() || !tree.contains(path) || !tree.isStable()) {
        return false;
    }
    seal.handle = qbrowser_archive_detail::UniqueWindowsHandle(CreateFileW(
        reinterpret_cast<LPCWSTR>(native->utf16()),
        READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!seal.handle.isValid()
        || GetFileInformationByHandleEx(
               seal.handle.get(), FileAttributeTagInfo, &attributes,
               static_cast<DWORD>(sizeof(attributes))) == FALSE
        || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || !tree.isStable()) {
        return false;
    }

    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security = GetSecurityInfo(
        seal.handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &existingDacl, nullptr, &descriptor);
    if (security != ERROR_SUCCESS || descriptor == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const DWORD descriptorBytes = GetSecurityDescriptorLength(descriptor);
    if (descriptorBytes == 0
        || GetSecurityDescriptorControl(
               descriptor, &control, &revision) == FALSE) {
        LocalFree(descriptor);
        return false;
    }
    seal.originalSecurity = QByteArray(
        static_cast<const char *>(descriptor),
        static_cast<qsizetype>(descriptorBytes));
    seal.originalDaclProtected = (control & SE_DACL_PROTECTED) != 0;

    BYTE worldBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD worldBytes = sizeof(worldBuffer);
    BYTE ownerRightsBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD ownerRightsBytes = sizeof(ownerRightsBuffer);
    if (CreateWellKnownSid(
            WinWorldSid, nullptr, worldBuffer, &worldBytes) == FALSE
        || CreateWellKnownSid(
               WinCreatorOwnerRightsSid, nullptr, ownerRightsBuffer,
               &ownerRightsBytes) == FALSE) {
        LocalFree(descriptor);
        seal.originalSecurity.clear();
        return false;
    }

    constexpr ACCESS_MASK directoryMutationRights = FILE_ADD_FILE
        | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES
        | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    EXPLICIT_ACCESSW deny[2]{};
    deny[0].grfAccessPermissions = directoryMutationRights;
    deny[1].grfAccessPermissions = WRITE_DAC | WRITE_OWNER;
    for (EXPLICIT_ACCESSW &entry : deny) {
        entry.grfAccessMode = DENY_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    }
    deny[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(worldBuffer);
    deny[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(ownerRightsBuffer);
    PACL sealedDacl = nullptr;
    const DWORD acl = SetEntriesInAclW(
        static_cast<ULONG>(std::size(deny)), deny, existingDacl, &sealedDacl);
    const DWORD applied = acl == ERROR_SUCCESS && sealedDacl != nullptr
        ? SetSecurityInfo(
              seal.handle.get(), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION
                  | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, sealedDacl, nullptr)
        : acl;
    if (sealedDacl != nullptr) LocalFree(sealedDacl);
    LocalFree(descriptor);
    if (applied != ERROR_SUCCESS) {
        seal.originalSecurity.clear();
        return false;
    }
    seal.active = true;
    return immutableDirectoryMembershipSealIsIntact(seal)
        && tree.isStable();
}
#endif
}

struct ImmutablePackageGuard::State final
{
    QString appId;
    QString versionDirectory;
    QString packageDirectory;
    QVector<ArchiveFile> files;
    QSet<QString> memberKeys;
#ifdef Q_OS_WIN
    struct LockedFile final
    {
        QString path;
        qbrowser_archive_detail::WindowsStableFile file;
    };

    qbrowser_archive_detail::WindowsStableDirectoryTree tree;
    mutable std::vector<LockedFile> lockedFiles;
    std::vector<ImmutableDirectoryMembershipSeal> directorySeals;
#endif

    ~State()
    {
#ifdef Q_OS_WIN
        for (auto seal = directorySeals.rbegin();
             seal != directorySeals.rend(); ++seal) {
            seal->restore();
        }
#endif
    }
};

ImmutablePackageGuard::ImmutablePackageGuard(std::unique_ptr<State> state)
    : state_(std::move(state))
{
}

ImmutablePackageGuard::~ImmutablePackageGuard() = default;

PackageInstaller::PackageInstaller(PackageStore &store,
                                   QByteArray trustedPublicKeyPem,
                                   InstallPolicy policy)
    : m_store(store)
    , m_trustedPublicKeyPem(std::move(trustedPublicKeyPem))
    , m_policy(std::move(policy))
{
}

InstallResult PackageInstaller::verifyInstalled(
    const QString &appId,
    const QString &versionDirectory) const
{
    const QString root = m_store.versionPath(appId, versionDirectory);
    const QFileInfo rootInfo(root);
    if (root.isEmpty() || !rootInfo.isDir() || rootInfo.isSymLink()) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    QVector<ArchiveFile> files;
    quint64 totalBytes = 0;
    quint64 scannedMembers = 0;
    const quint64 maximumScannedMembers =
        m_policy.archiveLimits.maximumEntries
                > std::numeric_limits<quint64>::max() / 64
        ? std::numeric_limits<quint64>::max()
        : m_policy.archiveLimits.maximumEntries * 64;
    QDirIterator iterator(root,
                          QDir::AllEntries | QDir::Hidden | QDir::System
                              | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (scannedMembers >= maximumScannedMembers) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        ++scannedMembers;
        if (info.isSymLink()) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        if (info.isDir()) continue;
        if (!info.isFile()
            || static_cast<quint64>(files.size()) >= m_policy.archiveLimits.maximumEntries
            || info.size() < 0
            || static_cast<quint64>(info.size()) > m_policy.archiveLimits.maximumEntryBytes) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        const QByteArray relative = QDir(root).relativeFilePath(path).toUtf8();
        const quint64 fileBytes = static_cast<quint64>(info.size());
        if (!qbrowser_archive_detail::validateArchivePath(
                 relative, m_policy.archiveLimits).has_value()
            || fileBytes > m_policy.archiveLimits.maximumTotalBytes
            || totalBytes > m_policy.archiveLimits.maximumTotalBytes
                   - fileBytes) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        const QByteArray bytes = file.read(info.size() + 1);
        if (bytes.size() != info.size()) {
            return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                           QStringLiteral("installed_content_invalid"));
        }
        totalBytes += static_cast<quint64>(bytes.size());
        files.push_back({relative, bytes});
    }
    InstallResult verified = validateInstalledFiles(
        std::move(files), appId, versionDirectory, root,
        m_trustedPublicKeyPem, m_policy);
    if (!verified.succeeded()) return verified;
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled(appId, versionDirectory);
    }
#endif
    return verified;
}

InstallResult PackageInstaller::verifyInstalledImmutable(
    const QString &appId,
    const QString &versionDirectory,
    std::shared_ptr<const ImmutablePackageGuard> retainedGuard) const
{
    const auto invalid = [] {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    };
    const QString root = m_store.versionPath(appId, versionDirectory);
    const QFileInfo rootInfo(root);
    if (root.isEmpty() || !rootInfo.isDir() || rootInfo.isSymLink()) {
        return invalid();
    }

    if (retainedGuard == nullptr) {
        auto state = std::make_unique<ImmutablePackageGuard::State>();
        state->appId = appId;
        state->versionDirectory = versionDirectory;
        state->packageDirectory = root;
#ifdef Q_OS_WIN
        const QString versionParent = QFileInfo(root).dir().absolutePath();
        if (!state->tree.openSharedRoot(versionParent)
            || !state->tree.addImmutableDirectory(root)) {
            return invalid();
        }
#endif
        QStringList directories;
        QStringList filePaths;
        quint64 scannedMembers = 0;
        const quint64 maximumScannedMembers =
            m_policy.archiveLimits.maximumEntries
                    > std::numeric_limits<quint64>::max() / 64
            ? std::numeric_limits<quint64>::max()
            : m_policy.archiveLimits.maximumEntries * 64;
        QSet<QString> collisionKeys;
        QDirIterator iterator(
            root,
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (scannedMembers >= maximumScannedMembers || info.isSymLink()) {
                return invalid();
            }
            ++scannedMembers;
            const QByteArray relative = QDir::fromNativeSeparators(
                QDir(root).relativeFilePath(path)).toUtf8();
            const auto checked = qbrowser_archive_detail::validateArchivePath(
                relative, m_policy.archiveLimits);
            if (!checked.has_value()
                || collisionKeys.contains(checked->collisionKey)) {
                return invalid();
            }
            collisionKeys.insert(checked->collisionKey);
            if (info.isDir()) {
                directories.push_back(path);
                state->memberKeys.insert(
                    immutableMemberKey(root, path, true));
                continue;
            }
            if (!info.isFile()
                || static_cast<quint64>(filePaths.size())
                       >= m_policy.archiveLimits.maximumEntries
                || info.size() < 0
                || static_cast<quint64>(info.size())
                       > m_policy.archiveLimits.maximumEntryBytes) {
                return invalid();
            }
            filePaths.push_back(path);
            state->memberKeys.insert(immutableMemberKey(root, path, false));
        }
        std::ranges::sort(directories, [&root](const QString &left,
                                               const QString &right) {
            const int leftDepth = relativePathDepth(root, left);
            const int rightDepth = relativePathDepth(root, right);
            return leftDepth != rightDepth
                ? leftDepth < rightDepth : left < right;
        });
#ifdef Q_OS_WIN
        for (const QString &directory : directories) {
            if (!state->tree.addImmutableDirectory(directory)) {
                return invalid();
            }
        }
#endif
        quint64 totalBytes = 0;
        for (const QString &path : filePaths) {
            const QByteArray relative = QDir::fromNativeSeparators(
                QDir(root).relativeFilePath(path)).toUtf8();
            QByteArray bytes;
#ifdef Q_OS_WIN
            qbrowser_archive_detail::WindowsStableFile locked;
            if (!locked.openReadLocked(path, state->tree)
                || !locked.hasSingleLink()
                || !locked.isStableWithin(state->tree)
                || !locked.isSameIdentityAt(path)
                || !installedFileIsReadOnly(path)
                || locked.readBoundedIncludingEmpty(
                       m_policy.archiveLimits.maximumEntryBytes, bytes)
                       != qbrowser_archive_detail::WindowsStableReadStatus::Read) {
                return invalid();
            }
            state->lockedFiles.push_back(
                {path, std::move(locked)});
#else
            QFile file(path);
            if (!installedFileIsReadOnly(path)
                || !file.open(QIODevice::ReadOnly)
                || file.size() < 0
                || static_cast<quint64>(file.size())
                       > m_policy.archiveLimits.maximumEntryBytes) {
                return invalid();
            }
            bytes = file.read(static_cast<qint64>(
                m_policy.archiveLimits.maximumEntryBytes) + 1);
            if (bytes.size() != file.size()) return invalid();
#endif
            const quint64 fileBytes = static_cast<quint64>(bytes.size());
            if (fileBytes > m_policy.archiveLimits.maximumTotalBytes
                || totalBytes > m_policy.archiveLimits.maximumTotalBytes
                       - fileBytes) {
                return invalid();
            }
            totalBytes += static_cast<quint64>(bytes.size());
            state->files.push_back({relative, std::move(bytes)});
        }
#ifdef Q_OS_WIN
        QStringList directoriesToSeal{root};
        directoriesToSeal.append(directories);
        state->directorySeals.reserve(
            static_cast<size_t>(directoriesToSeal.size()));
        for (const QString &directory : directoriesToSeal) {
            ImmutableDirectoryMembershipSeal seal;
            if (!sealImmutableDirectoryMembership(
                    directory, state->tree, seal)) {
                return invalid();
            }
            state->directorySeals.push_back(std::move(seal));
        }
#endif
        retainedGuard = std::shared_ptr<const ImmutablePackageGuard>(
            new ImmutablePackageGuard(std::move(state)));
    }

    if (retainedGuard->state_ == nullptr) return invalid();
    const ImmutablePackageGuard::State &state = *retainedGuard->state_;
    if (state.appId != appId || state.versionDirectory != versionDirectory
        || !sameCanonicalDirectory(state.packageDirectory, root)) {
        return invalid();
    }
#ifdef Q_OS_WIN
    const size_t expectedDirectorySeals = 1U
        + static_cast<size_t>(std::ranges::count_if(
            state.memberKeys, [](const QString &key) {
                return key.startsWith(QStringLiteral("d:"));
            }));
    if (!state.tree.isStable()
        || state.lockedFiles.size()
               != static_cast<size_t>(state.files.size())
        || state.directorySeals.size() != expectedDirectorySeals
        || !std::ranges::all_of(
            state.directorySeals,
            immutableDirectoryMembershipSealIsIntact)) {
        return invalid();
    }
    QVector<ArchiveFile> authenticatedFiles;
    authenticatedFiles.reserve(state.files.size());
    for (size_t index = 0; index < state.lockedFiles.size(); ++index) {
        auto &locked = state.lockedFiles.at(index);
        QByteArray bytes;
        if (!locked.file.hasSingleLink()
            || !locked.file.isStableWithin(state.tree)
            || !locked.file.isSameIdentityAt(locked.path)
            || !installedFileIsReadOnly(locked.path)
            || locked.file.readBoundedIncludingEmpty(
                   m_policy.archiveLimits.maximumEntryBytes, bytes)
                   != qbrowser_archive_detail::WindowsStableReadStatus::Read) {
            return invalid();
        }
        authenticatedFiles.push_back(
            {state.files.at(static_cast<qsizetype>(index)).path,
             std::move(bytes)});
    }
#else
    const QVector<ArchiveFile> &authenticatedFiles = state.files;
#endif
    QSet<QString> currentMembers;
    quint64 currentScannedMembers = 0;
    const quint64 maximumCurrentMembers =
        m_policy.archiveLimits.maximumEntries
                > std::numeric_limits<quint64>::max() / 64
        ? std::numeric_limits<quint64>::max()
        : m_policy.archiveLimits.maximumEntries * 64;
    QDirIterator current(
        root,
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (current.hasNext()) {
        const QString path = current.next();
        const QFileInfo info = current.fileInfo();
        if (currentScannedMembers >= maximumCurrentMembers
            || info.isSymLink() || (!info.isDir() && !info.isFile())) {
            return invalid();
        }
        ++currentScannedMembers;
#ifdef Q_OS_WIN
        if (info.isDir() && !state.tree.contains(path)) return invalid();
#endif
        currentMembers.insert(immutableMemberKey(root, path, info.isDir()));
    }
    if (currentMembers != state.memberKeys) return invalid();

    InstallResult verified = validateInstalledFiles(
        authenticatedFiles, appId, versionDirectory, root,
        m_trustedPublicKeyPem, m_policy);
    if (!verified.succeeded()) return verified;
    verified.immutableGuard = std::move(retainedGuard);
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterVerifyInstalled(appId, versionDirectory);
    }
#endif
    return verified;
}

InstallResult PackageInstaller::reverifyInstalledVersion(
    const QString &appId,
    const ActivationBinding &expected,
    std::shared_ptr<const ImmutablePackageGuard> retainedGuard) const
{
    const qsizetype separator = expected.currentDirectory.lastIndexOf(
        QLatin1Char('-'));
    if (separator <= 0
        || expected.currentDirectory.sliced(separator + 1).toLatin1()
            != expected.versionDigestHex) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    const PackageStoreResult active = m_store.compareCurrent(appId, expected);
    if (!active.succeeded()) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    InstallResult verified = verifyInstalledImmutable(
        appId, expected.currentDirectory, std::move(retainedGuard));
    if (!verified.succeeded()) return verified;
    const PackageStoreResult rebound = m_store.compareCurrent(appId, expected);
    if (!rebound.succeeded()) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    verified.activationBinding = expected;
    return verified;
}

InstallResult PackageInstaller::reverifyPinnedLease(
    const VerifiedPackageLease &lease,
    std::shared_ptr<const ImmutablePackageGuard> retainedGuard) const
{
    const QString expectedDirectory = m_store.versionPath(
        lease.appId, lease.versionDirectory);
    if (lease.appId.isEmpty() || lease.version.isEmpty()
        || lease.entryPoint.isEmpty() || !validDigestHex(lease.digestHex)
        || lease.activationGenerationAtIssue <= 0
        || lease.leaseAuthorityEpoch == 0
        || lease.versionDirectory
               != lease.version + QLatin1Char('-')
                    + QString::fromLatin1(lease.digestHex)
        || expectedDirectory.isEmpty()
        || !sameCanonicalDirectory(expectedDirectory,
                                   lease.packageDirectory)) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }

    InstallResult verified = verifyInstalledImmutable(
        lease.appId, lease.versionDirectory, std::move(retainedGuard));
    if (!verified.succeeded() || verified.appId != lease.appId
        || verified.version != lease.version
        || verified.entryPoint != lease.entryPoint
        || !sameCanonicalDirectory(verified.path,
                                   lease.packageDirectory)
        || !permissionsMatch(verified.permissions, lease.permissions)) {
        return failure(InstallPhase::Verify, InstallError::ContentInvalid,
                       QStringLiteral("installed_content_invalid"));
    }
    verified.activationBinding = ActivationBinding{
        lease.versionDirectory,
        lease.digestHex,
        lease.activationGenerationAtIssue};
    return verified;
}

InstallResult PackageInstaller::install(const QString &packagePath) const
{
    const std::optional<QString> authenticatedAppId = authenticatedManifestAppId(
        packagePath, m_trustedPublicKeyPem, m_policy.archiveLimits);
    if (!m_policy.expectedAppId.isEmpty() && authenticatedAppId.has_value()
        && *authenticatedAppId != m_policy.expectedAppId) {
        return failure(InstallPhase::Verify, InstallError::AppIdMismatch,
                       QStringLiteral("app_id_mismatch"));
    }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterAppIdPrecheckBeforeSourceCopy) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .afterAppIdPrecheckBeforeSourceCopy(packagePath);
    }
#endif
    const QString stagingParent = m_store.root() + QStringLiteral("/.staging");
    const bool storeRootExisted = QFileInfo::exists(m_store.root());
    const bool stagingParentExisted = QFileInfo::exists(stagingParent);
    if (!QDir().mkpath(stagingParent)) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    EmptyStagingParentCleanup emptyParentCleanup(
        stagingParent, storeRootExisted, stagingParentExisted);
    const QFileInfo stagingInfo(stagingParent);
    if (!stagingInfo.isDir() || stagingInfo.isSymLink()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    QTemporaryDir staging(stagingParent + QStringLiteral("/install-XXXXXX"));
    if (!staging.isValid()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree stagingTree;
    if (!stagingTree.openRoot(staging.path()) || !stagingTree.isStable()) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }
    staging.setAutoRemove(false);
    WindowsStagingCleanup stagingCleanup(staging.path(), stagingTree);
#endif
    const QString stagedPackage = staging.filePath(QStringLiteral("candidate.qapkg"));
    if (!copyBounded(packagePath,
                     stagedPackage,
                     m_policy.archiveLimits.maximumArchiveBytes)) {
        return failure(InstallPhase::Staging,
                       InstallError::StagingFailed,
                       QStringLiteral("staging_failed"));
    }

    const ArchiveSnapshotResult snapshot = Archive::snapshot(
        stagedPackage, m_policy.archiveLimits);
    if (!snapshot.hasValue()) {
        return failure(InstallPhase::Verify,
                       InstallError::ArchiveInvalid,
                       QStringLiteral("archive_invalid"));
    }
    const QVector<ArchiveFile> &files = snapshot.files();
#ifdef Q_OS_WIN
    if (!stagingCleanup.setAuthenticatedSnapshot(files, m_policy.archiveLimits)) {
        return failure(InstallPhase::Verify,
                       InstallError::ArchiveInvalid,
                       QStringLiteral("archive_invalid"));
    }
#endif
    const ArchiveFile *signatureFile = findFile(
        files, QByteArrayLiteral("metadata/signature.ed25519"));
    if (signatureFile == nullptr) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureMissing,
                       QStringLiteral("signature_missing"));
    }
    if (signatureFile->contents.size() != 64) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureInvalid,
                       QStringLiteral("signature_invalid"));
    }
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    if (!signedDigest.hasValue()
        || !SignatureVerifier::verifyPem(signedDigest.bytes(),
                                         m_trustedPublicKeyPem,
                                         signatureFile->contents)
                .isVerified()) {
        return failure(InstallPhase::Verify,
                       InstallError::SignatureInvalid,
                       QStringLiteral("signature_invalid"));
    }
    const ContentDigestValidation content = ContentDigest::validatePayload(files);
    if (!content.isValid()) {
        return failure(InstallPhase::Verify,
                       InstallError::ContentInvalid,
                       QStringLiteral("content_invalid"));
    }
    const ArchiveFile *manifestFile = findFile(files, QByteArrayLiteral("manifest.json"));
    if (manifestFile == nullptr) {
        return failure(InstallPhase::Verify,
                       InstallError::ManifestInvalid,
                       QStringLiteral("manifest_invalid"));
    }
    const ManifestParseResult parsed = Manifest::parse(manifestFile->contents);
    if (!parsed.hasValue()) {
        return failure(InstallPhase::Verify,
                       InstallError::ManifestInvalid,
                       QStringLiteral("manifest_invalid"));
    }
    const Manifest &manifest = parsed.value();
    if (!m_policy.expectedAppId.isEmpty()
        && manifest.appId() != m_policy.expectedAppId) {
        return failure(InstallPhase::Verify, InstallError::AppIdMismatch,
                       QStringLiteral("app_id_mismatch"));
    }
    if (!runtimeIsCompatible(m_policy.runtimeVersion, manifest.runtime())) {
        return failure(InstallPhase::Verify,
                       InstallError::RuntimeIncompatible,
                       QStringLiteral("runtime_incompatible"));
    }
    if (!importsAreAllowed(manifest.imports(), m_policy.allowedImports)) {
        return failure(InstallPhase::Verify,
                       InstallError::ImportDenied,
                       QStringLiteral("import_denied"));
    }
    if (!sourceImportsAreAllowed(files, manifest.imports(),
                                 m_policy.allowedImports)) {
        return failure(InstallPhase::Preflight,
                       InstallError::ImportDenied,
                       QStringLiteral("import_denied"));
    }
    if (!sourcesPassPolicy(files) || !packageMembersPassPolicy(files)) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("source_policy_rejected"));
    }

    const QString preflightRoot = staging.filePath(QStringLiteral("preflight"));
    if (!QDir().mkdir(preflightRoot)
        || !Archive::extractFiles(files, preflightRoot, m_policy.archiveLimits)
                .hasValue()) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }
    const QFileInfo entryPoint(
        preflightRoot + QLatin1Char('/') + manifest.entryPoint());
    if (!entryPoint.isFile() || entryPoint.isSymLink()
        || !m_policy.preflight
        || !m_policy.preflight(manifest, preflightRoot)) {
        return failure(InstallPhase::Preflight,
                       InstallError::PreflightRejected,
                       QStringLiteral("preflight_rejected"));
    }

    const QString candidateRoot = staging.filePath(QStringLiteral("candidate"));
    if (!QDir().mkdir(candidateRoot)
        || !Archive::extractFiles(files, candidateRoot, m_policy.archiveLimits)
                .hasValue()) {
        return failure(InstallPhase::Candidate,
                       InstallError::CandidateFailed,
                       QStringLiteral("candidate_failed"));
    }
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidateCommit) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeCandidateCommit(candidateRoot);
    }
#endif

    const PackageStoreResult candidate = m_store.commitVerifiedCandidate(
        manifest.appId(),
        manifest.version(),
        content.digest().toHex(),
        candidateRoot,
        signedDigest.bytes(),
        signatureFile->contents,
        files,
        m_policy.archiveLimits);
    if (!candidate.succeeded()) {
        return failure(InstallPhase::Candidate,
                       InstallError::CandidateFailed,
                       QStringLiteral("candidate_failed"));
    }
    const QString versionDirectory = QFileInfo(candidate.path).fileName();
#ifdef Q_BROWSER_PACKAGE_INSTALLER_TESTING
    if (qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeActivate) {
        qbrowser_package_installer_testing::packageInstallerTestHooks()
            .beforeActivate(manifest.appId(), versionDirectory);
    }
#endif
    const PackageStoreResult activated = m_store.activateVerified(
        manifest.appId(), versionDirectory);
    if (!activated.succeeded()) {
        InstallResult result = failure(InstallPhase::Activate,
                                       InstallError::ActivationFailed,
                                       QStringLiteral("activation_failed"));
        result.appId = manifest.appId();
        result.version = manifest.version();
        result.path = candidate.path;
        return result;
    }
    if (!activated.activationBinding.has_value()) {
        return failure(InstallPhase::Activate, InstallError::ActivationFailed,
                       QStringLiteral("activation_failed"));
    }
    InstallResult installed = reverifyInstalledVersion(
        manifest.appId(), *activated.activationBinding);
    installed.immutableGuard.reset();
    return installed;
}
