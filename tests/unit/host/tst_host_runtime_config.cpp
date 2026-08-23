#include "HostOwnedFileAuthority.h"
#include "HostOwnedStateDirectory.h"
#include "HostRuntimeConfig.h"
#include "SignatureVerifier.h"
#include "WindowsStableIo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <vector>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <Sddl.h>
#include <qt_windows.h>
#endif

QString currentExecutablePath()
{
#ifdef Q_OS_WIN
    std::vector<wchar_t> buffer(32U * 1024U);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(
            GetCurrentProcess(), 0U, buffer.data(), &length) == FALSE
        || length == 0U
        || static_cast<size_t>(length) >= buffer.size()) {
        return {};
    }
    return QString::fromWCharArray(
        buffer.data(), static_cast<qsizetype>(length));
#else
    return QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
#endif
}

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

bool protectPath(const QString &path, const bool container = false)
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
        entry.grfInheritance = container
            ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
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

bool protectTrustKey(const QString &path)
{
    return protectPath(path);
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

bool setArgument(QStringList &arguments,
                 const QString &prefix,
                 const QString &value)
{
    const qsizetype index = argumentIndex(arguments, prefix);
    if (index < 0) return false;
    arguments[index] = prefix + value;
    return true;
}

bool createProtectedFile(const QString &path, const QByteArray &bytes)
{
    if (!writeFile(path, bytes)) return false;
#ifdef Q_OS_WIN
    return protectPath(path);
#else
    return true;
#endif
}

struct ValidArguments final
{
    QTemporaryDir root;
    QString deployment;
    QString browserState;
    QString hostDirectory;
    QString trustDirectory;
    QString packagesDirectory;
    QString store;
    QString sandboxTemp;
    QString runtime;
    QString telemetry;
    QString storage;
    QString worker;
    QString publicKey;
    QString currentHostExecutable;
    HostRuntimeParseContext context;
    QStringList values;

    ValidArguments()
    {
        const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
        if (!root.isValid() || !keys.hasValue()) return;
        store = root.filePath(QStringLiteral("store"));
        deployment = root.filePath(QStringLiteral("deployment"));
        browserState = root.filePath(QStringLiteral("browser-state"));
        hostDirectory = QDir(deployment).filePath(QStringLiteral("host"));
        trustDirectory = QDir(deployment).filePath(QStringLiteral("trust"));
        packagesDirectory = QDir(deployment).filePath(QStringLiteral("packages"));
        sandboxTemp = root.filePath(QStringLiteral("sandbox-temp"));
        runtime = QDir(deployment).filePath(QStringLiteral("runtime"));
        telemetry = root.filePath(QStringLiteral("telemetry"));
        storage = root.filePath(QStringLiteral("storage"));
        worker = QDir(runtime).filePath(QStringLiteral("qbrowser-worker.exe"));
        publicKey = QDir(trustDirectory).filePath(
            QStringLiteral("trusted-public.pem"));
        currentHostExecutable = QDir(hostDirectory).filePath(
            QStringLiteral("qbrowser-host.exe"));
        if (!QDir().mkpath(hostDirectory) || !QDir().mkpath(trustDirectory)
            || !QDir().mkpath(packagesDirectory)
            || !QDir().mkpath(deployment) || !QDir().mkpath(browserState)
            || !QDir().mkpath(store) || !QDir().mkpath(sandboxTemp)
            || !QDir().mkpath(runtime) || !QDir().mkpath(telemetry)
            || !QDir().mkpath(storage)
            || !writeFile(worker, QByteArrayLiteral("worker"))
            || !writeFile(publicKey, keys.value().publicKeyPem)
            || !QFile::copy(currentExecutablePath(), currentHostExecutable)
#ifdef Q_OS_WIN
            || !protectPath(deployment, true)
            || !protectPath(browserState, true)
            || !protectPath(hostDirectory, true)
            || !protectPath(trustDirectory, true)
            || !protectPath(packagesDirectory, true)
            || !protectPath(runtime, true)
            || !protectPath(currentHostExecutable)
            || !protectPath(worker)
            || !protectTrustKey(publicKey)
#endif
        ) {
            values.clear();
            return;
        }
        context.currentHostExecutable =
            HostOwnedFileAuthority::open(currentHostExecutable);
        if (!context.currentHostExecutable) return;
        values = {
            QStringLiteral("--package-mode"),
            QStringLiteral("--deployment-root=") + deployment,
            QStringLiteral("--browser-state-directory=") + browserState,
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
    void init();
    void requiresExplicitMode();
    void acceptsOnlyExplicitTrustedShellWithoutPackageAuthority();
    void nonWindowsStableAuthoritiesFailClosed();
    void hostPathRelationsAreRootAware_data();
    void hostPathRelationsAreRootAware();
    void validatesImmutableRuntimeRootDisjointness_data();
    void validatesImmutableRuntimeRootDisjointness();
    void loadsCompletePackageAuthorityFromAbsolutePaths();
    void rejectsAmbientOrOverlappingAuthority();
    void rejectsTelemetryOverlappingPackageAuthority();
    void rejectsBroadWritableTrustKeyWithoutChangingPermissions();
    void trustKeyStableReadBlocksSwapAndGrowth();
    void rejectsOversizedMalformedAndJunctionTrustKeys();
    void rejectsNonLoopbackMockOrigin();
    void requiresDeploymentRoot();
    void requiresBrowserStateRoot();
    void requiresIndependentStorageRoot();
    void rejectsPackageModeWithoutCurrentHostEvidence();
    void bindsDeploymentToCurrentHostAndImmutableContents();
    void acceptsDeploymentContainedAndProtectedExternalPackages();
    void rejectsUnsafeExternalPackageAuthorities();
    void rejectsExternalPackageParentOverlappingMutableRoot_data();
    void rejectsExternalPackageParentOverlappingMutableRoot();
    void rejectsMutableRootsInsideDeployment_data();
    void rejectsMutableRootsInsideDeployment();
    void rejectsBrowserStateOverlap_data();
    void rejectsBrowserStateOverlap();
    void rejectsPermissiveProtectedRootsWithoutChangingPermissions_data();
    void rejectsPermissiveProtectedRootsWithoutChangingPermissions();
    void rejectsReparseAncestors_data();
    void rejectsReparseAncestors();
    void parsingPreservesProtectedAclsAndAuthoritiesBlockReplacement();
};

void HostRuntimeConfigTest::init()
{
#ifndef Q_OS_WIN
    const QString test = QString::fromLatin1(QTest::currentTestFunction());
    if (test != QLatin1String("requiresExplicitMode")
        && test != QLatin1String(
            "acceptsOnlyExplicitTrustedShellWithoutPackageAuthority")
        && test != QLatin1String("nonWindowsStableAuthoritiesFailClosed")
        && test != QLatin1String("hostPathRelationsAreRootAware")) {
        QSKIP("Package-mode stable authorities are Windows-only");
    }
#endif
}

void HostRuntimeConfigTest::nonWindowsStableAuthoritiesFailClosed()
{
#ifdef Q_OS_WIN
    QSKIP("Non-Windows fail-closed behavior cannot run on Windows");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString file = temporary.filePath(QStringLiteral("authority.bin"));
    QVERIFY(writeFile(file, QByteArrayLiteral("authority")));

    QVERIFY(!HostOwnedStateDirectory::open(temporary.path()));
    QVERIFY(!HostOwnedFileAuthority::open(file));
    QVERIFY(!HostOwnedFileAuthority::openCurrentProcessExecutable());

    HostRuntimeParseContext context;
    const HostRuntimeConfigResult package = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--package-mode"),
         QStringLiteral("--deployment-root=") + temporary.path(),
         QStringLiteral("--browser-state-directory=") + temporary.path(),
         QStringLiteral("--app-id=com.qbrowser.runtime"),
         QStringLiteral("--trusted-public-key=") + file,
         QStringLiteral("--package-store=") + temporary.path(),
         QStringLiteral("--sandbox-temp=") + temporary.path(),
         QStringLiteral("--runtime-root=") + temporary.path(),
         QStringLiteral("--worker-executable=") + file,
         QStringLiteral("--telemetry-directory=") + temporary.path(),
         QStringLiteral("--storage-directory=") + temporary.path()},
        context);
    QVERIFY(!package.value.has_value());
    QCOMPARE(package.error, HostRuntimeConfigError::UnsafePath);
    QCOMPARE(package.stableError,
             QStringLiteral("host.config.current_host_unavailable"));

    const HostRuntimeConfigResult shell = HostRuntimeConfig::fromArguments(
        {QStringLiteral("--trusted-shell")}, context);
    QVERIFY2(shell.value.has_value(), qPrintable(shell.stableError));
#endif
}

void HostRuntimeConfigTest::hostPathRelationsAreRootAware_data()
{
    QTest::addColumn<QString>("root");
    QTest::addColumn<QString>("candidate");
    QTest::addColumn<bool>("withinOrEqual");
    QTest::addColumn<bool>("strictDescendant");
    QTest::addColumn<bool>("overlap");

    QTest::newRow("drive-root-descendant")
        << QStringLiteral("D:\\") << QStringLiteral("D:\\deployment")
        << true << true << true;
    QTest::newRow("drive-root-equality")
        << QStringLiteral("D:\\") << QStringLiteral("d:\\")
        << true << false << true;
    QTest::newRow("non-prefix-sibling")
        << QStringLiteral("D:\\foo") << QStringLiteral("D:\\foobar")
        << false << false << false;
    QTest::newRow("normal-descendant")
        << QStringLiteral("D:\\deployment")
        << QStringLiteral("D:\\deployment\\runtime")
        << true << true << true;
    QTest::newRow("normal-equality")
        << QStringLiteral("D:\\deployment")
        << QStringLiteral("d:\\deployment")
        << true << false << true;
    QTest::newRow("normal-sibling")
        << QStringLiteral("D:\\deployment")
        << QStringLiteral("D:\\browser-state")
        << false << false << false;
    QTest::newRow("unc-root-descendant")
        << QStringLiteral("\\\\server\\share\\")
        << QStringLiteral("\\\\server\\share\\deployment")
        << true << true << true;
}

void HostRuntimeConfigTest::hostPathRelationsAreRootAware()
{
    QFETCH(QString, root);
    QFETCH(QString, candidate);
    QFETCH(bool, withinOrEqual);
    QFETCH(bool, strictDescendant);
    QFETCH(bool, overlap);

    QCOMPARE(qbrowser_host_detail::pathWithinOrEqual(root, candidate),
             withinOrEqual);
    QCOMPARE(qbrowser_host_detail::strictPathDescendant(root, candidate),
             strictDescendant);
    QCOMPARE(qbrowser_host_detail::pathsOverlap(root, candidate), overlap);
    QCOMPARE(qbrowser_host_detail::pathsOverlap(candidate, root), overlap);
}

void HostRuntimeConfigTest::validatesImmutableRuntimeRootDisjointness_data()
{
    QTest::addColumn<int>("relation");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("duplicate") << 0 << false;
    QTest::newRow("nested-child") << 1 << false;
    QTest::newRow("distinct-sibling") << 2 << true;
}

void HostRuntimeConfigTest::validatesImmutableRuntimeRootDisjointness()
{
    QFETCH(int, relation);
    QFETCH(bool, accepted);
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());

    QString additionalRoot = arguments.runtime;
    if (relation == 1) {
        additionalRoot = QDir(arguments.runtime).filePath(
            QStringLiteral("nested-runtime"));
    } else if (relation == 2) {
        additionalRoot = QDir(arguments.deployment).filePath(
            QStringLiteral("runtime-sibling"));
    }
    if (relation != 0) {
        QVERIFY(QDir().mkpath(additionalRoot));
#ifdef Q_OS_WIN
        QVERIFY(protectPath(additionalRoot, true));
#endif
    }
    arguments.values.push_back(
        QStringLiteral("--runtime-root=") + additionalRoot);

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
    QCOMPARE(result.value.has_value(), accepted);
    if (accepted) {
        QCOMPARE(result.error, HostRuntimeConfigError::None);
        QCOMPARE(result.value->immutableRuntimeRoots().size(), 2);
    } else {
        QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);
        QCOMPARE(result.stableError,
                 QStringLiteral("host.config.overlapping_roots"));
    }
}

void HostRuntimeConfigTest::requiresDeploymentRoot()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());

    QStringList missingDeploymentArguments = arguments.values;
    const qsizetype deploymentIndex = argumentIndex(
        missingDeploymentArguments, QStringLiteral("--deployment-root="));
    QVERIFY(deploymentIndex >= 0);
    missingDeploymentArguments.removeAt(deploymentIndex);
    const HostRuntimeConfigResult missingDeployment =
        HostRuntimeConfig::fromArguments(missingDeploymentArguments,
                                         arguments.context);
    QVERIFY(!missingDeployment.value.has_value());
    QCOMPARE(missingDeployment.error, HostRuntimeConfigError::MissingArgument);
    QCOMPARE(missingDeployment.stableError,
             QStringLiteral("host.config.missing_deployment_root"));
}

void HostRuntimeConfigTest::requiresBrowserStateRoot()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QStringList missingStateArguments = arguments.values;
    const qsizetype stateIndex = argumentIndex(
        missingStateArguments, QStringLiteral("--browser-state-directory="));
    QVERIFY(stateIndex >= 0);
    missingStateArguments.removeAt(stateIndex);
    const HostRuntimeConfigResult missingState =
        HostRuntimeConfig::fromArguments(missingStateArguments,
                                         arguments.context);
    QVERIFY(!missingState.value.has_value());
    QCOMPARE(missingState.error, HostRuntimeConfigError::MissingArgument);
    QCOMPARE(missingState.stableError,
             QStringLiteral("host.config.missing_browser_state_directory"));
}

void HostRuntimeConfigTest::rejectsPackageModeWithoutCurrentHostEvidence()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, {});

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::UnsafePath);
}

void HostRuntimeConfigTest::bindsDeploymentToCurrentHostAndImmutableContents()
{
    ValidArguments fakeRoot;
    QVERIFY(!fakeRoot.values.isEmpty());
    const QString callerRoot = fakeRoot.root.filePath(
        QStringLiteral("caller-fake-deployment"));
    QVERIFY(QDir().mkpath(callerRoot));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(callerRoot, true));
#endif
    QVERIFY(setArgument(fakeRoot.values, QStringLiteral("--deployment-root="),
                        callerRoot));
    const HostRuntimeConfigResult callerFake = HostRuntimeConfig::fromArguments(
        fakeRoot.values, fakeRoot.context);
    QVERIFY(!callerFake.value.has_value());
    QCOMPARE(callerFake.error, HostRuntimeConfigError::UnsafePath);

    ValidArguments hostOutside;
    QVERIFY(!hostOutside.values.isEmpty());
    const QString outsideHostDirectory = hostOutside.root.filePath(
        QStringLiteral("outside-host"));
    const QString outsideHost = QDir(outsideHostDirectory).filePath(
        QStringLiteral("qbrowser-host.exe"));
    QVERIFY(QDir().mkpath(outsideHostDirectory));
    QVERIFY(QFile::copy(currentExecutablePath(), outsideHost));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(outsideHostDirectory, true));
    QVERIFY(protectPath(outsideHost));
#endif
    HostRuntimeParseContext outsideContext;
    outsideContext.currentHostExecutable = HostOwnedFileAuthority::open(outsideHost);
    QVERIFY(outsideContext.currentHostExecutable);
    const HostRuntimeConfigResult externalHost = HostRuntimeConfig::fromArguments(
        hostOutside.values, outsideContext);
    QVERIFY(!externalHost.value.has_value());
    QCOMPARE(externalHost.error, HostRuntimeConfigError::UnsafePath);

    ValidArguments workerOutside;
    QVERIFY(!workerOutside.values.isEmpty());
    const QString outsideRuntime = workerOutside.root.filePath(
        QStringLiteral("outside-runtime"));
    const QString outsideWorker = QDir(outsideRuntime).filePath(
        QStringLiteral("qbrowser-worker.exe"));
    QVERIFY(QDir().mkpath(outsideRuntime));
    QVERIFY(createProtectedFile(outsideWorker, QByteArrayLiteral("worker")));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(outsideRuntime, true));
#endif
    QVERIFY(setArgument(workerOutside.values,
                        QStringLiteral("--worker-executable="), outsideWorker));
    const HostRuntimeConfigResult externalWorker =
        HostRuntimeConfig::fromArguments(workerOutside.values,
                                         workerOutside.context);
    QVERIFY(!externalWorker.value.has_value());
    QCOMPARE(externalWorker.error, HostRuntimeConfigError::UnsafePath);

    ValidArguments runtimeOutside;
    QVERIFY(!runtimeOutside.values.isEmpty());
    const QString externalRuntime = runtimeOutside.root.filePath(
        QStringLiteral("external-runtime"));
    QVERIFY(QDir().mkpath(externalRuntime));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(externalRuntime, true));
#endif
    runtimeOutside.values.push_back(
        QStringLiteral("--runtime-root=") + externalRuntime);
    const HostRuntimeConfigResult externalRuntimeResult =
        HostRuntimeConfig::fromArguments(runtimeOutside.values,
                                         runtimeOutside.context);
    QVERIFY(!externalRuntimeResult.value.has_value());
    QCOMPARE(externalRuntimeResult.error, HostRuntimeConfigError::UnsafePath);

    ValidArguments keyOutside;
    QVERIFY(!keyOutside.values.isEmpty());
    const QString outsideTrust = keyOutside.root.filePath(
        QStringLiteral("outside-trust"));
    const QString outsideKey = QDir(outsideTrust).filePath(
        QStringLiteral("trusted-public.pem"));
    QVERIFY(QDir().mkpath(outsideTrust));
    QVERIFY(QFile::copy(keyOutside.publicKey, outsideKey));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(outsideTrust, true));
    QVERIFY(protectPath(outsideKey));
#endif
    QVERIFY(setArgument(keyOutside.values,
                        QStringLiteral("--trusted-public-key="), outsideKey));
    const HostRuntimeConfigResult externalKey = HostRuntimeConfig::fromArguments(
        keyOutside.values, keyOutside.context);
    QVERIFY(!externalKey.value.has_value());
    QCOMPARE(externalKey.error, HostRuntimeConfigError::UnsafePath);
}

void HostRuntimeConfigTest::acceptsDeploymentContainedAndProtectedExternalPackages()
{
    ValidArguments internal;
    QVERIFY(!internal.values.isEmpty());
    const QString internalPackage = QDir(internal.packagesDirectory).filePath(
        QStringLiteral("internal.qapkg"));
    QVERIFY(createProtectedFile(internalPackage, QByteArrayLiteral("package")));
    internal.values.push_back(
        QStringLiteral("--install-package=") + internalPackage);

    const HostRuntimeConfigResult internalResult =
        HostRuntimeConfig::fromArguments(internal.values, internal.context);
    QVERIFY2(internalResult.value.has_value(),
             qPrintable(internalResult.stableError));
    QVERIFY(!internalResult.value->installPackageAuthority());

    ValidArguments external;
    QVERIFY(!external.values.isEmpty());
    const QString sourceDirectory = external.root.filePath(
        QStringLiteral("external-packages"));
    const QString externalPackage = QDir(sourceDirectory).filePath(
        QStringLiteral("external.qapkg"));
    QVERIFY(QDir().mkpath(sourceDirectory));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(sourceDirectory, true));
#endif
    QVERIFY(createProtectedFile(externalPackage, QByteArrayLiteral("package")));
    external.values.push_back(
        QStringLiteral("--install-package=") + externalPackage);

    const HostRuntimeConfigResult externalResult =
        HostRuntimeConfig::fromArguments(external.values, external.context);
    QVERIFY2(externalResult.value.has_value(),
             qPrintable(externalResult.stableError));
    QVERIFY(externalResult.value->installPackageAuthority());
    QVERIFY(externalResult.value->installPackageAuthority()->revalidate());
#ifdef Q_OS_WIN
    const QString replacement = QDir(sourceDirectory).filePath(
        QStringLiteral("replacement.qapkg"));
    QVERIFY(createProtectedFile(replacement, QByteArrayLiteral("replacement")));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(replacement.utf16()),
        reinterpret_cast<LPCWSTR>(externalPackage.utf16()),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    QVERIFY(externalResult.value->installPackageAuthority()->revalidate());
    QVERIFY(applySecurity(externalPackage,
                          QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
    QVERIFY(!externalResult.value->installPackageAuthority()->revalidate());
#endif
}

void HostRuntimeConfigTest::rejectsUnsafeExternalPackageAuthorities()
{
    ValidArguments mutableParent;
    QVERIFY(!mutableParent.values.isEmpty());
    const QString packageInStore = QDir(mutableParent.store).filePath(
        QStringLiteral("mutable-parent.qapkg"));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(mutableParent.store, true));
#endif
    QVERIFY(createProtectedFile(packageInStore, QByteArrayLiteral("package")));
    mutableParent.values.push_back(
        QStringLiteral("--install-package=") + packageInStore);
    const HostRuntimeConfigResult parentOverlap =
        HostRuntimeConfig::fromArguments(mutableParent.values,
                                         mutableParent.context);
    QVERIFY(!parentOverlap.value.has_value());
    QCOMPARE(parentOverlap.error, HostRuntimeConfigError::OverlappingRoots);

#ifdef Q_OS_WIN
    ValidArguments permissive;
    QVERIFY(!permissive.values.isEmpty());
    const QString sourceDirectory = permissive.root.filePath(
        QStringLiteral("permissive-source"));
    const QString package = QDir(sourceDirectory).filePath(
        QStringLiteral("permissive.qapkg"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(protectPath(sourceDirectory, true));
    QVERIFY(createProtectedFile(package, QByteArrayLiteral("package")));
    QVERIFY(applySecurity(package,
                          QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
    const QByteArray before = fileSecurity(package);
    QVERIFY(!before.isEmpty());
    permissive.values.push_back(QStringLiteral("--install-package=") + package);
    const HostRuntimeConfigResult broadAcl = HostRuntimeConfig::fromArguments(
        permissive.values, permissive.context);
    QVERIFY(!broadAcl.value.has_value());
    QCOMPARE(broadAcl.error, HostRuntimeConfigError::UnsafePath);
    QCOMPARE(fileSecurity(package), before);

    ValidArguments hardLinked;
    QVERIFY(!hardLinked.values.isEmpty());
    const QString linkSourceDirectory = hardLinked.root.filePath(
        QStringLiteral("hardlink-source"));
    const QString linkedPackage = QDir(linkSourceDirectory).filePath(
        QStringLiteral("linked.qapkg"));
    const QString secondLink = QDir(linkSourceDirectory).filePath(
        QStringLiteral("second-link.qapkg"));
    QVERIFY(QDir().mkpath(linkSourceDirectory));
    QVERIFY(protectPath(linkSourceDirectory, true));
    QVERIFY(createProtectedFile(linkedPackage, QByteArrayLiteral("package")));
    QVERIFY(CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(secondLink.utf16()),
        reinterpret_cast<LPCWSTR>(linkedPackage.utf16()), nullptr));
    hardLinked.values.push_back(
        QStringLiteral("--install-package=") + linkedPackage);
    const HostRuntimeConfigResult multipleLinks =
        HostRuntimeConfig::fromArguments(hardLinked.values, hardLinked.context);
    QVERIFY(!multipleLinks.value.has_value());
    QCOMPARE(multipleLinks.error, HostRuntimeConfigError::UnsafePath);
#endif
}

void HostRuntimeConfigTest::rejectsExternalPackageParentOverlappingMutableRoot_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::newRow("package-store") << QStringLiteral("--package-store=");
    QTest::newRow("sandbox-temp") << QStringLiteral("--sandbox-temp=");
    QTest::newRow("telemetry") << QStringLiteral("--telemetry-directory=");
    QTest::newRow("capability-storage")
        << QStringLiteral("--storage-directory=");
    QTest::newRow("browser-state")
        << QStringLiteral("--browser-state-directory=");
}

void HostRuntimeConfigTest::rejectsExternalPackageParentOverlappingMutableRoot()
{
    QFETCH(QString, prefix);
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const qsizetype rootIndex = argumentIndex(arguments.values, prefix);
    QVERIFY(rootIndex >= 0);
    const QString mutableRoot = arguments.values.at(rootIndex).sliced(
        prefix.size());
    const QString package = QDir(mutableRoot).filePath(
        QStringLiteral("overlapping-source.qapkg"));
    QVERIFY(writeFile(package, QByteArrayLiteral("package")));
    arguments.values.push_back(
        QStringLiteral("--install-package=") + package);

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);
}

void HostRuntimeConfigTest::rejectsMutableRootsInsideDeployment_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::newRow("package-store") << QStringLiteral("--package-store=");
    QTest::newRow("sandbox-temp") << QStringLiteral("--sandbox-temp=");
    QTest::newRow("telemetry") << QStringLiteral("--telemetry-directory=");
    QTest::newRow("capability-storage")
        << QStringLiteral("--storage-directory=");
}

void HostRuntimeConfigTest::rejectsMutableRootsInsideDeployment()
{
    QFETCH(QString, prefix);
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const QString nested = QDir(arguments.deployment).filePath(
        QStringLiteral("mutable-inside-deployment"));
    QVERIFY(QDir().mkpath(nested));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(nested, true));
#endif
    QVERIFY(setArgument(arguments.values, prefix, nested));

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);
}

void HostRuntimeConfigTest::rejectsBrowserStateOverlap_data()
{
    QTest::addColumn<QString>("targetName");
    QTest::addColumn<int>("relation");
    const QStringList targets{
        QStringLiteral("deployment"), QStringLiteral("package-store"),
        QStringLiteral("sandbox-temp"), QStringLiteral("telemetry"),
        QStringLiteral("capability-storage"), QStringLiteral("runtime"),
        QStringLiteral("trusted-key-parent"),
        QStringLiteral("install-package-parent")};
    const QStringList relations{QStringLiteral("equal"),
                                QStringLiteral("contains"),
                                QStringLiteral("contained-by")};
    for (qsizetype target = 0; target < targets.size(); ++target) {
        for (qsizetype relation = 0; relation < relations.size(); ++relation) {
            const QByteArray row = (targets[target] + QLatin1Char('-')
                                    + relations[relation]).toUtf8();
            QTest::newRow(row.constData()) << targets[target]
                                           << static_cast<int>(relation);
        }
    }
}

void HostRuntimeConfigTest::rejectsBrowserStateOverlap()
{
    QFETCH(QString, targetName);
    QFETCH(int, relation);
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    QString target;
    if (targetName == QLatin1String("deployment")) {
        target = arguments.deployment;
    } else if (targetName == QLatin1String("package-store")) {
        target = arguments.store;
    } else if (targetName == QLatin1String("sandbox-temp")) {
        target = arguments.sandboxTemp;
    } else if (targetName == QLatin1String("telemetry")) {
        target = arguments.telemetry;
    } else if (targetName == QLatin1String("capability-storage")) {
        target = arguments.storage;
    } else if (targetName == QLatin1String("runtime")) {
        target = arguments.runtime;
    } else if (targetName == QLatin1String("trusted-key-parent")) {
        target = arguments.trustDirectory;
    } else {
        target = arguments.root.filePath(QStringLiteral("package-source"));
        const QString package = QDir(target).filePath(QStringLiteral("source.qapkg"));
        QVERIFY(QDir().mkpath(target));
#ifdef Q_OS_WIN
        QVERIFY(protectPath(target, true));
#endif
        QVERIFY(createProtectedFile(package, QByteArrayLiteral("package")));
        arguments.values.push_back(
            QStringLiteral("--install-package=") + package);
    }

    QString state = target;
    if (relation == 1) {
        state = QFileInfo(target).absolutePath();
    } else if (relation == 2) {
        state = QDir(target).filePath(QStringLiteral("browser-state-child"));
        QVERIFY(QDir().mkpath(state));
    }
#ifdef Q_OS_WIN
    QVERIFY(protectPath(state, true));
#endif
    QVERIFY(setArgument(arguments.values,
                        QStringLiteral("--browser-state-directory="), state));

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);
}

void HostRuntimeConfigTest::rejectsPermissiveProtectedRootsWithoutChangingPermissions_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("member");
    QTest::newRow("deployment") << QStringLiteral("--deployment-root=")
                                 << QStringLiteral("deployment");
    QTest::newRow("browser-state")
        << QStringLiteral("--browser-state-directory=")
        << QStringLiteral("browser-state");
}

void HostRuntimeConfigTest::rejectsPermissiveProtectedRootsWithoutChangingPermissions()
{
#ifndef Q_OS_WIN
    QSKIP("Windows ACL validation is Windows-specific");
#else
    QFETCH(QString, prefix);
    QFETCH(QString, member);
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const QString path = member == QLatin1String("deployment")
        ? arguments.deployment : arguments.browserState;
    QVERIFY(argumentIndex(arguments.values, prefix) >= 0);
    QVERIFY(applySecurity(
        path, QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;WD)")));
    const QByteArray before = fileSecurity(path);
    QVERIFY(!before.isEmpty());

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::UnsafePath);
    QCOMPARE(fileSecurity(path), before);
#endif
}

void HostRuntimeConfigTest::rejectsReparseAncestors_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("member");
    QTest::newRow("deployment") << QStringLiteral("--deployment-root=")
                                 << QStringLiteral("deployment");
    QTest::newRow("browser-state")
        << QStringLiteral("--browser-state-directory=")
        << QStringLiteral("browser-state");
}

void HostRuntimeConfigTest::rejectsReparseAncestors()
{
#ifndef Q_OS_WIN
    QSKIP("Windows reparse validation is Windows-specific");
#else
    QFETCH(QString, prefix);
    QFETCH(QString, member);
    ValidArguments arguments;
    QTemporaryDir aliases;
    QVERIFY(!arguments.values.isEmpty());
    QVERIFY(aliases.isValid());
    const QString target = member == QLatin1String("deployment")
        ? arguments.deployment : arguments.browserState;
    const QString link = aliases.filePath(QStringLiteral("junction"));
    const int linked = QProcess::execute(
        QStringLiteral("cmd.exe"),
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"),
         QStringLiteral("/J"), QDir::toNativeSeparators(link),
         QDir::toNativeSeparators(arguments.root.path())});
    if (linked != 0) QSKIP("Junction creation unavailable");
    const QString supplied = QDir(link).filePath(
        QDir(arguments.root.path()).relativeFilePath(target));
    QVERIFY(setArgument(arguments.values, prefix, supplied));

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::UnsafePath);
#endif
}

void HostRuntimeConfigTest::parsingPreservesProtectedAclsAndAuthoritiesBlockReplacement()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable authority validation is Windows-specific");
#else
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const QByteArray deploymentBefore = fileSecurity(arguments.deployment);
    const QByteArray stateBefore = fileSecurity(arguments.browserState);
    QVERIFY(!deploymentBefore.isEmpty());
    QVERIFY(!stateBefore.isEmpty());

    const HostRuntimeConfigResult result = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
    QVERIFY2(result.value.has_value(), qPrintable(result.stableError));
    QCOMPARE(fileSecurity(arguments.deployment), deploymentBefore);
    QCOMPARE(fileSecurity(arguments.browserState), stateBefore);
    QVERIFY(result.value->deploymentAuthority());
    QVERIFY(result.value->browserStateAuthority());
    QVERIFY(result.value->deploymentAuthority()->revalidate());
    QVERIFY(result.value->browserStateAuthority()->revalidate());

    const QString movedDeployment = arguments.root.filePath(
        QStringLiteral("moved-deployment"));
    const QString movedState = arguments.root.filePath(
        QStringLiteral("moved-browser-state"));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(arguments.deployment.utf16()),
        reinterpret_cast<LPCWSTR>(movedDeployment.utf16()),
        MOVEFILE_WRITE_THROUGH));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(arguments.browserState.utf16()),
        reinterpret_cast<LPCWSTR>(movedState.utf16()),
        MOVEFILE_WRITE_THROUGH));

    const QString replacementDeployment = arguments.root.filePath(
        QStringLiteral("replacement-deployment"));
    const QString replacementState = arguments.root.filePath(
        QStringLiteral("replacement-browser-state"));
    QVERIFY(QDir().mkpath(replacementDeployment));
    QVERIFY(QDir().mkpath(replacementState));
    QVERIFY(protectPath(replacementDeployment, true));
    QVERIFY(protectPath(replacementState, true));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(replacementDeployment.utf16()),
        reinterpret_cast<LPCWSTR>(arguments.deployment.utf16()),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(replacementState.utf16()),
        reinterpret_cast<LPCWSTR>(arguments.browserState.utf16()),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    QVERIFY(result.value->deploymentAuthority()->revalidate());
    QVERIFY(result.value->browserStateAuthority()->revalidate());

    QVERIFY(applySecurity(
        arguments.browserState,
        QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;WD)")));
    QVERIFY(!result.value->browserStateAuthority()->revalidate());
    QVERIFY(result.value->deploymentAuthority()->revalidate());
    QVERIFY(applySecurity(
        arguments.deployment,
        QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;WD)")));
    QVERIFY(!result.value->deploymentAuthority()->revalidate());
#endif
}

void HostRuntimeConfigTest::requiresIndependentStorageRoot()
{
    ValidArguments arguments;
    QVERIFY(!arguments.values.isEmpty());
    const qsizetype storageIndex = argumentIndex(
        arguments.values, QStringLiteral("--storage-directory="));
    QVERIFY(storageIndex >= 0);
    arguments.values.removeAt(storageIndex);
    const HostRuntimeConfigResult missing = HostRuntimeConfig::fromArguments(
        arguments.values, arguments.context);
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
        arguments.values, arguments.context);

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
    const QString replacement = QDir(arguments.trustDirectory).filePath(
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
        oversized.values, oversized.context);
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
        malformed.values, malformed.context);
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
        HostRuntimeConfig::fromArguments(junction.values, junction.context);
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
        arguments.values, arguments.context);

    QVERIFY(!result.value.has_value());
    QCOMPARE(result.error, HostRuntimeConfigError::OverlappingRoots);

    ValidArguments keyArguments;
    QVERIFY(!keyArguments.values.isEmpty());
    const QString trustParent = QDir(keyArguments.deployment).filePath(
        QStringLiteral("isolated-trust"));
    const QString nestedTelemetry = QDir(trustParent).filePath(
        QStringLiteral("telemetry"));
    const QString nestedKey = QDir(trustParent).filePath(
        QStringLiteral("trusted.pem"));
    QVERIFY(QDir().mkpath(nestedTelemetry));
    QVERIFY(QFile::copy(keyArguments.publicKey, nestedKey));
#ifdef Q_OS_WIN
    QVERIFY(protectPath(trustParent, true));
    QVERIFY(protectPath(nestedTelemetry, true));
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
        keyArguments.values, keyArguments.context);
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
    QVERIFY(shell.value->deploymentRoot().isEmpty());
    QVERIFY(shell.value->browserStateDirectory().isEmpty());
    QVERIFY(!shell.value->deploymentAuthority());
    QVERIFY(!shell.value->browserStateAuthority());
    QVERIFY(!shell.value->installPackageAuthority());

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
        configured, arguments.context);
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
    QCOMPARE(result.value->deploymentRoot(),
             QDir(arguments.deployment).canonicalPath());
    QCOMPARE(result.value->browserStateDirectory(),
             QDir(arguments.browserState).canonicalPath());
    QVERIFY(result.value->deploymentAuthority());
    QVERIFY(result.value->browserStateAuthority());
    QVERIFY(result.value->deploymentAuthority()->revalidate());
    QVERIFY(result.value->browserStateAuthority()->revalidate());
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
        arguments.values, arguments.context);
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
        ambient, arguments.context);
    QVERIFY(!ambientResult.value.has_value());
    QCOMPARE(ambientResult.error, HostRuntimeConfigError::UnsafePath);

    QStringList overlapping = arguments.values;
    const qsizetype tempIndex = argumentIndex(
        overlapping, QStringLiteral("--sandbox-temp="));
    QVERIFY(tempIndex >= 0);
    overlapping[tempIndex] = QStringLiteral("--sandbox-temp=") + arguments.store;
    const HostRuntimeConfigResult overlapResult = HostRuntimeConfig::fromArguments(
        overlapping, arguments.context);
    QVERIFY(!overlapResult.value.has_value());
    QCOMPARE(overlapResult.error, HostRuntimeConfigError::OverlappingRoots);
}

QTEST_APPLESS_MAIN(HostRuntimeConfigTest)

#include "tst_host_runtime_config.moc"
