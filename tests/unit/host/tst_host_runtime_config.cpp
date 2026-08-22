#include "HostRuntimeConfig.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <Sddl.h>
#include <qt_windows.h>
#endif

namespace
{
#ifdef Q_OS_WIN
QByteArray fileSecurity(const QString &path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr) return {};
    const DWORD size = GetSecurityDescriptorLength(descriptor);
    const QByteArray bytes(static_cast<const char *>(descriptor),
                           static_cast<qsizetype>(size));
    LocalFree(descriptor);
    return bytes;
}

bool applySecurity(const QString &path, const QString &sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1,
            &descriptor, nullptr) == FALSE) {
        return false;
    }
    PACL dacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    const bool valid = GetSecurityDescriptorDacl(
               descriptor, &daclPresent, &dacl, &daclDefaulted) != FALSE
        && daclPresent != FALSE;
    const DWORD result = valid
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

bool protectTrustKey(const QString &path)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    if (queried != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemBytes = sizeof(systemBuffer);
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer,
                           &systemBytes) == FALSE) {
        LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESSW entries[2]{};
    for (EXPLICIT_ACCESSW &entry : entries) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(owner);
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(systemBuffer);
    PACL dacl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(2, entries, nullptr, &dacl);
    const DWORD applied = aclResult == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : aclResult;
    if (dacl != nullptr) LocalFree(dacl);
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
}
#endif

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}

qsizetype argumentIndex(const QStringList &arguments, const QString &prefix)
{
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        if (arguments[index].startsWith(prefix)) return index;
    }
    return -1;
}

struct ValidArguments final
{
    QTemporaryDir root;
    QTemporaryDir trustRoot;
    QString store;
    QString sandboxTemp;
    QString runtime;
    QString telemetry;
    QString storage;
    QString worker;
    QString publicKey;
    QStringList values;

    ValidArguments()
    {
        const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
        if (!root.isValid() || !trustRoot.isValid() || !keys.hasValue()) return;
        store = root.filePath(QStringLiteral("store"));
        sandboxTemp = root.filePath(QStringLiteral("sandbox-temp"));
        runtime = root.filePath(QStringLiteral("runtime"));
        telemetry = root.filePath(QStringLiteral("telemetry"));
        storage = root.filePath(QStringLiteral("storage"));
        worker = QDir(runtime).filePath(QStringLiteral("qbrowser-worker.exe"));
        publicKey = trustRoot.filePath(QStringLiteral("trusted-public.pem"));
        if (!QDir().mkpath(store) || !QDir().mkpath(sandboxTemp)
            || !QDir().mkpath(runtime) || !QDir().mkpath(telemetry)
            || !QDir().mkpath(storage)
            || !writeFile(worker, QByteArrayLiteral("worker"))
            || !writeFile(publicKey, keys.value().publicKeyPem)
#ifdef Q_OS_WIN
            || !protectTrustKey(publicKey)
#endif
        ) {
            values.clear();
            return;
        }
        values = {
            QStringLiteral("--package-mode"),
            QStringLiteral("--app-id=com.qbrowser.runtime"),
            QStringLiteral("--trusted-public-key=") + publicKey,
            QStringLiteral("--package-store=") + store,
            QStringLiteral("--sandbox-temp=") + sandboxTemp,
            QStringLiteral("--runtime-root=") + runtime,
            QStringLiteral("--worker-executable=") + worker,
            QStringLiteral("--telemetry-directory=") + telemetry,
            QStringLiteral("--storage-directory=") + storage,
        };
    }
};
}

class HostRuntimeConfigTest final : public QObject
{
    Q_OBJECT

private slots:
    void requiresExplicitMode();
    void acceptsOnlyExplicitTrustedShellWithoutPackageAuthority();
    void loadsCompletePackageAuthorityFromAbsolutePaths();
    void rejectsAmbientOrOverlappingAuthority();
    void rejectsTelemetryOverlappingPackageAuthority();
    void rejectsBroadWritableTrustKeyWithoutChangingPermissions();
    void trustKeyStableReadBlocksSwapAndGrowth();
    void rejectsOversizedMalformedAndJunctionTrustKeys();
    void rejectsNonLoopbackMockOrigin();
    void requiresIndependentStorageRoot();
};

void HostRuntimeConfigTest::requiresIndependentStorageRoot()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const qsizetype storageIndex = argumentIndex(
        arguments.values, QStringLiteral("--storage-directory="));
    QVERIFY(storageIndex >= 0);
    arguments.values.removeAt(storageIndex);
    const HostRuntimeConfigResult missing = HostRuntimeConfig::fromArguments(
        arguments.values);
    QVERIFY(!missing.value.has_value());
    QCOMPARE(missing.error, HostRuntimeConfigError::MissingArgument);
    QCOMPARE(missing.stableError,
             QStringLiteral("host.config.missing_storage_directory"));
}

void HostRuntimeConfigTest::rejectsBroadWritableTrustKeyWithoutChangingPermissions()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation is Windows-specific");
#else
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QVERIFY(applySecurity(arguments.publicKey,
                          QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
    const QByteArray before = fileSecurity(arguments.publicKey);
    QVERIFY(!before.isEmpty());

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values);

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::UnsafePath);
    QCOMPARE(fileSecurity(arguments.publicKey), before);
#endif
}

void HostRuntimeConfigTest::trustKeyStableReadBlocksSwapAndGrowth()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable-handle validation is Windows-specific");
#else
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    qbrowser_archive_detail::WindowsStableDirectoryTree tree;
    qbrowser_archive_detail::WindowsStableFile file;
    QVERIFY(tree.openRoot(QFileInfo(arguments.publicKey).absolutePath()));
    QVERIFY(file.openReadLocked(arguments.publicKey, tree));
    const QString replacement = arguments.trustRoot.filePath(
        QStringLiteral("replacement.pem"));
    QVERIFY(writeFile(replacement, QByteArrayLiteral("replacement")));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(replacement.utf16()),
        reinterpret_cast<LPCWSTR>(arguments.publicKey.utf16()),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    QFile growth(arguments.publicKey);
    QVERIFY(!growth.open(QIODevice::WriteOnly | QIODevice::Append));
    QByteArray bytes;
    QVERIFY(file.readBounded(64U * 1024U, bytes));
    QVERIFY(file.isSameIdentityAt(arguments.publicKey));
    QVERIFY(SignatureVerifier::isValidPublicKeyPem(bytes));
#endif
}

void HostRuntimeConfigTest::rejectsOversizedMalformedAndJunctionTrustKeys()
{
    ValidArguments oversized;
    QVERIFY(!oversized.values.isEmpty());
    QFile oversizedFile(oversized.publicKey);
    QVERIFY(oversizedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(oversizedFile.write(QByteArray(64 * 1024 + 1, 'x')),
             qint64(64 * 1024 + 1));
    oversizedFile.close();
#ifdef Q_OS_WIN
    QVERIFY(protectTrustKey(oversized.publicKey));
#endif
    const HostRuntimeConfigResult tooLarge = HostRuntimeConfig::fromArguments(
        oversized.values);
    QVERIFY(!tooLarge.value.has_value());
    QCOMPARE(tooLarge.error, HostRuntimeConfigError::PublicKeyUnavailable);

    ValidArguments malformed;
    QVERIFY(!malformed.values.isEmpty());
    QFile malformedFile(malformed.publicKey);
    QVERIFY(malformedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray malformedBytes = QByteArrayLiteral(
        "-----BEGIN PUBLIC KEY-----\ninvalid\n-----END PUBLIC KEY-----\n");
    QCOMPARE(malformedFile.write(malformedBytes),
             static_cast<qint64>(malformedBytes.size()));
    malformedFile.close();
#ifdef Q_OS_WIN
    QVERIFY(protectTrustKey(malformed.publicKey));
#endif
    const HostRuntimeConfigResult invalidPem = HostRuntimeConfig::fromArguments(
        malformed.values);
    QVERIFY(!invalidPem.value.has_value());
    QCOMPARE(invalidPem.error, HostRuntimeConfigError::PublicKeyUnavailable);

#ifdef Q_OS_WIN
    ValidArguments junction;
    QVERIFY(!junction.values.isEmpty());
    const QString source = junction.root.filePath(QStringLiteral("junction-source"));
    const QString link = junction.root.filePath(QStringLiteral("junction-link"));
    QVERIFY(QDir().mkpath(source));
    const QString linkedKey = QDir(source).filePath(QStringLiteral("trusted.pem"));
    QVERIFY(QFile::copy(junction.publicKey, linkedKey));
    QVERIFY(protectTrustKey(linkedKey));
    const int linked = QProcess::execute(
        QStringLiteral("cmd.exe"),
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"),
         QStringLiteral("/J"), QDir::toNativeSeparators(link),
         QDir::toNativeSeparators(source)});
    if (linked != 0) QSKIP("Junction creation unavailable");
    const qsizetype keyIndex = argumentIndex(
        junction.values, QStringLiteral("--trusted-public-key="));
    QVERIFY(keyIndex >= 0);
    junction.values[keyIndex] = QStringLiteral("--trusted-public-key=")
        + QDir(link).filePath(QStringLiteral("trusted.pem"));
    const HostRuntimeConfigResult throughJunction =
        HostRuntimeConfig::fromArguments(junction.values);
    QVERIFY(!throughJunction.value.has_value());
    QCOMPARE(throughJunction.error, HostRuntimeConfigError::UnsafePath);
#endif
}

void HostRuntimeConfigTest::rejectsTelemetryOverlappingPackageAuthority()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const qsizetype telemetryIndex = argumentIndex(
        arguments.values, QStringLiteral("--telemetry-directory="));
    QVERIFY(telemetryIndex >= 0);
    arguments.values[telemetryIndex] =
        QStringLiteral("--telemetry-directory=") + arguments.store;

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values);

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);

    ValidArguments keyArguments;
    QVERIFY(!keyArguments.values.isEmpty());
    const QString trustParent = keyArguments.root.filePath(
        QStringLiteral("isolated-trust"));
    const QString nestedTelemetry = QDir(trustParent).filePath(
        QStringLiteral("telemetry"));
    const QString nestedKey = QDir(trustParent).filePath(
        QStringLiteral("trusted.pem"));
    QVERIFY(QDir().mkpath(nestedTelemetry));
    QVERIFY(QFile::copy(keyArguments.publicKey, nestedKey));
#ifdef Q_OS_WIN
    QVERIFY(protectTrustKey(nestedKey));
#endif
    const qsizetype keyIndex = argumentIndex(
        keyArguments.values, QStringLiteral("--trusted-public-key="));
    const qsizetype nestedTelemetryIndex = argumentIndex(
        keyArguments.values, QStringLiteral("--telemetry-directory="));
    QVERIFY(keyIndex >= 0);
    QVERIFY(nestedTelemetryIndex >= 0);
    keyArguments.values[keyIndex] =
        QStringLiteral("--trusted-public-key=") + nestedKey;
    keyArguments.values[nestedTelemetryIndex] =
        QStringLiteral("--telemetry-directory=") + nestedTelemetry;

    const HostRuntimeConfigResult nested = HostRuntimeConfig::fromArguments(
        keyArguments.values);
    QVERIFY(!nested.value.has_value());
    QCOMPARE(nested.error, HostRuntimeConfigError::OverlappingRoots);
}

void HostRuntimeConfigTest::requiresExplicitMode()
{
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments({});
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::ModeRequired);
    QCOMPARE(result.stableError, QStringLiteral("host.config.mode_required"));
}

void HostRuntimeConfigTest::acceptsOnlyExplicitTrustedShellWithoutPackageAuthority()
{
    const HostRuntimeConfigResult shell = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell")});
    QVERIFY2(shell.value.has_value(), qPrintable(shell.stableError));
    QCOMPARE(shell.value->mode(), HostRuntimeMode::TrustedShell);
    QVERIFY(shell.value->appId().isEmpty());
    QVERIFY(shell.value->trustedPublicKeyPem().isEmpty());

    const HostRuntimeConfigResult mixed = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell"), QStringLiteral("--package-mode")});
    QVERIFY(!mixed.value.has_value());
    QCOMPARE(mixed.error, HostRuntimeConfigError::AmbiguousMode);
}

void HostRuntimeConfigTest::loadsCompletePackageAuthorityFromAbsolutePaths()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QStringList configured = arguments.values;
    configured.push_back(
        QStringLiteral("--mock-origin=http://127.0.0.1:53111/"));
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        configured);
    QVERIFY2(result.value.has_value(), qPrintable(result.stableError));
    QCOMPARE(result.value->mode(), HostRuntimeMode::Package);
    QCOMPARE(result.value->appId(), QStringLiteral("com.qbrowser.runtime"));
    QVERIFY(result.value->trustedPublicKeyPem().startsWith(
        QByteArrayLiteral("-----BEGIN PUBLIC KEY-----")));
    QCOMPARE(result.value->packageStoreRoot(), QDir(arguments.store).canonicalPath());
    QCOMPARE(result.value->sandboxTempRoot(),
             QDir(arguments.sandboxTemp).canonicalPath());
    QCOMPARE(result.value->storageDirectory(),
             QDir(arguments.storage).canonicalPath());
    QCOMPARE(result.value->workerExecutable(),
             QFileInfo(arguments.worker).canonicalFilePath());
    QCOMPARE(result.value->immutableRuntimeRoots(),
             QStringList{QDir(arguments.runtime).canonicalPath()});
    QVERIFY(!result.value->installPackage().has_value());
    QCOMPARE(result.value->mockOrigin(),
             QUrl(QStringLiteral("http://127.0.0.1:53111/")));
}

void HostRuntimeConfigTest::rejectsNonLoopbackMockOrigin()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    arguments.values.push_back(
        QStringLiteral("--mock-origin=http://example.com:4173"));
    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values);
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.stableError,
             QStringLiteral("host.config.invalid_mock_origin"));
}

void HostRuntimeConfigTest::rejectsAmbientOrOverlappingAuthority()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QStringList ambient = arguments.values;
    const qsizetype workerIndex = argumentIndex(
        ambient, QStringLiteral("--worker-executable="));
    QVERIFY(workerIndex >= 0);
    ambient[workerIndex] = QStringLiteral("--worker-executable=qbrowser-worker.exe");
    const HostRuntimeConfigResult ambientResult = HostRuntimeConfig::fromArguments(
        ambient);
    QVERIFY(!ambientResult.value.has_value());
    QCOMPARE(ambientResult.error, HostRuntimeConfigError::UnsafePath);

    QStringList overlapping = arguments.values;
    const qsizetype tempIndex = argumentIndex(
        overlapping, QStringLiteral("--sandbox-temp="));
    QVERIFY(tempIndex >= 0);
    overlapping[tempIndex] = QStringLiteral("--sandbox-temp=") + arguments.store;
    const HostRuntimeConfigResult overlapResult = HostRuntimeConfig::fromArguments(
        overlapping);
    QVERIFY(!overlapResult.value.has_value());
    QCOMPARE(overlapResult.error, HostRuntimeConfigError::OverlappingRoots);
}

QTEST_APPLESS_MAIN(HostRuntimeConfigTest)

#include "tst_host_runtime_config.moc"
