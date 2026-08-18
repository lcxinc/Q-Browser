#include "Archive.h"
#include "ArchiveTestHooks.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageInstallerTestHooks.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

namespace
{
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
            QString native = QDir::toNativeSeparators(entry);
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
    void preflightMutationCannotEnterCommittedVersion();
    void changedCandidateFailsBeforeActivation();
    void activationFailureDoesNotChangeCurrent();
    void postVerificationNewMemberCannotPublishUnderOldDigest();
    void postVerificationReplacementCannotPublishUnderOldDigest();
    void unexpectedEmptyDirectoryFailsCandidate();
    void publicationRaceRestoresOwnedStagingCleanup();
};

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
