#include "Archive.h"
#include "ArchiveTestHooks.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageInstallerTestHooks.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>
#include <utility>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

namespace
{
#ifdef Q_OS_WIN
bool enableCaseSensitiveDirectory(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE directory = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = SetFileInformationByHandle(
                             directory,
                             FileCaseSensitiveInfo,
                             &info,
                             sizeof(info))
        != FALSE;
    CloseHandle(directory);
    return enabled;
}
#endif

class PackageTemporaryDir final : public QTemporaryDir
{
public:
    ~PackageTemporaryDir()
    {
#ifdef Q_OS_WIN
        if (!isValid()) {
            return;
        }
        QStringList paths{path()};
        QDirIterator iterator(
            path(),
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            paths.push_back(iterator.next());
        }
        for (const QString &entry : paths) {
            const auto extended = qbrowser_archive_detail::windowsApiPath(entry);
            if (!extended) {
                continue;
            }
            QString native = *extended;
            (void)SetNamedSecurityInfoW(
                reinterpret_cast<LPWSTR>(native.data()),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                nullptr,
                nullptr);
            const auto *nativePath = reinterpret_cast<LPCWSTR>(native.utf16());
            const DWORD attributes = GetFileAttributesW(nativePath);
            if (attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
                (void)SetFileAttributesW(
                    nativePath, attributes & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        (void)QDir(path()).removeRecursively();
        setAutoRemove(false);
#endif
    }
};

QByteArray manifest(const QString &version,
                    const QString &minimumRuntime = QStringLiteral("1.0.0"),
                    const QString &maximumRuntime = QStringLiteral("1.x"),
                    const QStringList &imports = {QStringLiteral("QtQuick")},
                    const QString &entryPoint = QStringLiteral("qml/Main.qml"),
                    const QString &appId = QStringLiteral("company.pilot"))
{
    QJsonArray importArray;
    for (const QString &name : imports) {
        importArray.append(name);
    }
    const QJsonObject object{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("appId"), appId},
        {QStringLiteral("version"), version},
        {QStringLiteral("entryPoint"), entryPoint},
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
                      const bool corruptSignature = false,
                      QByteArray mainQml = QByteArrayLiteral("import QtQuick\nItem {}"),
                      QVector<ArchiveFile> extraFiles = {},
                      const QByteArray &entryPoint = QByteArrayLiteral("qml/Main.qml"))
{
    QVector<ArchiveFile> files{
        {QByteArrayLiteral("manifest.json"), std::move(manifestBytes)},
        {entryPoint, std::move(mainQml)}};
    files.append(std::move(extraFiles));
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
    result.expectedAppId = QStringLiteral("company.pilot");
    result.runtimeVersion = QStringLiteral("1.2.0");
    result.allowedImports = {QStringLiteral("QtQuick"), QStringLiteral("Company.Design")};
    result.preflight = [](const Manifest &, const QString &) { return true; };
    return result;
}

VerifiedPackageLease leaseFor(const InstallResult &installed,
                              const quint64 authorityEpoch)
{
    Q_ASSERT(installed.activationBinding.has_value());
    const ActivationBinding &binding = *installed.activationBinding;
    return {installed.appId,
            installed.version,
            binding.currentDirectory,
            installed.path,
            installed.entryPoint,
            installed.permissions,
            binding.versionDigestHex,
            binding.generation,
            authorityEpoch};
}

bool makeInstalledFileWritable(const QString &path)
{
#ifdef Q_OS_WIN
    if (SetNamedSecurityInfoW(
            const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto *const nativePath = reinterpret_cast<LPCWSTR>(path.utf16());
    const DWORD attributes = GetFileAttributesW(nativePath);
    if (attributes == INVALID_FILE_ATTRIBUTES
        || ((attributes & FILE_ATTRIBUTE_READONLY) != 0U
            && SetFileAttributesW(nativePath,
                                  attributes & ~FILE_ATTRIBUTE_READONLY)
                   == FALSE)) {
        return false;
    }
#endif
    return QFile::setPermissions(path,
                                 QFileDevice::ReadOwner
                                     | QFileDevice::WriteOwner);
}

#ifdef Q_OS_WIN
struct PackageMembershipMutationAttempt final
{
    bool rootAclCleared = false;
    bool nestedAclCleared = false;
    bool rootFileCreated = false;
    bool nestedFileCreated = false;
    bool rootDirectoryCreated = false;
    bool nestedDirectoryCreated = false;
};

bool clearInstalledDirectoryDacl(const QString &path)
{
    return SetNamedSecurityInfoW(
               const_cast<LPWSTR>(
                   reinterpret_cast<LPCWSTR>(path.utf16())),
               SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION
                   | UNPROTECTED_DACL_SECURITY_INFORMATION,
               nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool createNewInstalledFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write("late member") == qint64(11);
}

PackageMembershipMutationAttempt attemptPackageMembershipMutation(
    const QString &root,
    const QString &nested)
{
    PackageMembershipMutationAttempt result;
    result.rootAclCleared = clearInstalledDirectoryDacl(root);
    result.nestedAclCleared = clearInstalledDirectoryDacl(nested);
    result.rootFileCreated = createNewInstalledFile(
        QDir(root).filePath(QStringLiteral("late-root.qml")));
    result.nestedFileCreated = createNewInstalledFile(
        QDir(nested).filePath(QStringLiteral("late-nested.qml")));
    result.rootDirectoryCreated = QDir(root).mkdir(
        QStringLiteral("late-root-directory"));
    result.nestedDirectoryCreated = QDir(nested).mkdir(
        QStringLiteral("late-nested-directory"));
    return result;
}
#endif
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
    void rejectsSignedUnsafeQmlWithoutChangingCurrent();
    void preflightMutationCannotEnterCommittedVersion();
    void changedCandidateFailsBeforeActivation();
    void changedSignatureFailsBeforeActivation();
    void archiveEntryLimitAcceptsMaximumAndRejectsMaximumPlusOne();
    void injectedCandidateMembersKeepCleanupHandleCountBounded();
    void caseFoldedRaceMembersFailBeforePublication();
    void activationFailureDoesNotChangeCurrent();
    void postVerificationNewMemberCannotPublishUnderOldDigest();
    void postVerificationReplacementCannotPublishUnderOldDigest();
    void unexpectedEmptyDirectoryFailsCandidate();
    void publicationRaceRestoresOwnedStagingCleanup();
    void reverifyRejectsMismatchedActivationGeneration();
    void reverifyRejectsActivationChangedDuringSnapshot();
    void reverifyPinnedSignedVersionAfterCurrentChanges();
    void rejectTamperedPinnedVersion();
    void rejectWritableButUnchangedPinnedVersion();
    void pinnedGuardLocksIdentityAndRejectsMemberJunction();
    void pinnedGuardFreezesMembershipUntilReleased();
    void rejectPinnedPathOrDigestMismatch();
    void installsReverifiesAndActivatesEntryBeyondWindowsMaxPath();
    void rejectsAuthenticatedOtherAppBeforeStoreMutation();
    void rejectsAuthenticatedAppSwappedAfterPrecheckWithoutStoreMutation();
};

void PackageInstallerTest::reverifyPinnedSignedVersionAfterCurrentChanges()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-first"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    QVERIFY(first.activationBinding.has_value());
    const VerifiedPackageLease lease = leaseFor(first, 41);

    const InstallResult second = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-current"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0"))));
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             second.path);
    QVERIFY(!installer.reverifyInstalledVersion(
                           lease.appId, *first.activationBinding)
                 .succeeded());

    const InstallResult reverified = installer.reverifyPinnedLease(lease);
    QVERIFY2(reverified.succeeded(), qPrintable(reverified.stableError));
    QCOMPARE(reverified.appId, lease.appId);
    QCOMPARE(reverified.version, lease.version);
    QCOMPARE(reverified.path, lease.packageDirectory);
    QCOMPARE(reverified.entryPoint, lease.entryPoint);
    QVERIFY(reverified.activationBinding.has_value());
    QCOMPARE(reverified.activationBinding->currentDirectory,
             lease.versionDirectory);
    QCOMPARE(reverified.activationBinding->versionDigestHex, lease.digestHex);
    QCOMPARE(reverified.activationBinding->generation,
             lease.activationGenerationAtIssue);
}

void PackageInstallerTest::rejectTamperedPinnedVersion()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-tamper"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    const VerifiedPackageLease lease = leaseFor(installed, 42);
    const QString entryPath = QDir(installed.path).filePath(installed.entryPoint);
    installed.immutableGuard.reset();
    QVERIFY(makeInstalledFileWritable(entryPath));
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(entry.write("tampered pinned contents"), qint64(24));
    entry.close();

    const InstallResult rejected = installer.reverifyPinnedLease(lease);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.error, InstallError::ContentInvalid);
    QCOMPARE(rejected.stableError, QStringLiteral("installed_content_invalid"));
}

void PackageInstallerTest::rejectWritableButUnchangedPinnedVersion()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-writable"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    const VerifiedPackageLease lease = leaseFor(installed, 44);
    const QString entryPath = QDir(installed.path).filePath(installed.entryPoint);
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = entry.readAll();
    entry.close();
    QVERIFY(makeInstalledFileWritable(entryPath));
    QVERIFY(entry.open(QIODevice::ReadOnly));
    QCOMPARE(entry.readAll(), originalBytes);
    entry.close();

    const InstallResult rejected = installer.reverifyPinnedLease(lease);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.error, InstallError::ContentInvalid);
    QCOMPARE(rejected.stableError, QStringLiteral("installed_content_invalid"));
}

void PackageInstallerTest::pinnedGuardLocksIdentityAndRejectsMemberJunction()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable handle and junction behavior is Windows-specific");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());

    {
        InstallResult installed = installer.install(signedPackage(
            temporary, QStringLiteral("pinned-identity"),
            keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
        QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
        const VerifiedPackageLease lease = leaseFor(installed, 45);
        const InstallResult first = installer.reverifyPinnedLease(
            lease, installed.immutableGuard);
        QVERIFY2(first.succeeded(), qPrintable(first.stableError));
        QVERIFY(first.immutableGuard != nullptr);
        QVERIFY(installed.immutableGuard == nullptr);
        const QString replacement = temporary.filePath(
            QStringLiteral("identity-replacement.qml"));
        QFile replacementFile(replacement);
        QVERIFY(replacementFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(replacementFile.write("replacement identity"), qint64(20));
        replacementFile.close();
        const QString entry = QDir(lease.packageDirectory).filePath(
            lease.entryPoint);
        SetLastError(ERROR_SUCCESS);
        QVERIFY(!MoveFileExW(
            reinterpret_cast<LPCWSTR>(replacement.utf16()),
            reinterpret_cast<LPCWSTR>(entry.utf16()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        const DWORD replacementError = GetLastError();
        QVERIFY(replacementError == ERROR_SHARING_VIOLATION
                || replacementError == ERROR_ACCESS_DENIED);
        const InstallResult second = installer.reverifyPinnedLease(
            lease, first.immutableGuard);
        QVERIFY2(second.succeeded(), qPrintable(second.stableError));
        QCOMPARE(second.immutableGuard, first.immutableGuard);
    }

    InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-junction"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    const VerifiedPackageLease lease = leaseFor(installed, 46);
    installed.immutableGuard.reset();
    QStringList members{lease.packageDirectory};
    QDirIterator iterator(
        lease.packageDirectory,
        QDir::AllEntries | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) members.push_back(iterator.next());
    for (const QString &member : members) {
        QVERIFY2(makeInstalledFileWritable(member), qPrintable(member));
    }
    const QString metadata = QDir(lease.packageDirectory).filePath(
        QStringLiteral("metadata"));
    const QString junctionTarget = temporary.filePath(
        QStringLiteral("junction-target"));
    QVERIFY(QDir().mkpath(junctionTarget));
    const QStringList metadataFiles = QDir(metadata).entryList(
        QDir::Files | QDir::Hidden | QDir::System);
    QVERIFY(!metadataFiles.isEmpty());
    for (const QString &name : metadataFiles) {
        QVERIFY(QFile::copy(QDir(metadata).filePath(name),
                            QDir(junctionTarget).filePath(name)));
    }
    QVERIFY(QDir(metadata).removeRecursively());
    const int junctionExit = QProcess::execute(
        QStringLiteral("C:/Windows/System32/cmd.exe"),
        {QStringLiteral("/d"), QStringLiteral("/c"),
         QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(metadata),
         QDir::toNativeSeparators(junctionTarget)});
    if (junctionExit != 0) {
        QSKIP("Windows junction creation is unavailable");
    }
    const InstallResult rejected = installer.reverifyPinnedLease(lease);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.error, InstallError::ContentInvalid);
    QCOMPARE(rejected.stableError,
             QStringLiteral("installed_content_invalid"));
#endif
}

void PackageInstallerTest::pinnedGuardFreezesMembershipUntilReleased()
{
#ifndef Q_OS_WIN
    QSKIP("Windows package membership sealing is Windows-specific");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-membership"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    const VerifiedPackageLease lease = leaseFor(installed, 47);
    InstallResult first = installer.reverifyPinnedLease(lease);
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    QVERIFY(first.immutableGuard != nullptr);
    InstallResult second = installer.reverifyPinnedLease(
        lease, first.immutableGuard);
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    QCOMPARE(second.immutableGuard, first.immutableGuard);

    const QString root = lease.packageDirectory;
    const QString nested = QDir(root).filePath(QStringLiteral("qml"));
    QVERIFY(QFileInfo(nested).isDir());
    auto heldMutation = std::async(
        std::launch::async,
        [root, nested] {
            return attemptPackageMembershipMutation(root, nested);
        });
    QCOMPARE(heldMutation.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    const PackageMembershipMutationAttempt held = heldMutation.get();
    QVERIFY(!held.rootAclCleared);
    QVERIFY(!held.nestedAclCleared);
    QVERIFY(!held.rootFileCreated);
    QVERIFY(!held.nestedFileCreated);
    QVERIFY(!held.rootDirectoryCreated);
    QVERIFY(!held.nestedDirectoryCreated);
    QVERIFY(!QFileInfo::exists(
        QDir(root).filePath(QStringLiteral("late-root.qml"))));
    QVERIFY(!QFileInfo::exists(
        QDir(nested).filePath(QStringLiteral("late-nested.qml"))));
    QVERIFY(!QFileInfo::exists(
        QDir(root).filePath(QStringLiteral("late-root-directory"))));
    QVERIFY(!QFileInfo::exists(
        QDir(nested).filePath(QStringLiteral("late-nested-directory"))));

    second.immutableGuard.reset();
    first.immutableGuard.reset();
    auto releasedMutation = std::async(
        std::launch::async,
        [root, nested] {
            return attemptPackageMembershipMutation(root, nested);
        });
    QCOMPARE(releasedMutation.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    const PackageMembershipMutationAttempt released = releasedMutation.get();
    QVERIFY(released.rootAclCleared);
    QVERIFY(released.nestedAclCleared);
    QVERIFY(released.rootFileCreated);
    QVERIFY(released.nestedFileCreated);
    QVERIFY(released.rootDirectoryCreated);
    QVERIFY(released.nestedDirectoryCreated);
#endif
}

void PackageInstallerTest::rejectPinnedPathOrDigestMismatch()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-path-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    const InstallResult second = installer.install(signedPackage(
        temporary, QStringLiteral("pinned-path-second"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    const VerifiedPackageLease original = leaseFor(first, 43);

    VerifiedPackageLease wrongPath = original;
    wrongPath.packageDirectory = second.path;
    QVERIFY(!installer.reverifyPinnedLease(wrongPath).succeeded());

    VerifiedPackageLease wrongVersionDirectory = original;
    wrongVersionDirectory.versionDirectory = QFileInfo(second.path).fileName();
    QVERIFY(!installer.reverifyPinnedLease(wrongVersionDirectory).succeeded());

    VerifiedPackageLease wrongDigest = original;
    wrongDigest.digestHex[0] = wrongDigest.digestHex.at(0) == '0' ? '1' : '0';
    QVERIFY(!installer.reverifyPinnedLease(wrongDigest).succeeded());

    VerifiedPackageLease zeroGeneration = original;
    zeroGeneration.activationGenerationAtIssue = 0;
    QVERIFY(!installer.reverifyPinnedLease(zeroGeneration).succeeded());
}

void PackageInstallerTest::rejectsAuthenticatedAppSwappedAfterPrecheckWithoutStoreMutation()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString storeRoot = temporary.filePath(QStringLiteral("store"));
    PackageStore store(storeRoot);
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const QString sourcePackage = signedPackage(
        temporary, QStringLiteral("swap-source"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0")));
    const QString otherPackage = signedPackage(
        temporary, QStringLiteral("swap-other"), keys.value().privateKeyPem,
        manifest(QStringLiteral("9.9.9"), QStringLiteral("1.0.0"),
                 QStringLiteral("1.x"), {QStringLiteral("QtQuick")},
                 QStringLiteral("qml/Main.qml"),
                 QStringLiteral("company.other")));
    QVERIFY(!sourcePackage.isEmpty());
    QVERIFY(!otherPackage.isEmpty());

    bool swapped = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.afterAppIdPrecheckBeforeSourceCopy = [&](const QString &path) {
        QCOMPARE(path, sourcePackage);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(otherPackage, path));
        swapped = true;
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(sourcePackage);
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(swapped);
    QCOMPARE(rejected.error, InstallError::AppIdMismatch);
    QCOMPARE(rejected.stableError, QStringLiteral("app_id_mismatch"));
    QVERIFY(!QFileInfo::exists(store.appRoot(QStringLiteral("company.other"))));
    QVERIFY(!QFileInfo::exists(
        storeRoot + QStringLiteral("/apps/company.other/state.json")));
    QVERIFY(!QFileInfo::exists(storeRoot + QStringLiteral("/.staging")));
}

void PackageInstallerTest::rejectsAuthenticatedOtherAppBeforeStoreMutation()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    const QString storeRoot = temporary.filePath(QStringLiteral("store"));
    PackageStore store(storeRoot);
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const QString package = signedPackage(
        temporary,
        QStringLiteral("other-app"),
        keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"),
                 QStringLiteral("1.0.0"),
                 QStringLiteral("1.x"),
                 {QStringLiteral("QtQuick")},
                 QStringLiteral("qml/Main.qml"),
                 QStringLiteral("company.other")));
    QVERIFY(!package.isEmpty());

    const InstallResult rejected = installer.install(package);

    QCOMPARE(rejected.error, InstallError::AppIdMismatch);
    QCOMPARE(rejected.stableError, QStringLiteral("app_id_mismatch"));
    QVERIFY(!QFileInfo::exists(storeRoot));
    QVERIFY(!QFileInfo::exists(store.appRoot(QStringLiteral("company.other"))));
}

void PackageInstallerTest::reverifyRejectsActivationChangedDuringSnapshot()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("snapshot-a"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    const InstallResult second = installer.install(signedPackage(
        temporary, QStringLiteral("snapshot-b"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0"))));
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    QVERIFY(second.activationBinding.has_value());
    const PackageStoreResult rolledBack = store.rollbackForTesting(
        QStringLiteral("company.pilot"), *second.activationBinding);
    QVERIFY(rolledBack.succeeded());
    QVERIFY(rolledBack.activationBinding.has_value());

    bool changed = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.afterVerifyInstalled = [&](const QString &, const QString &) {
        if (changed) return;
        changed = true;
        const PackageStoreResult activated = store.activateForTesting(
            QStringLiteral("company.pilot"), QFileInfo(second.path).fileName());
        QVERIFY(activated.succeeded());
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.reverifyInstalledVersion(
        QStringLiteral("company.pilot"), *rolledBack.activationBinding);
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(changed);
    QCOMPARE(rejected.error, InstallError::ContentInvalid);
    QVERIFY(!rejected.succeeded());
}

void PackageInstallerTest::installsReverifiesAndActivatesEntryBeyondWindowsMaxPath()
{
#ifndef Q_OS_WIN
    QSKIP("Windows extended paths are Windows-specific");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    // Keep the final installed entry safely beyond MAX_PATH without making the
    // fixture depend on the caller's TEMP prefix length.  Very long acceptance
    // build roots otherwise pushed Qt's own QLockFile cleanup onto a different
    // path-length boundary before this test reached the extended-path assertion.
    constexpr qsizetype targetStoreRootLength = 136;
    const qsizetype storePadding = std::max<qsizetype>(
        8, targetStoreRootLength - temporary.path().size() - 1);
    const QString storeRoot = temporary.filePath(
        QString(storePadding, QLatin1Char('s')));
    PackageStore store(storeRoot);
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const QString entryPoint = QStringLiteral("qml/")
        + QString(96, QLatin1Char('e')) + QStringLiteral("/Start.qml");
    const QString packagePath = signedPackage(
        temporary,
        QStringLiteral("long-installed-entry"),
        keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"),
                 QStringLiteral("1.0.0"),
                 QStringLiteral("1.x"),
                 {QStringLiteral("QtQuick")},
                 entryPoint),
        false,
        QByteArrayLiteral("import QtQuick\nItem {}"),
        {},
        entryPoint.toUtf8());
    QVERIFY(!packagePath.isEmpty());

    const InstallResult installed = installer.install(packagePath);
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    QVERIFY(installed.activationBinding.has_value());
    QCOMPARE(installed.entryPoint, entryPoint);
    const QString installedEntry = installed.path + QLatin1Char('/') + entryPoint;
    QVERIFY2(QFileInfo(installedEntry).absoluteFilePath().size() > 260,
             qPrintable(installedEntry));
    QVERIFY(QFileInfo::exists(installedEntry));

    const InstallResult reverifed = installer.reverifyInstalledVersion(
        QStringLiteral("company.pilot"), *installed.activationBinding);
    QVERIFY2(reverifed.succeeded(), qPrintable(reverifed.stableError));
    QCOMPARE(reverifed.path, installed.path);
    QCOMPARE(reverifed.entryPoint, entryPoint);
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             installed.path);
#endif
}

void PackageInstallerTest::reverifyRejectsMismatchedActivationGeneration()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("bound-metadata"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    QVERIFY(installed.activationBinding.has_value());
    QCOMPARE(installed.entryPoint, QStringLiteral("qml/Main.qml"));

    ActivationBinding mismatched = *installed.activationBinding;
    ++mismatched.generation;
    const InstallResult rejected = installer.reverifyInstalledVersion(
        QStringLiteral("company.pilot"), mismatched);
    QCOMPARE(rejected.error, InstallError::ContentInvalid);
    QVERIFY(!rejected.succeeded());
}

void PackageInstallerTest::installsActivatesAndRollsBackVerifiedVersions()
{
    PackageTemporaryDir temporary;
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
    QVERIFY(store.markCurrentLastKnownGoodForTesting(
                      QStringLiteral("company.pilot"))
                .succeeded());

    const QString second = signedPackage(
        temporary, QStringLiteral("two"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0")));
    const InstallResult installedSecond = installer.install(second);
    QVERIFY2(installedSecond.succeeded(), qPrintable(installedSecond.stableError));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             installedSecond.path);
    QVERIFY(store.rollbackForTesting(QStringLiteral("company.pilot")).succeeded());
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             installedFirst.path);
}

void PackageInstallerTest::rejectsSignedUnsafeQmlWithoutChangingCurrent()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());

    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("safe"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    const QString originalCurrent = store.resolveCurrent(
        QStringLiteral("company.pilot")).path;

    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("malicious"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.0")), false,
        QByteArrayLiteral("import QtQuick\nImage { source: \"file:///C:/secret.txt\" }")));
    QCOMPARE(rejected.phase, InstallPhase::Preflight);
    QCOMPARE(rejected.error, InstallError::PreflightRejected);
    QCOMPARE(rejected.stableError, QStringLiteral("source_policy_rejected"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             originalCurrent);

    const InstallResult mixedCaseRejected = installer.install(signedPackage(
        temporary, QStringLiteral("malicious-mixed-case"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.1.1")), false,
        QByteArrayLiteral("import QtQuick\nItem {}"),
        {{QByteArrayLiteral("qml/Evil.QML"),
          QByteArrayLiteral("import QtQuick\nImage { source: base + name }")},
         {QByteArrayLiteral("logic/Evil.MJS"),
          QByteArrayLiteral("Qt.createComponent(target)")}}));
    QCOMPARE(mixedCaseRejected.phase, InstallPhase::Preflight);
    QCOMPARE(mixedCaseRejected.stableError, QStringLiteral("source_policy_rejected"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path,
             originalCurrent);
}

void PackageInstallerTest::rejectsInvalidSignatureWithoutChangingCurrent()
{
    PackageTemporaryDir temporary;
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
    bool cleanupRaceRan = false;
    bool stagingRenameBlocked = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.afterWindowsHandleOpened = [&](const QString &path,
                                         const quint32 access,
                                         const bool) {
        if (QFileInfo(path).fileName().startsWith(QStringLiteral("install-"))
            && (access & DELETE) != 0U) {
            sawExactStagingLock = true;
        }
    };
    hooks.beforeFailureCleanup = [&](const QString &stagingRoot) {
        if (!QFileInfo(stagingRoot).fileName().startsWith(
                QStringLiteral("install-"))) {
            return;
        }
        cleanupRaceRan = true;
        const QString renamed = stagingRoot + QStringLiteral("-replaced");
        stagingRenameBlocked = MoveFileExW(
                                   reinterpret_cast<LPCWSTR>(stagingRoot.utf16()),
                                   reinterpret_cast<LPCWSTR>(renamed.utf16()),
                                   0U)
            == FALSE;
        if (QFileInfo::exists(renamed)) {
            QVERIFY(QDir().mkpath(stagingRoot));
            QFile replacement(stagingRoot + QStringLiteral("/replacement-marker"));
            QVERIFY(replacement.open(QIODevice::WriteOnly));
            QCOMPARE(replacement.write("preserve"), qint64(8));
        }
    };
    qbrowser_archive_testing::setArchiveTestHooks(std::move(hooks));
#endif
    const InstallResult rejected = installer.install(rejectedPackage);
#ifdef Q_OS_WIN
    qbrowser_archive_testing::resetArchiveTestHooks();
    QVERIFY(sawExactStagingLock);
    QVERIFY(cleanupRaceRan);
    QVERIFY(stagingRenameBlocked);
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
    PackageTemporaryDir temporary;
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
    PackageTemporaryDir temporary;
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
    PackageTemporaryDir temporary;
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

void PackageInstallerTest::preflightMutationCannotEnterCommittedVersion()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    InstallPolicy mutatingPolicy = policy();
    mutatingPolicy.preflight = [](const Manifest &, const QString &root) {
        QFile entry(root + QStringLiteral("/qml/Main.qml"));
        return entry.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && entry.write("mutated by preflight") == qint64(20);
    };
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               std::move(mutatingPolicy));
    const InstallResult installed = installer.install(signedPackage(
        temporary, QStringLiteral("mutating-preflight"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
    QFile entry(installed.path + QStringLiteral("/qml/Main.qml"));
    QVERIFY(entry.open(QIODevice::ReadOnly));
    QCOMPARE(entry.readAll(), QByteArray("import QtQuick\nItem {}"));
    entry.close();
    QVERIFY(!entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
}

void PackageInstallerTest::changedCandidateFailsBeforeActivation()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("candidate-first"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;
    bool hookRan = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidateCommit = [&hookRan](const QString &candidateRoot) {
        hookRan = true;
        QFile entry(candidateRoot + QStringLiteral("/qml/Main.qml"));
        QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(entry.write("changed after authentication"), qint64(28));
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("candidate-changed"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("candidate_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::changedSignatureFailsBeforeActivation()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("signature-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    bool hookRan = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidateCommit = [&hookRan](const QString &candidateRoot) {
        hookRan = true;
        QFile signature(candidateRoot
                        + QStringLiteral("/metadata/signature.ed25519"));
        QVERIFY(signature.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(signature.write(QByteArray(64, 'x')), qint64(64));
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("signature-changed"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("candidate_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::archiveEntryLimitAcceptsMaximumAndRejectsMaximumPlusOne()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));

    InstallPolicy maximumPolicy = policy();
    maximumPolicy.archiveLimits.maximumEntries = 4;
    maximumPolicy.archiveLimits.maximumPathBytes = 26;
    maximumPolicy.archiveLimits.maximumPathUtf16Units = 26;
    maximumPolicy.archiveLimits.maximumComponentBytes = 17;
    maximumPolicy.archiveLimits.maximumComponentUtf16Units = 17;
    PackageInstaller maximumInstaller(
        store, keys.value().publicKeyPem, std::move(maximumPolicy));
    const InstallResult accepted = maximumInstaller.install(signedPackage(
        temporary, QStringLiteral("entry-limit-maximum"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY2(accepted.succeeded(), qPrintable(accepted.stableError));
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    InstallPolicy exceededPolicy = policy();
    exceededPolicy.archiveLimits.maximumEntries = 3;
    PackageInstaller exceededInstaller(
        store, keys.value().publicKeyPem, std::move(exceededPolicy));
    const InstallResult rejected = exceededInstaller.install(signedPackage(
        temporary, QStringLiteral("entry-limit-exceeded"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Verify);
    QCOMPARE(rejected.error, InstallError::ArchiveInvalid);
    QCOMPARE(rejected.stableError, QStringLiteral("archive_invalid"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);

    InstallPolicy componentExceededPolicy = policy();
    componentExceededPolicy.archiveLimits.maximumComponentBytes = 16;
    componentExceededPolicy.archiveLimits.maximumComponentUtf16Units = 16;
    PackageInstaller componentExceededInstaller(
        store, keys.value().publicKeyPem, std::move(componentExceededPolicy));
    const InstallResult componentRejected = componentExceededInstaller.install(
        signedPackage(temporary,
                      QStringLiteral("component-limit-exceeded"),
                      keys.value().privateKeyPem,
                      manifest(QStringLiteral("1.2.0"))));
    QVERIFY(!componentRejected.succeeded());
    QCOMPARE(componentRejected.phase, InstallPhase::Verify);
    QCOMPARE(componentRejected.error, InstallError::ArchiveInvalid);

    InstallPolicy pathExceededPolicy = policy();
    pathExceededPolicy.archiveLimits.maximumPathBytes = 25;
    pathExceededPolicy.archiveLimits.maximumPathUtf16Units = 25;
    PackageInstaller pathExceededInstaller(
        store, keys.value().publicKeyPem, std::move(pathExceededPolicy));
    const InstallResult pathRejected = pathExceededInstaller.install(signedPackage(
        temporary, QStringLiteral("path-limit-exceeded"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.3.0"))));
    QVERIFY(!pathRejected.succeeded());
    QCOMPARE(pathRejected.phase, InstallPhase::Verify);
    QCOMPARE(pathRejected.error, InstallError::ArchiveInvalid);
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::injectedCandidateMembersKeepCleanupHandleCountBounded()
{
#ifndef Q_OS_WIN
    QSKIP("Windows owned staging handles are unavailable");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    InstallPolicy boundedPolicy = policy();
    boundedPolicy.archiveLimits.maximumEntries = 4;
    PackageInstaller installer(
        store, keys.value().publicKeyPem, std::move(boundedPolicy));
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("bounded-cleanup-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    bool injected = false;
    qsizetype handlesAfterInjection = 0;
    qbrowser_package_installer_testing::PackageInstallerTestHooks installerHooks;
    installerHooks.beforeCandidateCommit = [&](const QString &candidateRoot) {
        for (int index = 0; index < 128; ++index) {
            QVERIFY(QDir().mkdir(
                candidateRoot
                + QStringLiteral("/unauthenticated-%1").arg(index, 3, 10, QLatin1Char('0'))));
        }
        injected = true;
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(installerHooks));
    qbrowser_archive_testing::ArchiveTestHooks archiveHooks;
    archiveHooks.afterWindowsHandleOpened = [&](const QString &path,
                                                 const quint32,
                                                 const bool) {
        if (injected
            && QDir::fromNativeSeparators(path).contains(
                QStringLiteral("/.staging/install-"))) {
            ++handlesAfterInjection;
        }
    };
    qbrowser_archive_testing::setArchiveTestHooks(std::move(archiveHooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("bounded-cleanup-injected"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_archive_testing::resetArchiveTestHooks();
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(injected);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QVERIFY2(handlesAfterInjection <= 20,
             qPrintable(QStringLiteral("held %1 post-injection handles")
                            .arg(handlesAfterInjection)));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
#endif
}

void PackageInstallerTest::caseFoldedRaceMembersFailBeforePublication()
{
#ifndef Q_OS_WIN
    QSKIP("Windows case-sensitive directory mode is unavailable");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("case-fold-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    bool caseSensitiveReady = false;
    bool raceHookRan = false;
    bool fileInserted = false;
    bool directoryInserted = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidateCommit = [&](const QString &candidateRoot) {
        caseSensitiveReady = enableCaseSensitiveDirectory(candidateRoot)
            && enableCaseSensitiveDirectory(
                candidateRoot + QStringLiteral("/qml"));
    };
    hooks.afterCandidateScanBeforeSeal = [&](const QString &candidateRoot) {
        raceHookRan = true;
        if (!caseSensitiveReady) {
            return;
        }
        directoryInserted = QDir().mkdir(
            candidateRoot + QStringLiteral("/QML"));
        QFile collision(candidateRoot + QStringLiteral("/qml/main.qml"));
        fileInserted = collision.open(QIODevice::WriteOnly)
            && collision.write("unauthenticated case collision") == qint64(30);
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("case-fold-race"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    if (!caseSensitiveReady) {
        QSKIP("The test volume cannot enable per-directory case sensitivity");
    }
    QVERIFY(raceHookRan);
    QVERIFY(fileInserted);
    QVERIFY(directoryInserted);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("candidate_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
#endif
}

void PackageInstallerTest::activationFailureDoesNotChangeCurrent()
{
#ifndef Q_OS_WIN
    QSKIP("The deterministic activation sharing violation is Windows-specific");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("activation-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;
    bool hookRan = false;
    HANDLE blocker = INVALID_HANDLE_VALUE;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeActivate = [&](const QString &appId, const QString &) {
        hookRan = true;
        const QString statePath = store.root() + QStringLiteral("/apps/") + appId
            + QStringLiteral("/activation.json");
        blocker = CreateFileW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(statePath).utf16()),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("activation-blocked"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();
    if (blocker != INVALID_HANDLE_VALUE) {
        CloseHandle(blocker);
    }

    QVERIFY(hookRan);
    QVERIFY(blocker != INVALID_HANDLE_VALUE);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Activate);
    QCOMPARE(rejected.error, InstallError::ActivationFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("activation_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
#endif
}

void PackageInstallerTest::postVerificationNewMemberCannotPublishUnderOldDigest()
{
#ifndef Q_OS_WIN
    QSKIP("Windows candidate tree mutation seals are unavailable");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("member-first"), keys.value().privateKeyPem,
        manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());

    bool hookRan = false;
    bool mutationSucceeded = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidatePublish = [&](const QString &candidateRoot,
                                        const QString &) {
        hookRan = true;
        const QString injected = candidateRoot
            + QStringLiteral("/injected/member.txt");
        mutationSucceeded = QDir().mkpath(QFileInfo(injected).dir().absolutePath());
        QFile file(injected);
        mutationSucceeded = mutationSucceeded
            && file.open(QIODevice::WriteOnly)
            && file.write("unauthenticated") == qint64(15);
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult result = installer.install(signedPackage(
        temporary, QStringLiteral("member-injected"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(!mutationSucceeded);
    QVERIFY2(result.succeeded(), qPrintable(result.stableError));
    QVERIFY(!QFileInfo::exists(result.path + QStringLiteral("/injected")));
    QFile added(result.path + QStringLiteral("/postpublish.txt"));
    QVERIFY(!added.open(QIODevice::WriteOnly));
#endif
}

void PackageInstallerTest::postVerificationReplacementCannotPublishUnderOldDigest()
{
#ifndef Q_OS_WIN
    QSKIP("Windows candidate tree mutation seals are unavailable");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("replacement-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());

    bool hookRan = false;
    bool mutationSucceeded = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidatePublish = [&](const QString &candidateRoot,
                                        const QString &) {
        hookRan = true;
        const QString entryPath = candidateRoot + QStringLiteral("/qml/Main.qml");
        (void)QFile::setPermissions(
            entryPath,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile entry(entryPath);
        mutationSucceeded = entry.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && entry.write("unauthenticated replacement") == qint64(27);
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult result = installer.install(signedPackage(
        temporary, QStringLiteral("replacement-injected"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(!mutationSucceeded);
    QVERIFY2(result.succeeded(), qPrintable(result.stableError));
    const QString entryPath = result.path + QStringLiteral("/qml/Main.qml");
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::ReadOnly));
    QCOMPARE(entry.readAll(), QByteArray("import QtQuick\nItem {}"));
    entry.close();
    QVERIFY(!entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(!QFile::remove(entryPath));
#endif
}

void PackageInstallerTest::unexpectedEmptyDirectoryFailsCandidate()
{
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("empty-directory-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;

    bool hookRan = false;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidateCommit = [&hookRan](const QString &candidateRoot) {
        hookRan = true;
        QVERIFY(QDir().mkdir(
            candidateRoot + QStringLiteral("/unauthenticated-empty")));
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("empty-directory-injected"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("candidate_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
}

void PackageInstallerTest::publicationRaceRestoresOwnedStagingCleanup()
{
#ifndef Q_OS_WIN
    QSKIP("Windows handle-based publication is unavailable");
#else
    PackageTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, policy());
    const InstallResult first = installer.install(signedPackage(
        temporary, QStringLiteral("publication-first"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.0.0"))));
    QVERIFY(first.succeeded());
    const QString before = store.resolveCurrent(QStringLiteral("company.pilot")).path;
    const QString marker = store.root()
        + QStringLiteral("/.staging/do-not-remove/marker");
    QVERIFY(QDir().mkpath(QFileInfo(marker).dir().absolutePath()));
    QFile markerFile(marker);
    QVERIFY(markerFile.open(QIODevice::WriteOnly));
    QCOMPARE(markerFile.write("preserve"), qint64(8));
    markerFile.close();

    bool hookRan = false;
    bool destinationWonRace = false;
    QString racedDestination;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.beforeCandidatePublish = [&](const QString &, const QString &destination) {
        hookRan = true;
        racedDestination = destination;
        destinationWonRace = QDir().mkdir(destination);
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const InstallResult rejected = installer.install(signedPackage(
        temporary, QStringLiteral("publication-race"),
        keys.value().privateKeyPem, manifest(QStringLiteral("1.1.0"))));
    qbrowser_package_installer_testing::resetPackageInstallerTestHooks();

    QVERIFY(hookRan);
    QVERIFY(destinationWonRace);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.phase, InstallPhase::Candidate);
    QCOMPARE(rejected.error, InstallError::CandidateFailed);
    QCOMPARE(rejected.stableError, QStringLiteral("candidate_failed"));
    QCOMPARE(store.resolveCurrent(QStringLiteral("company.pilot")).path, before);
    QVERIFY(QFileInfo::exists(racedDestination));
    QVERIFY(QFileInfo::exists(marker));
    const QStringList stagingEntries = QDir(store.root() + QStringLiteral("/.staging"))
                                           .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(stagingEntries, QStringList{QStringLiteral("do-not-remove")});
#endif
}

QTEST_APPLESS_MAIN(PackageInstallerTest)

#include "tst_package_installer.moc"
