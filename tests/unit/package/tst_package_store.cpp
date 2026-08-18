#include "PackageStore.h"
#include "ArchiveTestHooks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace
{
constexpr auto AppId = "company.pilot";
constexpr auto VersionOne = "1.0.0";
constexpr auto VersionTwo = "1.1.0";

QString appId()
{
    return QString::fromLatin1(AppId);
}

QString versionOne()
{
    return QString::fromLatin1(VersionOne);
}

QString versionTwo()
{
    return QString::fromLatin1(VersionTwo);
}

QByteArray digest(char value)
{
    return QByteArray(64, value);
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.dir().absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

QString createCandidate(QTemporaryDir &temporary,
                        const QString &name,
                        const QByteArray &contents)
{
    const QString root = temporary.filePath(QStringLiteral("candidates/") + name);
    if (!QDir().mkpath(root)
        || !writeFile(root + QStringLiteral("/qml/Main.qml"), contents)) {
        return {};
    }
    return root;
}

QString targetName(const QString &version, const QByteArray &digestHex)
{
    return version + QLatin1Char('-') + QString::fromLatin1(digestHex);
}
}

class PackageStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void storesVersionDirectoriesWithoutReplacingExistingContent();
    void acceptsStrictSemanticVersionDirectoryNames();
    void atomicallyTracksCurrentPreviousAndLastKnownGood();
    void activationIsIdempotentAndRollbackRestoresPrevious();
    void ignoresInterruptedTemporaryStateFiles();
    void rejectsInvalidOrEscapingPointerTargets();
    void pinsStoreDirectoriesDuringStateCommit();
};

void PackageStoreTest::storesVersionDirectoriesWithoutReplacingExistingContent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));

    const QString first = createCandidate(temporary, QStringLiteral("first"), "first");
    QVERIFY(!first.isEmpty());
    const PackageStoreResult committed = store.commitCandidate(
        appId(), versionOne(), digest('a'), first);
    QVERIFY2(committed.succeeded(), qPrintable(committed.message));
    QCOMPARE(QDir::cleanPath(committed.path),
             QDir::cleanPath(temporary.filePath(
                 QStringLiteral("store/apps/company.pilot/versions/")
                 + targetName(versionOne(), digest('a')))));
    QVERIFY(!QFileInfo::exists(first));

    const QString second = createCandidate(temporary, QStringLiteral("second"), "second");
    const PackageStoreResult duplicate = store.commitCandidate(
        appId(), versionOne(), digest('a'), second);
    QVERIFY(!duplicate.succeeded());
    QCOMPARE(duplicate.error, PackageStoreError::VersionExists);
    QFile installed(committed.path + QStringLiteral("/qml/Main.qml"));
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), QByteArray("first"));
    QVERIFY(QFileInfo::exists(second));
}

void PackageStoreTest::acceptsStrictSemanticVersionDirectoryNames()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString candidate = createCandidate(
        temporary, QStringLiteral("prerelease"), "prerelease");
    const QString version = QStringLiteral("1.0.0-beta.1+pilot.7");
    const PackageStoreResult committed = store.commitCandidate(
        appId(), version, digest('c'), candidate);
    QVERIFY2(committed.succeeded(), qPrintable(committed.message));
    QCOMPARE(QFileInfo(committed.path).fileName(), targetName(version, digest('c')));
}

void PackageStoreTest::atomicallyTracksCurrentPreviousAndLastKnownGood()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString first = createCandidate(temporary, QStringLiteral("one"), "one");
    const QString second = createCandidate(temporary, QStringLiteral("two"), "two");
    QVERIFY(store.commitCandidate(appId(), versionOne(),
                                  digest('a'), first).succeeded());
    QVERIFY(store.commitCandidate(appId(), versionTwo(),
                                  digest('b'), second).succeeded());
    const QString one = targetName(versionOne(), digest('a'));
    const QString two = targetName(versionTwo(), digest('b'));

    QVERIFY(store.activate(appId(), one).succeeded());
    QVERIFY(store.markCurrentLastKnownGood(appId()).succeeded());
    QVERIFY(store.activate(appId(), two).succeeded());

    const ActivationStateResult loaded = store.activationState(appId());
    QVERIFY2(loaded.hasValue(), qPrintable(loaded.message));
    QCOMPARE(loaded.state.current, two);
    QCOMPARE(loaded.state.previous, one);
    QCOMPARE(loaded.state.lastKnownGood, one);
    QCOMPARE(store.resolveCurrent(appId()).path,
             store.versionPath(appId(), two));

    const QString statePath = store.appRoot(appId())
        + QStringLiteral("/activation.json");
    QFile stateFile(statePath);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    const QJsonObject object = QJsonDocument::fromJson(stateFile.readAll()).object();
    QCOMPARE(object.value(QStringLiteral("current")).toString(),
             QStringLiteral("versions/") + two);
    QCOMPARE(object.value(QStringLiteral("previous")).toString(),
             QStringLiteral("versions/") + one);
    QCOMPARE(object.value(QStringLiteral("lastKnownGood")).toString(),
             QStringLiteral("versions/") + one);
}

void PackageStoreTest::activationIsIdempotentAndRollbackRestoresPrevious()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString first = createCandidate(temporary, QStringLiteral("one"), "one");
    const QString second = createCandidate(temporary, QStringLiteral("two"), "two");
    QVERIFY(store.commitCandidate(appId(), versionOne(),
                                  digest('a'), first).succeeded());
    QVERIFY(store.commitCandidate(appId(), versionTwo(),
                                  digest('b'), second).succeeded());
    const QString one = targetName(versionOne(), digest('a'));
    const QString two = targetName(versionTwo(), digest('b'));
    QVERIFY(store.activate(appId(), one).succeeded());
    QVERIFY(store.markCurrentLastKnownGood(appId()).succeeded());
    QVERIFY(store.activate(appId(), two).succeeded());

    QVERIFY(store.activate(appId(), two).succeeded());
    QCOMPARE(store.activationState(appId()).state.previous, one);
    QVERIFY(store.rollback(appId()).succeeded());
    const ActivationState state = store.activationState(appId()).state;
    QCOMPARE(state.current, one);
    QCOMPARE(state.previous, two);
    QCOMPARE(state.lastKnownGood, one);
}

void PackageStoreTest::ignoresInterruptedTemporaryStateFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString candidate = createCandidate(temporary, QStringLiteral("one"), "one");
    QVERIFY(store.commitCandidate(appId(), versionOne(),
                                  digest('a'), candidate).succeeded());
    const QString one = targetName(versionOne(), digest('a'));
    QVERIFY(store.activate(appId(), one).succeeded());
    QVERIFY(writeFile(store.appRoot(appId())
                          + QStringLiteral("/.activation.json.interrupted"),
                      R"({"current":"versions/escape"})"));
    const ActivationStateResult loaded = store.activationState(appId());
    QVERIFY(loaded.hasValue());
    QCOMPARE(loaded.state.current, one);
}

void PackageStoreTest::rejectsInvalidOrEscapingPointerTargets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString candidate = createCandidate(temporary, QStringLiteral("one"), "one");
    QVERIFY(store.commitCandidate(appId(), versionOne(),
                                  digest('a'), candidate).succeeded());
    const QString appRoot = store.appRoot(appId());
    const QString validTarget = QStringLiteral("versions/")
        + targetName(versionOne(), digest('a'));

    const QList<QByteArray> invalidStates{
        R"({"current":"../outside","previous":"","lastKnownGood":""})",
        R"({"current":"C:/Windows","previous":"","lastKnownGood":""})",
        R"({"current":"versions/missing","previous":"","lastKnownGood":""})",
        R"({"current":"versions/../outside","previous":"","lastKnownGood":""})",
        QByteArrayLiteral("{\"current\":\"") + validTarget.toUtf8()
            + QByteArrayLiteral("\",\"current\":\"") + validTarget.toUtf8()
            + QByteArrayLiteral("\",\"previous\":\"\",\"lastKnownGood\":\"\"}")};
    for (const QByteArray &state : invalidStates) {
        QVERIFY(writeFile(appRoot + QStringLiteral("/activation.json"), state));
        const ActivationStateResult loaded = store.activationState(appId());
        QVERIFY(!loaded.hasValue());
        QCOMPARE(loaded.error, PackageStoreError::InvalidState);
        QVERIFY(!store.resolveCurrent(appId()).succeeded());
    }
}

void PackageStoreTest::pinsStoreDirectoriesDuringStateCommit()
{
#ifdef Q_OS_WIN
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    const QString candidate = createCandidate(temporary, QStringLiteral("one"), "one");
    QVERIFY(store.commitCandidate(appId(), versionOne(), digest('a'), candidate).succeeded());
    const QString one = targetName(versionOne(), digest('a'));

    bool sawAppRootDeleteLock = false;
    bool sawStateSourceHandle = false;
    bool stateWriteBlocked = false;
    bool targetRenameBlocked = false;
    const QString targetPath = store.versionPath(appId(), one);
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.afterWindowsHandleOpened = [&](const QString &path,
                                         const quint32 access,
                                         const bool) {
        if (QDir::cleanPath(path).compare(QDir::cleanPath(store.appRoot(appId())),
                                          Qt::CaseInsensitive)
                == 0
            && (access & DELETE) != 0U) {
            sawAppRootDeleteLock = true;
        }
        const QString statePath = store.appRoot(appId())
            + QStringLiteral("/activation.json");
        if (QDir::cleanPath(path).compare(QDir::cleanPath(statePath),
                                          Qt::CaseInsensitive)
                == 0
            && (access & GENERIC_READ) != 0U) {
            sawStateSourceHandle = true;
            HANDLE writer = CreateFileW(
                reinterpret_cast<LPCWSTR>(path.utf16()),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            stateWriteBlocked = writer == INVALID_HANDLE_VALUE;
            if (writer != INVALID_HANDLE_VALUE) {
                (void)CloseHandle(writer);
            }
        }
        if (QDir::cleanPath(path).compare(QDir::cleanPath(targetPath),
                                          Qt::CaseInsensitive)
                == 0
            && (access & FILE_LIST_DIRECTORY) != 0U) {
            const QString renamed = targetPath + QStringLiteral("-renamed");
            targetRenameBlocked = MoveFileExW(
                                      reinterpret_cast<LPCWSTR>(targetPath.utf16()),
                                      reinterpret_cast<LPCWSTR>(renamed.utf16()),
                                      0U)
                == FALSE;
            if (QFileInfo::exists(renamed)) {
                (void)MoveFileExW(
                    reinterpret_cast<LPCWSTR>(renamed.utf16()),
                    reinterpret_cast<LPCWSTR>(targetPath.utf16()),
                    0U);
            }
        }
    };
    qbrowser_archive_testing::setArchiveTestHooks(std::move(hooks));
    const PackageStoreResult activated = store.activate(appId(), one);
    const ActivationStateResult loaded = store.activationState(appId());
    qbrowser_archive_testing::resetArchiveTestHooks();
    QVERIFY(activated.succeeded());
    QVERIFY(loaded.hasValue());
    QVERIFY(sawAppRootDeleteLock);
    QVERIFY(sawStateSourceHandle);
    QVERIFY(stateWriteBlocked);
    QVERIFY(targetRenameBlocked);
#else
    QSKIP("Windows stable directory locking is unavailable");
#endif
}

QTEST_APPLESS_MAIN(PackageStoreTest)

#include "tst_package_store.moc"
