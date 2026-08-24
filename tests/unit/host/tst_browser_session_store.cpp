#include "BrowserSessionStore.h"
#include "HostOwnedStateDirectory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <Sddl.h>
#include <qt_windows.h>
#endif

namespace
{
bool writeFile(const QString &path, const QByteArray &bytes);

#ifdef Q_OS_WIN
bool protectPath(const QString &path, const bool container = false)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &owner,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
    if (queried != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }

    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemBytes = sizeof(systemBuffer);
    if (CreateWellKnownSid(WinLocalSystemSid,
                           nullptr,
                           systemBuffer,
                           &systemBytes)
        == FALSE) {
        LocalFree(descriptor);
        return false;
    }

    EXPLICIT_ACCESSW entries[2]{};
    for (EXPLICIT_ACCESSW &entry : entries) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = container
            ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
            : NO_INHERITANCE;
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
              nullptr,
              nullptr,
              dacl,
              nullptr)
        : aclResult;
    if (dacl != nullptr) LocalFree(dacl);
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
}

bool applySecurity(const QString &path, const QString &sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)
        == FALSE) {
        return false;
    }
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    const bool valid = GetSecurityDescriptorDacl(
                           descriptor, &present, &dacl, &defaulted)
            != FALSE
        && present != FALSE;
    const DWORD applied = valid
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr,
              nullptr,
              dacl,
              nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
}

bool grantLpacRead(const QString &path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL existing = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &existing,
        nullptr,
        &descriptor);
    if (queried != ERROR_SUCCESS || descriptor == nullptr || existing == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    BYTE lpacBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD lpacBytes = sizeof(lpacBuffer);
    if (CreateWellKnownSid(WinBuiltinAnyPackageSid,
                           nullptr,
                           lpacBuffer,
                           &lpacBytes)
        == FALSE) {
        LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_GENERIC_READ;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(lpacBuffer);
    PACL updated = nullptr;
    const DWORD aclResult = SetEntriesInAclW(1, &entry, existing, &updated);
    const DWORD applied = aclResult == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(
              const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
              SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr,
              nullptr,
              updated,
              nullptr)
        : aclResult;
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
    return applied == ERROR_SUCCESS;
}

bool createHardLink(const QString &link, const QString &target)
{
    return CreateHardLinkW(
               reinterpret_cast<LPCWSTR>(link.utf16()),
               reinterpret_cast<LPCWSTR>(target.utf16()),
               nullptr)
        != FALSE;
}

bool createSymbolicLink(const QString &link, const QString &target)
{
    return CreateSymbolicLinkW(
               reinterpret_cast<LPCWSTR>(link.utf16()),
               reinterpret_cast<LPCWSTR>(target.utf16()),
               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        != FALSE;
}

bool replaceLeaf(const QString &path, const QByteArray &replacement)
{
    const QString moved = path + QStringLiteral(".moved");
    if (MoveFileExW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            reinterpret_cast<LPCWSTR>(moved.utf16()),
            MOVEFILE_WRITE_THROUGH)
        == FALSE) {
        return false;
    }
    return writeFile(path, replacement);
}
#endif

class StateDirectoryFixture final
{
public:
    StateDirectoryFixture()
    {
        if (!root.isValid()) return;
        path = root.filePath(QStringLiteral("browser-state"));
        if (!QDir().mkpath(path)) return;
#ifdef Q_OS_WIN
        if (!protectPath(path, true)) return;
#endif
        authority = HostOwnedStateDirectory::open(path);
    }

    QTemporaryDir root;
    QString path;
    std::shared_ptr<const HostOwnedStateDirectory> authority;
};

const QString FirstId = QStringLiteral("11111111111111111111111111111111");
const QString SecondId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        return false;
    }
    file.close();
#ifdef Q_OS_WIN
    return protectPath(path);
#else
    return true;
#endif
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

BrowserWindowSnapshot sampleSnapshot()
{
    BrowserTabSnapshot first;
    first.id = FirstId;
    first.kind = BrowserTabKind::Host;
    first.title = QStringLiteral("New \"tab\"");
    first.address = QStringLiteral("qbrowser://newtab");
    first.history = {first.address};
    first.historyIndex = 0;

    BrowserTabSnapshot second;
    second.id = SecondId;
    second.kind = BrowserTabKind::Web;
    second.title = QString::fromUtf8("Orders ✓");
    second.address = QStringLiteral("app://pilot/orders?view=recent");
    second.history = {QStringLiteral("app://pilot/home"), second.address};
    second.historyIndex = 1;

    return {QRect(-20, 40, 1280, 800), SecondId, {first, second}};
}

QByteArray sampleDocument()
{
    return QByteArrayLiteral(
        "{\"version\":1,\"window\":{\"x\":-20,\"y\":40,\"width\":1280,\"height\":800},"
        "\"activeTabId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"tabs\":[{"
        "\"id\":\"11111111111111111111111111111111\",\"kind\":\"host\","
        "\"title\":\"New \\\"tab\\\"\",\"address\":\"qbrowser://newtab\","
        "\"history\":[\"qbrowser://newtab\"],\"historyIndex\":0},{"
        "\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"kind\":\"web\","
        "\"title\":\"Orders ")
        + QString::fromUtf8("✓").toUtf8()
        + QByteArrayLiteral(
            "\",\"address\":\"app://pilot/orders?view=recent\",\"history\":["
            "\"app://pilot/home\",\"app://pilot/orders?view=recent\"],"
            "\"historyIndex\":1}]}\n");
}

QByteArray oneTabDocument(const QByteArray &title = QByteArrayLiteral("New tab"),
                          const QByteArray &address = QByteArrayLiteral("qbrowser://newtab"),
                          const QByteArray &history = QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          const QByteArray &index = QByteArrayLiteral("0"),
                          const QByteArray &id = QByteArrayLiteral("11111111111111111111111111111111"))
{
    return QByteArrayLiteral(
               "{\"version\":1,\"window\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":800},"
               "\"activeTabId\":\"")
        + id
        + QByteArrayLiteral("\",\"tabs\":[{\"id\":\"") + id
        + QByteArrayLiteral(
            "\",\"kind\":\"host\",\"title\":\"")
        + title + QByteArrayLiteral("\",\"address\":\"") + address
        + QByteArrayLiteral("\",\"history\":") + history
        + QByteArrayLiteral(",\"historyIndex\":") + index
        + QByteArrayLiteral("}]}\n");
}

BrowserSessionLoadResult loadDocument(StateDirectoryFixture &fixture,
                                      const QByteArray &bytes)
{
    if (!fixture.authority) return {};
    const QString path = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    if (!writeFile(path, bytes)) return {};
    BrowserSessionStore store(fixture.authority);
    return store.load();
}

#ifdef Q_BROWSER_HOST_TESTING
class SessionHooksReset final
{
public:
    SessionHooksReset()
    {
        qbrowser_host_testing::resetBrowserSessionStoreTestHooks();
    }

    ~SessionHooksReset()
    {
        qbrowser_host_testing::resetBrowserSessionStoreTestHooks();
    }

    SessionHooksReset(const SessionHooksReset &) = delete;
    SessionHooksReset &operator=(const SessionHooksReset &) = delete;
};
#endif
}

class BrowserSessionStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void missingFile();
    void canonicalRoundTrip();
    void rejectsNonCanonicalSchema_data();
    void rejectsNonCanonicalSchema();
    void rejectsMalformedAndBoundViolations_data();
    void rejectsMalformedAndBoundViolations();
    void rejectsOversizedDocument();
    void sanitizesRuntimeTitleBeforeSave();
    void rejectsOversizedInMemoryTitleBeforeSanitization();
    void returnsOnTooLongAddressBeforeAuthorityScan();
    void validatesAddressesBeforeCurrentHistoryEquality();
    void rejectsNoncanonicalLoadedTitle();
    void handlesSurrogateBoundariesAndRejectsLoneSurrogates();
    void rejectsOneMalformedTabWithoutPartialRecovery();
    void resolvesUsingCurrentAuthority();
    void atomicReplacementFailurePreservesPreviousFile();
    void rejectsDangerousExistingTargetBeforeSave();
    void detectsTargetIdentityRaceBeforeAtomicReplace();
    void rejectsDangerousPrimaryLeaves_data();
    void rejectsDangerousPrimaryLeaves();
    void rejectsDangerousCorruptLeaves_data();
    void rejectsDangerousCorruptLeaves();
    void detectsLeafIdentityAndDaclRaces_data();
    void detectsLeafIdentityAndDaclRaces();
    void blocksParentReplacementRaces_data();
    void blocksParentReplacementRaces();
    void detectsDaclRaceAfterAtomicReplace();
    void preservesAtMostOneBoundedCorruptCopyWithoutParsing();
    void rejectsOversizedExistingCorruptLeaf();
};

void BrowserSessionStoreTest::missingFile()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);

    BrowserSessionStore store(fixture.authority);
    const BrowserSessionLoadResult result = store.load();

    QCOMPARE(result.status, BrowserSessionLoadStatus::Missing);
    QVERIFY(!result.snapshot.has_value());
    QVERIFY(result.stableError.isEmpty());
#endif
}

void BrowserSessionStoreTest::canonicalRoundTrip()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);
    const BrowserWindowSnapshot expected = sampleSnapshot();

    const BrowserSessionSaveResult saved = store.save(expected);
    QCOMPARE(saved.status, BrowserSessionSaveStatus::Saved);
    QVERIFY(saved.stableError.isEmpty());
    QCOMPARE(readFile(QDir(fixture.path).filePath(
                 QStringLiteral("browser-session.json"))),
             sampleDocument());

    const BrowserSessionLoadResult loaded = store.load();
    QCOMPARE(loaded.status, BrowserSessionLoadStatus::Loaded);
    QVERIFY(loaded.snapshot.has_value());
    QCOMPARE(*loaded.snapshot, expected);
    QVERIFY(loaded.stableError.isEmpty());
#endif
}

void BrowserSessionStoreTest::rejectsNonCanonicalSchema_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QByteArray bytes = oneTabDocument();
    QTest::newRow("version")
        << QByteArray(bytes).replace("\"version\":1", "\"version\":2");
    QTest::newRow("root-key-order")
        << QByteArrayLiteral(
               "{\"window\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":800},"
               "\"version\":1,\"activeTabId\":\"11111111111111111111111111111111\","
               "\"tabs\":[{\"id\":\"11111111111111111111111111111111\","
               "\"kind\":\"host\",\"title\":\"New tab\","
               "\"address\":\"qbrowser://newtab\","
               "\"history\":[\"qbrowser://newtab\"],\"historyIndex\":0}]}\n");
    QTest::newRow("window-key-order")
        << QByteArray(bytes).replace(
               "{\"x\":0,\"y\":0,\"width\":1280,\"height\":800}",
               "{\"y\":0,\"x\":0,\"width\":1280,\"height\":800}");
    QTest::newRow("tab-key-order")
        << QByteArray(bytes).replace(
               "{\"id\":\"11111111111111111111111111111111\",\"kind\":\"host\"",
               "{\"kind\":\"host\",\"id\":\"11111111111111111111111111111111\"");
    QTest::newRow("whitespace")
        << QByteArray(bytes).replace("{\"version\"", "{ \"version\"");
    QTest::newRow("number-spelling")
        << QByteArray(bytes).replace("\"x\":0", "\"x\":0.0");
    QTest::newRow("alternate-escaping")
        << QByteArray(bytes).replace("qbrowser://newtab", "qbrowser:\\/\\/newtab");
    QTest::newRow("missing-final-newline") << bytes.chopped(1);
    QTest::newRow("extra-final-newline") << bytes + '\n';
    QTest::newRow("unknown-root")
        << QByteArray(bytes).replace(
               "{\"version\":1,",
               "{\"version\":1,\"credentials\":\"secret\",");
    QTest::newRow("unknown-window")
        << QByteArray(bytes).replace(
               "\"height\":800}", "\"height\":800,\"screen\":1}");
    QTest::newRow("unknown-tab")
        << QByteArray(bytes).replace(
               "\"historyIndex\":0", "\"historyIndex\":0,\"cookie\":\"secret\"");
    QTest::newRow("missing-tab-key")
        << QByteArray(bytes).replace(",\"title\":\"New tab\"", QByteArray());
    QTest::newRow("invalid-kind")
        << QByteArray(bytes).replace("\"kind\":\"host\"", "\"kind\":\"worker\"");
}

void BrowserSessionStoreTest::rejectsNonCanonicalSchema()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    QFETCH(QByteArray, bytes);
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const BrowserSessionLoadResult result = loadDocument(fixture, bytes);
    QCOMPARE(result.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!result.snapshot.has_value());
    QCOMPARE(result.stableError, QStringLiteral("host.session.corrupt"));
#endif
}

void BrowserSessionStoreTest::rejectsMalformedAndBoundViolations_data()
{
    QTest::addColumn<QByteArray>("bytes");
    const QByteArray valid = oneTabDocument();

    QTest::newRow("invalid-json") << QByteArrayLiteral("{not-json}\n");
    QTest::newRow("nonintegral-coordinate")
        << QByteArray(valid).replace("\"x\":0", "\"x\":0.5");
    QTest::newRow("coordinate-low")
        << QByteArray(valid).replace("\"x\":0", "\"x\":-1000001");
    QTest::newRow("coordinate-high")
        << QByteArray(valid).replace("\"y\":0", "\"y\":1000001");
    QTest::newRow("width-low")
        << QByteArray(valid).replace("\"width\":1280", "\"width\":319");
    QTest::newRow("height-high")
        << QByteArray(valid).replace("\"height\":800", "\"height\":32769");
    QTest::newRow("uppercase-id")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          QByteArrayLiteral("0"),
                          QByteArrayLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    QTest::newRow("short-id")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          QByteArrayLiteral("0"),
                          QByteArrayLiteral("1111111111111111111111111111111"));
    QTest::newRow("active-id-not-found")
        << QByteArray(valid).replace(
               "\"activeTabId\":\"11111111111111111111111111111111\"",
               "\"activeTabId\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"");
    QTest::newRow("duplicate-id")
        << QByteArray(sampleDocument()).replace(
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
               "11111111111111111111111111111111");
    QTest::newRow("negative-index")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          QByteArrayLiteral("-1"));
    QTest::newRow("nonintegral-index")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          QByteArrayLiteral("0.5"));
    QTest::newRow("index-out-of-range")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"qbrowser://newtab\"]"),
                          QByteArrayLiteral("1"));
    QTest::newRow("address-not-current-history")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[\"app://pilot/home\"]"));
    QTest::newRow("unsupported-address")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("https://example.test/"),
                          QByteArrayLiteral("[\"https://example.test/\"]"));
    QTest::newRow("empty-history")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("qbrowser://newtab"),
                          QByteArrayLiteral("[]"));
    QTest::newRow("empty-tabs")
        << QByteArrayLiteral(
               "{\"version\":1,\"window\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":800},"
               "\"activeTabId\":\"11111111111111111111111111111111\",\"tabs\":[]}\n");

    QByteArray longAddress("app://pilot/");
    longAddress.append(QByteArray(2048, 'a'));
    QTest::newRow("overlong-address")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          longAddress,
                          QByteArrayLiteral("[\"") + longAddress
                              + QByteArrayLiteral("\"]"));

    QByteArray longTitle(257, 't');
    QTest::newRow("overlong-title") << oneTabDocument(longTitle);

    QByteArray histories = "[";
    for (int index = 0; index < 257; ++index) {
        if (index != 0) histories.append(',');
        histories.append("\"app://pilot/p");
        histories.append(QByteArray::number(index));
        histories.append("\"");
    }
    histories.append(']');
    QTest::newRow("257-history-items")
        << oneTabDocument(QByteArrayLiteral("New tab"),
                          QByteArrayLiteral("app://pilot/p256"),
                          histories,
                          QByteArrayLiteral("256"));

    QByteArray tabs;
    for (int index = 0; index < 17; ++index) {
        if (index != 0) tabs.append(',');
        const QByteArray id = QByteArray::number(index + 1, 16).rightJustified(32, '0');
        tabs.append("{\"id\":\"");
        tabs.append(id);
        tabs.append("\",\"kind\":\"host\",\"title\":\"New tab\","
                    "\"address\":\"qbrowser://newtab\","
                    "\"history\":[\"qbrowser://newtab\"],\"historyIndex\":0}");
    }
    QTest::newRow("17-tabs")
        << QByteArrayLiteral(
               "{\"version\":1,\"window\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":800},"
               "\"activeTabId\":\"00000000000000000000000000000001\",\"tabs\":[")
               + tabs + QByteArrayLiteral("]}\n");
}

void BrowserSessionStoreTest::rejectsMalformedAndBoundViolations()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    QFETCH(QByteArray, bytes);
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const BrowserSessionLoadResult result = loadDocument(fixture, bytes);
    QCOMPARE(result.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!result.snapshot.has_value());
#endif
}

void BrowserSessionStoreTest::rejectsOversizedDocument()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const BrowserSessionLoadResult result = loadDocument(
        fixture, QByteArray(16 * 1024 * 1024 + 1, 'x'));
    QCOMPARE(result.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!result.snapshot.has_value());
#endif
}

void BrowserSessionStoreTest::sanitizesRuntimeTitleBeforeSave()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserWindowSnapshot snapshot = sampleSnapshot();
    snapshot.tabs[0].title = QString(255, u'a') + QString(QChar(0x0001))
        + QString::fromUcs4(U"\U0001f642") + QStringLiteral("ignored");
    BrowserSessionStore store(fixture.authority);

    QCOMPARE(store.save(snapshot).status, BrowserSessionSaveStatus::Saved);
    const BrowserSessionLoadResult loaded = store.load();
    QCOMPARE(loaded.status, BrowserSessionLoadStatus::Loaded);
    QVERIFY(loaded.snapshot.has_value());
    QCOMPARE(loaded.snapshot->tabs.at(0).title, QString(255, u'a'));
#endif
}

void BrowserSessionStoreTest::rejectsOversizedInMemoryTitleBeforeSanitization()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);

    int titleSanitizations = 0;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.beforeTitleSanitize = [&titleSanitizations]() {
        ++titleSanitizations;
    };
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    const BrowserSessionResolveResult control = store.validateAndResolve(
        sampleSnapshot(),
        [](const BrowserAddress &) {
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });
    QVERIFY(control.snapshot.has_value());
    QVERIFY(titleSanitizations > 0);

    titleSanitizations = 0;
    BrowserWindowSnapshot oversized = sampleSnapshot();
    oversized.tabs[0].title = QString(
        BrowserTabModel::MaxTitleCodeUnits * 16 + 1, u'a');
    int resolverCalls = 0;
    const BrowserSessionSaveResult saved = store.save(oversized);
    const BrowserSessionResolveResult resolved = store.validateAndResolve(
        oversized,
        [&resolverCalls](const BrowserAddress &) {
            ++resolverCalls;
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });

    QCOMPARE(saved.status, BrowserSessionSaveStatus::InvalidSnapshot);
    QVERIFY(!resolved.snapshot.has_value());
    QCOMPARE(resolverCalls, 0);
    QCOMPARE(titleSanitizations, 0);
#endif
}

void BrowserSessionStoreTest::returnsOnTooLongAddressBeforeAuthorityScan()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);

    int authorityScans = 0;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.beforeAppAuthorityScan = [&authorityScans]() {
        ++authorityScans;
    };
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    const BrowserSessionResolveResult control = store.validateAndResolve(
        sampleSnapshot(),
        [](const BrowserAddress &) {
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });
    QVERIFY(control.snapshot.has_value());
    QVERIFY(authorityScans > 0);

    authorityScans = 0;
    const QString tooLong = QStringLiteral("app://")
        + QString(1024, QChar(0x00e9));
    BrowserWindowSnapshot oversized = sampleSnapshot();
    oversized.tabs[0].address = QString(tooLong.constData(), tooLong.size());
    oversized.tabs[0].history = {
        QString(tooLong.constData(), tooLong.size())};
    int resolverCalls = 0;
    const BrowserSessionSaveResult saved = store.save(oversized);
    const BrowserSessionResolveResult resolved = store.validateAndResolve(
        oversized,
        [&resolverCalls](const BrowserAddress &) {
            ++resolverCalls;
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });

    QCOMPARE(saved.status, BrowserSessionSaveStatus::InvalidSnapshot);
    QVERIFY(!resolved.snapshot.has_value());
    QCOMPARE(resolverCalls, 0);
    QCOMPARE(authorityScans, 0);
#endif
}

void BrowserSessionStoreTest::validatesAddressesBeforeCurrentHistoryEquality()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);

    int currentHistoryComparisons = 0;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.beforeCurrentHistoryEquality = [&currentHistoryComparisons]() {
        ++currentHistoryComparisons;
    };
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    const BrowserSessionResolveResult control = store.validateAndResolve(
        sampleSnapshot(),
        [](const BrowserAddress &) {
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });
    QVERIFY(control.snapshot.has_value());
    QVERIFY(currentHistoryComparisons > 0);

    currentHistoryComparisons = 0;
    const QString tooLong = QStringLiteral("app://") + QString(4096, u'a');
    BrowserWindowSnapshot oversized = sampleSnapshot();
    oversized.tabs[0].address = QString(tooLong.constData(), tooLong.size());
    oversized.tabs[0].history = {
        QString(tooLong.constData(), tooLong.size())};
    int resolverCalls = 0;
    const BrowserSessionSaveResult saved = store.save(oversized);
    const BrowserSessionResolveResult resolved = store.validateAndResolve(
        oversized,
        [&resolverCalls](const BrowserAddress &) {
            ++resolverCalls;
            return std::optional<BrowserTabKind>(BrowserTabKind::Host);
        });

    QCOMPARE(saved.status, BrowserSessionSaveStatus::InvalidSnapshot);
    QVERIFY(!resolved.snapshot.has_value());
    QCOMPARE(resolverCalls, 0);
    QCOMPARE(currentHistoryComparisons, 0);
#endif
}

void BrowserSessionStoreTest::rejectsNoncanonicalLoadedTitle()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const BrowserSessionLoadResult control = loadDocument(
        fixture, oneTabDocument(QByteArrayLiteral("unsafe\\u0001title")));
    QCOMPARE(control.status, BrowserSessionLoadStatus::Corrupt);

    StateDirectoryFixture overlong;
    QVERIFY(overlong.authority);
    const BrowserSessionLoadResult tooLong = loadDocument(
        overlong, oneTabDocument(QByteArray(257, 'a')));
    QCOMPARE(tooLong.status, BrowserSessionLoadStatus::Corrupt);
#endif
}

void BrowserSessionStoreTest::handlesSurrogateBoundariesAndRejectsLoneSurrogates()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture validFixture;
    QVERIFY(validFixture.authority);
    BrowserWindowSnapshot valid = sampleSnapshot();
    valid.tabs[0].title = QString(254, u'a') + QString::fromUcs4(U"\U0001f642");
    BrowserSessionStore validStore(validFixture.authority);
    QCOMPARE(validStore.save(valid).status, BrowserSessionSaveStatus::Saved);
    const BrowserSessionLoadResult loaded = validStore.load();
    QCOMPARE(loaded.status, BrowserSessionLoadStatus::Loaded);
    QVERIFY(loaded.snapshot.has_value());
    QCOMPARE(loaded.snapshot->tabs.at(0).title, valid.tabs.at(0).title);

    StateDirectoryFixture loneFixture;
    QVERIFY(loneFixture.authority);
    BrowserWindowSnapshot lone = sampleSnapshot();
    lone.tabs[0].title = QString(QChar(0xd800));
    BrowserSessionStore loneStore(loneFixture.authority);
    QCOMPARE(loneStore.save(lone).status,
             BrowserSessionSaveStatus::InvalidSnapshot);

    StateDirectoryFixture loadedLoneFixture;
    QVERIFY(loadedLoneFixture.authority);
    const BrowserSessionLoadResult loadedLone = loadDocument(
        loadedLoneFixture,
        oneTabDocument(QByteArrayLiteral("bad\\ud800title")));
    QCOMPARE(loadedLone.status, BrowserSessionLoadStatus::Corrupt);
#endif
}

void BrowserSessionStoreTest::rejectsOneMalformedTabWithoutPartialRecovery()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    QByteArray bytes = sampleDocument();
    bytes.replace("app://pilot/orders?view=recent",
                  "https://secret.example/");
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const BrowserSessionLoadResult result = loadDocument(fixture, bytes);
    QCOMPARE(result.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!result.snapshot.has_value());
#endif
}

void BrowserSessionStoreTest::resolvesUsingCurrentAuthority()
{
    BrowserWindowSnapshot raw = sampleSnapshot();
    raw.tabs[0].kind = BrowserTabKind::Web;
    raw.tabs[1].kind = BrowserTabKind::App;

    const auto appendRoute = [&raw](const QString &id,
                                    const BrowserTabKind persistedKind,
                                    const QString &authority) {
        BrowserTabSnapshot tab;
        tab.id = id;
        tab.kind = persistedKind;
        tab.title = authority;
        tab.address = QStringLiteral("app://") + authority
            + QStringLiteral("/home");
        tab.history = {tab.address};
        tab.historyIndex = 0;
        raw.tabs.append(std::move(tab));
    };
    appendRoute(QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
                BrowserTabKind::Web,
                QStringLiteral("signed"));
    appendRoute(QStringLiteral("cccccccccccccccccccccccccccccccc"),
                BrowserTabKind::App,
                QStringLiteral("removed"));
    appendRoute(QStringLiteral("dddddddddddddddddddddddddddddddd"),
                BrowserTabKind::Host,
                QStringLiteral("unsigned"));
    appendRoute(QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"),
                BrowserTabKind::Web,
                QStringLiteral("denied"));

    BrowserSessionStore store({});
    int calls = 0;
    const BrowserSessionResolveResult result = store.validateAndResolve(
        raw,
        [&calls](const BrowserAddress &address)
            -> std::optional<BrowserTabKind> {
            ++calls;
            if (address.canonical() == QLatin1String("qbrowser://newtab")) {
                return BrowserTabKind::Host;
            }
            if (address.canonical()
                == QLatin1String("app://pilot/orders?view=recent")) {
                return BrowserTabKind::Web;
            }
            if (address.canonical() == QLatin1String("app://signed/home")) {
                return BrowserTabKind::App;
            }
            return std::nullopt;
        });

    QVERIFY(result.snapshot.has_value());
    QVERIFY(result.stableError.isEmpty());
    QCOMPARE(calls, 6);
    QCOMPARE(result.snapshot->tabs.at(0).kind, BrowserTabKind::Host);
    QCOMPARE(result.snapshot->tabs.at(1).kind, BrowserTabKind::Web);
    QCOMPARE(result.snapshot->tabs.at(2).kind, BrowserTabKind::App);
    QCOMPARE(result.snapshot->tabs.at(3).kind, BrowserTabKind::TrustedError);
    QCOMPARE(result.snapshot->tabs.at(4).kind, BrowserTabKind::TrustedError);
    QCOMPARE(result.snapshot->tabs.at(5).kind, BrowserTabKind::TrustedError);
}

void BrowserSessionStoreTest::atomicReplacementFailurePreservesPreviousFile()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);
    const BrowserWindowSnapshot original = sampleSnapshot();
    QCOMPARE(store.save(original).status, BrowserSessionSaveStatus::Saved);
    const QString path = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QByteArray before = readFile(path);
    QVERIFY(!before.isEmpty());

    BrowserWindowSnapshot replacement = original;
    replacement.geometry.moveTo(55, 66);
    int publishAttempts = 0;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.beforeAtomicPublish = [&publishAttempts](const QString &) {
        ++publishAttempts;
    };
    hooks.failAtomicPublish = true;
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    const BrowserSessionSaveResult result = store.save(replacement);
    QCOMPARE(result.status, BrowserSessionSaveStatus::IoFailure);
    QCOMPARE(publishAttempts, 1);
    QCOMPARE(readFile(path), before);
    const BrowserSessionLoadResult loaded = store.load();
    QCOMPARE(loaded.status, BrowserSessionLoadStatus::Loaded);
    QVERIFY(loaded.snapshot.has_value());
    QCOMPARE(*loaded.snapshot, original);
#endif
}

void BrowserSessionStoreTest::rejectsDangerousExistingTargetBeforeSave()
{
#ifndef Q_OS_WIN
    QSKIP("Windows leaf validation is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString path = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    QVERIFY(writeFile(path, sampleDocument()));
    QVERIFY(applySecurity(
        path, QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
    const QByteArray before = readFile(path);

    BrowserWindowSnapshot replacement = sampleSnapshot();
    replacement.geometry.moveTo(10, 20);
    BrowserSessionStore store(fixture.authority);
    const BrowserSessionSaveResult result = store.save(replacement);

    QCOMPARE(result.status, BrowserSessionSaveStatus::IoFailure);
    QCOMPARE(readFile(path), before);
#endif
}

void BrowserSessionStoreTest::detectsTargetIdentityRaceBeforeAtomicReplace()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);
    QCOMPARE(store.save(sampleSnapshot()).status,
             BrowserSessionSaveStatus::Saved);
    const QString path = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QByteArray safeReplacement = oneTabDocument();
    bool raced = false;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.beforeAtomicPublish = [&raced, &safeReplacement](const QString &target) {
        raced = replaceLeaf(target, safeReplacement);
    };
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    BrowserWindowSnapshot replacement = sampleSnapshot();
    replacement.geometry.moveTo(30, 40);
    const BrowserSessionSaveResult result = store.save(replacement);
    QVERIFY(raced);
    QCOMPARE(result.status, BrowserSessionSaveStatus::IoFailure);
    QCOMPARE(readFile(path), safeReplacement);
#endif
}

void BrowserSessionStoreTest::rejectsDangerousPrimaryLeaves_data()
{
    QTest::addColumn<int>("leafType");
    QTest::newRow("reparse") << 0;
    QTest::newRow("hardlink") << 1;
    QTest::newRow("non-regular") << 2;
    QTest::newRow("permissive-dacl") << 3;
    QTest::newRow("lpac-allow") << 4;
    QTest::newRow("writable-inherited-outsider") << 5;
}

void BrowserSessionStoreTest::rejectsDangerousPrimaryLeaves()
{
#ifndef Q_OS_WIN
    QSKIP("Windows leaf validation is Windows-specific");
#else
    QFETCH(int, leafType);
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString leaf = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString source = QDir(fixture.path).filePath(
        QStringLiteral("source.json"));

    if (leafType == 0) {
        QVERIFY(writeFile(source, sampleDocument()));
        if (!createSymbolicLink(leaf, source)) {
            QSKIP("File symlink creation unavailable");
        }
    } else if (leafType == 1) {
        QVERIFY(writeFile(source, sampleDocument()));
        QVERIFY(createHardLink(leaf, source));
    } else if (leafType == 2) {
        QVERIFY(QDir().mkpath(leaf));
    } else {
        QVERIFY(writeFile(leaf, sampleDocument()));
        if (leafType == 3) {
            QVERIFY(applySecurity(
                leaf,
                QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
        } else if (leafType == 4) {
            QVERIFY(grantLpacRead(leaf));
        } else {
            QVERIFY(applySecurity(
                leaf,
                QStringLiteral("D:AI(A;ID;GW;;;WD)(A;;FA;;;SY)")));
        }
    }

    BrowserSessionStore store(fixture.authority);
    const BrowserSessionLoadResult result = store.load();
    QCOMPARE(result.status, BrowserSessionLoadStatus::IoFailure);
    QVERIFY(!result.snapshot.has_value());
#endif
}

void BrowserSessionStoreTest::rejectsDangerousCorruptLeaves_data()
{
    rejectsDangerousPrimaryLeaves_data();
}

void BrowserSessionStoreTest::rejectsDangerousCorruptLeaves()
{
#ifndef Q_OS_WIN
    QSKIP("Windows leaf validation is Windows-specific");
#else
    QFETCH(int, leafType);
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString primary = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString corrupt = primary + QStringLiteral(".corrupt");
    const QString source = QDir(fixture.path).filePath(
        QStringLiteral("corrupt-source.json"));
    QVERIFY(writeFile(primary, QByteArrayLiteral("{bad-primary}\n")));

    if (leafType == 0) {
        QVERIFY(writeFile(source, QByteArrayLiteral("old")));
        if (!createSymbolicLink(corrupt, source)) {
            QSKIP("File symlink creation unavailable");
        }
    } else if (leafType == 1) {
        QVERIFY(writeFile(source, QByteArrayLiteral("old")));
        QVERIFY(createHardLink(corrupt, source));
    } else if (leafType == 2) {
        QVERIFY(QDir().mkpath(corrupt));
    } else {
        QVERIFY(writeFile(corrupt, QByteArrayLiteral("old")));
        if (leafType == 3) {
            QVERIFY(applySecurity(
                corrupt,
                QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)")));
        } else if (leafType == 4) {
            QVERIFY(grantLpacRead(corrupt));
        } else {
            QVERIFY(applySecurity(
                corrupt,
                QStringLiteral("D:AI(A;ID;GW;;;WD)(A;;FA;;;SY)")));
        }
    }

    BrowserSessionStore store(fixture.authority);
    const BrowserSessionLoadResult result = store.load();
    QCOMPARE(result.status, BrowserSessionLoadStatus::IoFailure);
    QVERIFY(QFileInfo::exists(primary));
#endif
}

void BrowserSessionStoreTest::detectsLeafIdentityAndDaclRaces_data()
{
    QTest::addColumn<bool>("corruptLeaf");
    QTest::addColumn<bool>("identityRace");
    QTest::newRow("primary-identity") << false << true;
    QTest::newRow("primary-dacl") << false << false;
    QTest::newRow("corrupt-identity") << true << true;
    QTest::newRow("corrupt-dacl") << true << false;
}

void BrowserSessionStoreTest::detectsLeafIdentityAndDaclRaces()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    QFETCH(bool, corruptLeaf);
    QFETCH(bool, identityRace);
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString primary = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString corrupt = primary + QStringLiteral(".corrupt");
    QVERIFY(writeFile(primary,
                      corruptLeaf ? QByteArrayLiteral("{bad-primary}\n")
                                  : sampleDocument()));
    if (corruptLeaf) {
        QVERIFY(writeFile(corrupt, QByteArrayLiteral("old-corrupt")));
    }
    bool raced = false;
    auto race = [&raced, identityRace](const QString &path) {
        raced = identityRace
            ? replaceLeaf(path, QByteArrayLiteral("replacement"))
            : applySecurity(
                  path,
                  QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)"));
    };
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    if (corruptLeaf) {
        hooks.afterCorruptLeafOpenedBeforeUse = race;
    } else {
        hooks.afterPrimaryLeafOpenedBeforeRead = race;
    }
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    BrowserSessionStore store(fixture.authority);
    const BrowserSessionLoadResult result = store.load();
    QVERIFY(raced);
    QCOMPARE(result.status, BrowserSessionLoadStatus::IoFailure);
    QVERIFY(!result.snapshot.has_value());
#endif
}

void BrowserSessionStoreTest::blocksParentReplacementRaces_data()
{
    QTest::addColumn<bool>("corruptLeaf");
    QTest::newRow("primary") << false;
    QTest::newRow("corrupt") << true;
}

void BrowserSessionStoreTest::blocksParentReplacementRaces()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    QFETCH(bool, corruptLeaf);
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString primary = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString corrupt = primary + QStringLiteral(".corrupt");
    QVERIFY(writeFile(primary,
                      corruptLeaf ? QByteArrayLiteral("{bad-primary}\n")
                                  : sampleDocument()));
    if (corruptLeaf) {
        QVERIFY(writeFile(corrupt, QByteArrayLiteral("old-corrupt")));
    }

    bool replacementSucceeded = true;
    const auto replaceParent = [&fixture, &replacementSucceeded](const QString &) {
        const QString moved = fixture.root.filePath(
            QStringLiteral("moved-browser-state"));
        replacementSucceeded = MoveFileExW(
                                   reinterpret_cast<LPCWSTR>(fixture.path.utf16()),
                                   reinterpret_cast<LPCWSTR>(moved.utf16()),
                                   MOVEFILE_WRITE_THROUGH)
            != FALSE;
    };
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    if (corruptLeaf) {
        hooks.afterCorruptLeafOpenedBeforeUse = replaceParent;
    } else {
        hooks.afterPrimaryLeafOpenedBeforeRead = replaceParent;
    }
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    BrowserSessionStore store(fixture.authority);
    const BrowserSessionLoadResult result = store.load();
    QVERIFY(!replacementSucceeded);
    QCOMPARE(result.status,
             corruptLeaf ? BrowserSessionLoadStatus::Corrupt
                         : BrowserSessionLoadStatus::Loaded);
#endif
}

void BrowserSessionStoreTest::detectsDaclRaceAfterAtomicReplace()
{
#if !defined(Q_OS_WIN) || !defined(Q_BROWSER_HOST_TESTING)
    QSKIP("Windows session test hooks are unavailable");
#else
    SessionHooksReset hooksReset;
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    BrowserSessionStore store(fixture.authority);
    QCOMPARE(store.save(sampleSnapshot()).status,
             BrowserSessionSaveStatus::Saved);
    bool changed = false;
    qbrowser_host_testing::BrowserSessionStoreTestHooks hooks;
    hooks.afterAtomicPublishBeforeVerify = [&changed](const QString &path) {
        changed = applySecurity(
            path, QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;WD)"));
    };
    qbrowser_host_testing::setBrowserSessionStoreTestHooks(std::move(hooks));

    BrowserWindowSnapshot replacement = sampleSnapshot();
    replacement.geometry.moveTo(100, 200);
    const BrowserSessionSaveResult result = store.save(replacement);
    QVERIFY(changed);
    QCOMPARE(result.status, BrowserSessionSaveStatus::IoFailure);
    QCOMPARE(store.load().status, BrowserSessionLoadStatus::IoFailure);
#endif
}

void BrowserSessionStoreTest::preservesAtMostOneBoundedCorruptCopyWithoutParsing()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString primary = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString corrupt = primary + QStringLiteral(".corrupt");
    const QByteArray first = QByteArrayLiteral(
        "{\"secret-marker\":\"never-log-or-parse\"}\n");
    QVERIFY(writeFile(primary, first));
    BrowserSessionStore store(fixture.authority);

    const BrowserSessionLoadResult firstResult = store.load();
    QCOMPARE(firstResult.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!firstResult.stableError.contains(QStringLiteral("secret-marker")));
    QVERIFY(!QFileInfo::exists(primary));
    QCOMPARE(readFile(corrupt), first);
    QVERIFY(first.size() <= 16 * 1024 * 1024);

    const QByteArray second = QByteArrayLiteral("{different-bad-json}\n");
    QVERIFY(writeFile(primary, second));
    const BrowserSessionLoadResult secondResult = store.load();
    QCOMPARE(secondResult.status, BrowserSessionLoadStatus::Corrupt);
    QVERIFY(!QFileInfo::exists(primary));
    QCOMPARE(readFile(corrupt), first);

    const QStringList entries = QDir(fixture.path).entryList(
        {QStringLiteral("browser-session.json.corrupt*")}, QDir::Files);
    QCOMPARE(entries, QStringList{QStringLiteral("browser-session.json.corrupt")});
#endif
}

void BrowserSessionStoreTest::rejectsOversizedExistingCorruptLeaf()
{
#ifndef Q_OS_WIN
    QSKIP("Windows stable session persistence is Windows-specific");
#else
    StateDirectoryFixture fixture;
    QVERIFY(fixture.authority);
    const QString primary = QDir(fixture.path).filePath(
        QStringLiteral("browser-session.json"));
    const QString corrupt = primary + QStringLiteral(".corrupt");
    QVERIFY(writeFile(primary, QByteArrayLiteral("{bad-primary}\n")));
    QVERIFY(writeFile(corrupt, QByteArray(16 * 1024 * 1024 + 1, 'x')));
    BrowserSessionStore store(fixture.authority);

    const BrowserSessionLoadResult result = store.load();

    QCOMPARE(result.status, BrowserSessionLoadStatus::IoFailure);
    QVERIFY(QFileInfo::exists(primary));
    QVERIFY(QFileInfo::exists(corrupt));
#endif
}

QTEST_APPLESS_MAIN(BrowserSessionStoreTest)

#include "tst_browser_session_store.moc"
