#include "Archive.h"
#include "ArchiveTestHooks.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <ctime>
#include <limits>
#include <type_traits>

static_assert(!std::is_default_constructible_v<ArchiveResult>);
static_assert(!std::is_constructible_v<ArchiveResult, QVector<ArchiveEntry>>);
static_assert(!std::is_constructible_v<ArchiveResult, ArchiveError>);

namespace
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
class ArchiveHookGuard final
{
public:
    explicit ArchiveHookGuard(qbrowser_archive_testing::ArchiveTestHooks hooks)
    {
        qbrowser_archive_testing::setArchiveTestHooks(std::move(hooks));
    }

    ~ArchiveHookGuard()
    {
        qbrowser_archive_testing::resetArchiveTestHooks();
    }
};
#endif

struct RawEntry final
{
    QByteArray name;
    QByteArray data = QByteArrayLiteral("x");
    QByteArray centralName;
    quint16 flags = 0;
    quint16 method = 0;
    quint32 externalAttributes = 0x81A40000U;
    std::optional<quint32> crc;
    std::optional<quint32> compressedSize;
    std::optional<quint32> uncompressedSize;
    quint16 versionMadeBy = 0x0314U;
    std::optional<quint16> localFlags;
    std::optional<quint16> localMethod;
};

void append16(QByteArray &bytes, quint16 value)
{
    const quint16 littleEndian = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char *>(&littleEndian),
        static_cast<qsizetype>(sizeof(littleEndian)));
}

void append32(QByteArray &bytes, quint32 value)
{
    const quint32 littleEndian = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char *>(&littleEndian),
        static_cast<qsizetype>(sizeof(littleEndian)));
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const quint32 mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

QByteArray makeZip(
    const QVector<RawEntry> &entries,
    quint16 endDiskNumber = 0,
    quint16 centralDiskNumber = 0)
{
    QByteArray bytes;
    struct CentralEntry final
    {
        RawEntry entry;
        quint32 offset = 0;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 uncompressedSize = 0;
    };
    QVector<CentralEntry> centralEntries;

    for (const RawEntry &entry : entries) {
        CentralEntry central;
        central.entry = entry;
        central.offset = static_cast<quint32>(bytes.size());
        central.crc = entry.crc.value_or(crc32(entry.data));
        central.compressedSize = entry.compressedSize.value_or(
            static_cast<quint32>(entry.data.size()));
        central.uncompressedSize = entry.uncompressedSize.value_or(
            static_cast<quint32>(entry.data.size()));
        centralEntries.push_back(central);

        append32(bytes, 0x04034B50U);
        append16(bytes, 20);
        append16(bytes, entry.localFlags.value_or(entry.flags));
        append16(bytes, entry.localMethod.value_or(entry.method));
        append16(bytes, 0);
        append16(bytes, 0x0021);
        append32(bytes, central.crc);
        append32(bytes, central.compressedSize);
        append32(bytes, central.uncompressedSize);
        append16(bytes, static_cast<quint16>(entry.name.size()));
        append16(bytes, 0);
        bytes.append(entry.name);
        bytes.append(entry.data);
    }

    const quint32 centralOffset = static_cast<quint32>(bytes.size());
    for (const CentralEntry &central : centralEntries) {
        const QByteArray centralName = central.entry.centralName.isNull()
            ? central.entry.name
            : central.entry.centralName;
        append32(bytes, 0x02014B50U);
        append16(bytes, central.entry.versionMadeBy);
        append16(bytes, 20);
        append16(bytes, central.entry.flags);
        append16(bytes, central.entry.method);
        append16(bytes, 0);
        append16(bytes, 0x0021);
        append32(bytes, central.crc);
        append32(bytes, central.compressedSize);
        append32(bytes, central.uncompressedSize);
        append16(bytes, static_cast<quint16>(centralName.size()));
        append16(bytes, 0);
        append16(bytes, 0);
        append16(bytes, 0);
        append16(bytes, 0);
        append32(bytes, central.entry.externalAttributes);
        append32(bytes, central.offset);
        bytes.append(centralName);
    }
    const quint32 centralSize = static_cast<quint32>(bytes.size()) - centralOffset;
    append32(bytes, 0x06054B50U);
    append16(bytes, endDiskNumber);
    append16(bytes, centralDiskNumber);
    append16(bytes, static_cast<quint16>(entries.size()));
    append16(bytes, static_cast<quint16>(entries.size()));
    append32(bytes, centralSize);
    append32(bytes, centralOffset);
    append16(bytes, 0);
    return bytes;
}

QString writeArchive(QTemporaryDir &temporary, const QByteArray &bytes)
{
    const QString path = temporary.filePath(QStringLiteral("input.qapkg"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

quint16 read16(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 read32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

struct CentralMetadata final
{
    QByteArray name;
    quint16 versionMadeBy = 0;
    quint16 method = 0;
    quint16 time = 0;
    quint16 date = 0;
    quint16 localTime = 0;
    quint16 localDate = 0;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 externalAttributes = 0;
};

QVector<CentralMetadata> centralMetadata(const QByteArray &bytes)
{
    const qsizetype endOffset = bytes.lastIndexOf(QByteArrayLiteral("PK\x05\x06"));
    if (endOffset < 0 || endOffset + 22 > bytes.size()) {
        return {};
    }
    const quint16 count = read16(bytes, endOffset + 10);
    qsizetype offset = static_cast<qsizetype>(read32(bytes, endOffset + 16));
    QVector<CentralMetadata> result;
    for (quint16 index = 0; index < count; ++index) {
        if (offset + 46 > bytes.size()
            || read32(bytes, offset) != 0x02014B50U) {
            return {};
        }
        const quint16 nameLength = read16(bytes, offset + 28);
        const quint16 extraLength = read16(bytes, offset + 30);
        const quint16 commentLength = read16(bytes, offset + 32);
        const qsizetype localOffset = static_cast<qsizetype>(
            read32(bytes, offset + 42));
        if (localOffset + 30 > bytes.size()
            || read32(bytes, localOffset) != 0x04034B50U) {
            return {};
        }
        result.push_back({
            bytes.mid(offset + 46, nameLength),
            read16(bytes, offset + 4),
            read16(bytes, offset + 10),
            read16(bytes, offset + 12),
            read16(bytes, offset + 14),
            read16(bytes, localOffset + 10),
            read16(bytes, localOffset + 12),
            read32(bytes, offset + 16),
            read32(bytes, offset + 20),
            read32(bytes, offset + 24),
            read32(bytes, offset + 38)});
        offset += 46 + nameLength + extraLength + commentLength;
    }
    return result;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

#ifdef Q_OS_WIN
bool movePathNoReplace(const QString &from, const QString &to)
{
    return MoveFileExW(
               reinterpret_cast<LPCWSTR>(from.utf16()),
               reinterpret_cast<LPCWSTR>(to.utf16()),
               0U)
        != FALSE;
}

bool createJunction(const QString &junction, const QString &target)
{
    return QProcess::execute(
               QStringLiteral("cmd.exe"),
               {QStringLiteral("/d"),
                QStringLiteral("/c"),
                QStringLiteral("mklink"),
                QStringLiteral("/J"),
                QDir::toNativeSeparators(junction),
                QDir::toNativeSeparators(target)})
        == 0;
}
#endif

class TimeZoneGuard final
{
public:
    TimeZoneGuard()
        : m_original(qgetenv("TZ")), m_hadOriginal(qEnvironmentVariableIsSet("TZ"))
    {
    }

    ~TimeZoneGuard()
    {
        if (m_hadOriginal) {
            (void)qputenv("TZ", m_original);
        } else {
            qunsetenv("TZ");
        }
        refresh();
    }

    void set(const QByteArray &value)
    {
        (void)qputenv("TZ", value);
        refresh();
    }

private:
    static void refresh()
    {
#ifdef Q_OS_WIN
        _tzset();
#else
        tzset();
#endif
    }

    QByteArray m_original;
    bool m_hadOriginal = false;
};
}

class ArchiveTest final : public QObject
{
    Q_OBJECT

private slots:
    void resultStateIsTotal();
    void createsAndInspectsAnArchive();
    void rejectsUnsafeEntryPaths_data();
    void rejectsUnsafeEntryPaths();
    void rejectsFileAncestorCollisions_data();
    void rejectsFileAncestorCollisions();
    void rejectsUnsafeMetadata_data();
    void rejectsUnsafeMetadata();
    void acceptsWhitelistedDosFileAttributes();
    void enforcesResourceLimits();
    void boundsArchiveReadsAfterOpen();
    void rejectsUnrepresentableArchiveLimit();
    void rejectsSourceGrowthAfterPreflight();
    void rejectsSourceShrinkAfterPreflight();
    void exposesCanonicalContentDigestView();
    void extractsOnlyAfterCompleteVerification();
    void leavesStagingEmptyWhenVerificationFails();
    void requiresEmptyNonReparseStagingRoot();
    void stagingRootCannotBeReplacedAfterGuard();
    void createdParentCannotBeReplacedAfterGuard();
    void doesNotOverwriteConcurrentTargetOrDeleteUserFiles();
    void cleansOnlyOwnedObjectsAfterFailure();
    void sourceParentsRemainStableDuringRead();
    void writesByteForByteDeterministicArchives();
    void rejectsUnsafeSourceTreesBeforeWriting();
};

void ArchiveTest::resultStateIsTotal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString missing = temporary.filePath(QStringLiteral("missing.qapkg"));
    const ArchiveResult failed = Archive::inspect(missing);
    QVERIFY(!failed.hasValue());
    QVERIFY(failed.entries().isEmpty());
    QVERIFY(failed.error().code != ArchiveErrorCode::None);

    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    QVERIFY(writeFile(source + QStringLiteral("/empty"), {}));
    const ArchiveResult succeeded = Archive::create(
        source, temporary.filePath(QStringLiteral("valid.qapkg")));
    QVERIFY(succeeded.hasValue());
    QCOMPARE(succeeded.error().code, ArchiveErrorCode::None);
}

void ArchiveTest::createsAndInspectsAnArchive()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkpath(source + QStringLiteral("/qml")));
    QFile input(source + QStringLiteral("/qml/Main.qml"));
    QVERIFY(input.open(QIODevice::WriteOnly));
    QCOMPARE(input.write("import QtQuick\n"), 15);
    input.close();
    QVERIFY(writeFile(source + QStringLiteral("/empty.txt"), {}));

    const QString package = temporary.filePath(QStringLiteral("app.qapkg"));
    const ArchiveResult created = Archive::create(source, package);
    QVERIFY2(created.hasValue(), qPrintable(created.error().message));

    const ArchiveResult inspected = Archive::inspect(package);
    QVERIFY2(inspected.hasValue(), qPrintable(inspected.error().message));
    QCOMPARE(inspected.entries().size(), 2);
    QCOMPARE(inspected.entries().front().path, QByteArray("empty.txt"));
    const QVector<CentralMetadata> metadata = centralMetadata(
        [&package] {
            QFile file(package);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        }());
    QCOMPARE(metadata.size(), 2);
    QCOMPARE(metadata.front().name, QByteArray("empty.txt"));
    QCOMPARE(metadata.front().method, static_cast<quint16>(0U));
    QCOMPARE(metadata.front().crc, 0U);
    QCOMPARE(metadata.front().compressedSize, 0U);
    QCOMPARE(metadata.front().uncompressedSize, 0U);
}

void ArchiveTest::rejectsUnsafeEntryPaths_data()
{
    QTest::addColumn<QVector<QByteArray>>("names");

    QTest::newRow("empty") << QVector<QByteArray>{QByteArray()};
    QTest::newRow("dot") << QVector<QByteArray>{QByteArrayLiteral("./x")};
    QTest::newRow("dot-dot") << QVector<QByteArray>{QByteArrayLiteral("../x")};
    QTest::newRow("nested-dot-dot")
        << QVector<QByteArray>{QByteArrayLiteral("a/../x")};
    QTest::newRow("absolute") << QVector<QByteArray>{QByteArrayLiteral("/x")};
    QTest::newRow("drive") << QVector<QByteArray>{QByteArrayLiteral("C:/x")};
    QTest::newRow("backslash") << QVector<QByteArray>{QByteArrayLiteral("a\\x")};
    QTest::newRow("unc") << QVector<QByteArray>{QByteArrayLiteral("\\\\server\\x")};
    QTest::newRow("duplicate-separator")
        << QVector<QByteArray>{QByteArrayLiteral("a//x")};
    QTest::newRow("trailing-separator")
        << QVector<QByteArray>{QByteArrayLiteral("a/")};
    QTest::newRow("colon-ads") << QVector<QByteArray>{QByteArrayLiteral("a:x")};
    QTest::newRow("reserved-character")
        << QVector<QByteArray>{QByteArrayLiteral("a<x")};
    QTest::newRow("device") << QVector<QByteArray>{QByteArrayLiteral("CON.txt")};
    QTest::newRow("device-superscript")
        << QVector<QByteArray>{QStringLiteral("COM¹.txt").toUtf8()};
    QTest::newRow("trailing-dot") << QVector<QByteArray>{QByteArrayLiteral("a.")};
    QTest::newRow("trailing-space") << QVector<QByteArray>{QByteArrayLiteral("a ")};
    QTest::newRow("nul") << QVector<QByteArray>{QByteArray("a\0b", 3)};
    QTest::newRow("invalid-utf8") << QVector<QByteArray>{QByteArray("\xC3\x28", 2)};
    QTest::newRow("exact-duplicate")
        << QVector<QByteArray>{QByteArrayLiteral("a"), QByteArrayLiteral("a")};
    QTest::newRow("case-collision")
        << QVector<QByteArray>{QByteArrayLiteral("A"), QByteArrayLiteral("a")};
    QTest::newRow("unicode-collision")
        << QVector<QByteArray>{QStringLiteral("é").toUtf8(),
                               QStringLiteral("e\u0301").toUtf8()};
}

void ArchiveTest::rejectsUnsafeEntryPaths()
{
    QFETCH(QVector<QByteArray>, names);
    QVector<RawEntry> entries;
    for (const QByteArray &name : names) {
        entries.push_back({name});
    }
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = writeArchive(temporary, makeZip(entries));
    QVERIFY(!path.isEmpty());

    const ArchiveResult result = Archive::inspect(path);
    QVERIFY(!result.hasValue());
    QVERIFY(result.error().code == ArchiveErrorCode::InvalidEntryPath
            || result.error().code == ArchiveErrorCode::DuplicateEntryPath);
    QVERIFY(!result.error().message.contains(QStringLiteral("x")));
}

void ArchiveTest::rejectsFileAncestorCollisions_data()
{
    QTest::addColumn<QVector<QByteArray>>("names");
    QTest::addColumn<QByteArray>("expectedPath");

    QTest::newRow("parent-first")
        << QVector<QByteArray>{QByteArrayLiteral("a"), QByteArrayLiteral("a/b")}
        << QByteArray("a/b");
    QTest::newRow("child-first")
        << QVector<QByteArray>{QByteArrayLiteral("a/b"), QByteArrayLiteral("a")}
        << QByteArray("a/b");
    QTest::newRow("case-equivalent-parent")
        << QVector<QByteArray>{QByteArrayLiteral("A"), QByteArrayLiteral("a/b")}
        << QByteArray("a/b");
    const QByteArray composedParent = QStringLiteral("é").toUtf8();
    const QByteArray decomposedChild = QStringLiteral("e\u0301/b").toUtf8();
    QTest::newRow("nfc-equivalent-parent")
        << QVector<QByteArray>{composedParent, decomposedChild}
        << decomposedChild;
    QTest::newRow("nfc-equivalent-child-first")
        << QVector<QByteArray>{decomposedChild, composedParent}
        << decomposedChild;
}

void ArchiveTest::rejectsFileAncestorCollisions()
{
    QFETCH(QVector<QByteArray>, names);
    QFETCH(QByteArray, expectedPath);
    QVector<RawEntry> entries;
    for (const QByteArray &name : names) {
        entries.push_back({name});
    }
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(temporary, makeZip(entries));
    QVERIFY(!package.isEmpty());

    const ArchiveResult inspected = Archive::inspect(package);
    QVERIFY(!inspected.hasValue());
    QCOMPARE(inspected.error().code, ArchiveErrorCode::DuplicateEntryPath);
    QCOMPARE(inspected.error().path, expectedPath);

    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));
    const ArchiveResult extracted = Archive::extract(package, staging);
    QVERIFY(!extracted.hasValue());
    QCOMPARE(extracted.error().code, ArchiveErrorCode::DuplicateEntryPath);
    QCOMPARE(extracted.error().path, expectedPath);
    QVERIFY(QDir(staging).isEmpty());
}

void ArchiveTest::rejectsUnsafeMetadata_data()
{
    QTest::addColumn<QByteArray>("archive");
    QTest::addColumn<ArchiveErrorCode>("expectedCode");

    RawEntry encrypted{QByteArrayLiteral("safe.txt")};
    encrypted.flags = 0x0001U;
    QTest::newRow("encrypted")
        << makeZip({encrypted}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry descriptor{QByteArrayLiteral("safe.txt")};
    descriptor.flags = 0x0008U;
    QTest::newRow("data-descriptor")
        << makeZip({descriptor}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry strongEncryption{QByteArrayLiteral("safe.txt")};
    strongEncryption.flags = 0x0040U;
    QTest::newRow("strong-encryption")
        << makeZip({strongEncryption}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry compressedPatch{QByteArrayLiteral("safe.txt")};
    compressedPatch.flags = 0x0020U;
    QTest::newRow("compressed-patch")
        << makeZip({compressedPatch}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry unsupportedMethod{QByteArrayLiteral("safe.txt")};
    unsupportedMethod.method = 99;
    QTest::newRow("unsupported-method")
        << makeZip({unsupportedMethod}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry zeroDeflate{QByteArrayLiteral("empty.txt")};
    zeroDeflate.data.clear();
    zeroDeflate.method = 8;
    zeroDeflate.crc = 0;
    zeroDeflate.compressedSize = 0;
    zeroDeflate.uncompressedSize = 0;
    QTest::newRow("zero-length-deflate")
        << makeZip({zeroDeflate}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry hiddenEmptyBytes{QByteArrayLiteral("empty.txt")};
    hiddenEmptyBytes.data = QByteArrayLiteral("hidden");
    hiddenEmptyBytes.crc = 0;
    hiddenEmptyBytes.compressedSize = 0;
    hiddenEmptyBytes.uncompressedSize = 0;
    QTest::newRow("zero-length-hidden-bytes")
        << makeZip({hiddenEmptyBytes}) << ArchiveErrorCode::InvalidArchive;

    RawEntry storedSizeMismatch{QByteArrayLiteral("stored.txt")};
    storedSizeMismatch.data = QByteArrayLiteral("x");
    storedSizeMismatch.uncompressedSize = 2;
    QTest::newRow("stored-size-mismatch")
        << makeZip({storedSizeMismatch}) << ArchiveErrorCode::InvalidArchive;

    RawEntry overOutput{QByteArrayLiteral("deflate.txt")};
    overOutput.data = QByteArray::fromHex("4b4c4c0400");
    overOutput.method = 8;
    overOutput.crc = crc32(QByteArrayLiteral("aa"));
    overOutput.uncompressedSize = 2;
    QTest::newRow("deflate-over-output")
        << makeZip({overOutput}) << ArchiveErrorCode::InvalidArchive;

    RawEntry trailingDeflate{QByteArrayLiteral("deflate.txt")};
    trailingDeflate.data = QByteArray::fromHex("4b4c4c0400deadbeef");
    trailingDeflate.method = 8;
    trailingDeflate.crc = crc32(QByteArrayLiteral("aaa"));
    trailingDeflate.uncompressedSize = 3;
    QTest::newRow("deflate-trailing-stream")
        << makeZip({trailingDeflate}) << ArchiveErrorCode::InvalidArchive;

    RawEntry symlink{QByteArrayLiteral("safe.txt")};
    symlink.externalAttributes = 0xA1FF0000U;
    QTest::newRow("symlink")
        << makeZip({symlink}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry directory{QByteArrayLiteral("directory")};
    directory.externalAttributes = 0x41ED0010U;
    QTest::newRow("non-regular-directory-mode")
        << makeZip({directory}) << ArchiveErrorCode::UnsupportedEntry;

    auto dosEntryWithAttributes = [](quint32 attributes) {
        RawEntry entry{QByteArrayLiteral("safe.txt")};
        entry.versionMadeBy = 0x0014U;
        entry.externalAttributes = attributes;
        return entry;
    };
    QTest::newRow("dos-volume-label")
        << makeZip({dosEntryWithAttributes(0x08U)})
        << ArchiveErrorCode::UnsupportedEntry;
    QTest::newRow("dos-directory")
        << makeZip({dosEntryWithAttributes(0x10U)})
        << ArchiveErrorCode::UnsupportedEntry;
    QTest::newRow("dos-device")
        << makeZip({dosEntryWithAttributes(0x40U)})
        << ArchiveErrorCode::UnsupportedEntry;
    QTest::newRow("dos-reparse")
        << makeZip({dosEntryWithAttributes(0x400U)})
        << ArchiveErrorCode::UnsupportedEntry;
    QTest::newRow("dos-unknown-attribute")
        << makeZip({dosEntryWithAttributes(0x800U)})
        << ArchiveErrorCode::UnsupportedEntry;

    RawEntry nameMismatch{QByteArrayLiteral("local.txt")};
    nameMismatch.centralName = QByteArrayLiteral("central.txt");
    QTest::newRow("local-central-name-mismatch")
        << makeZip({nameMismatch}) << ArchiveErrorCode::InvalidArchive;

    RawEntry flagsMismatch{QByteArrayLiteral("safe.txt")};
    flagsMismatch.localFlags = 0x0001U;
    QTest::newRow("local-central-flags-mismatch")
        << makeZip({flagsMismatch}) << ArchiveErrorCode::InvalidArchive;

    RawEntry unknownCreator{QByteArrayLiteral("safe.txt")};
    unknownCreator.versionMadeBy = 0x0A14U;
    QTest::newRow("unknown-creator-platform")
        << makeZip({unknownCreator}) << ArchiveErrorCode::UnsupportedEntry;

    RawEntry badCrc{QByteArrayLiteral("safe.txt")};
    badCrc.crc = 0;
    QTest::newRow("crc-mismatch")
        << makeZip({badCrc}) << ArchiveErrorCode::InvalidArchive;

    QTest::newRow("multi-disk")
        << makeZip({RawEntry{QByteArrayLiteral("safe.txt")}}, 1, 1)
        << ArchiveErrorCode::InvalidArchive;

    QByteArray commented = makeZip({RawEntry{QByteArrayLiteral("safe.txt")}});
    commented[commented.size() - 2] = '\x01';
    commented.append('c');
    QTest::newRow("archive-comment")
        << commented << ArchiveErrorCode::InvalidArchive;
}

void ArchiveTest::rejectsUnsafeMetadata()
{
    QFETCH(QByteArray, archive);
    QFETCH(ArchiveErrorCode, expectedCode);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = writeArchive(temporary, archive);
    QVERIFY(!path.isEmpty());

    const ArchiveResult result = Archive::inspect(path);
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, expectedCode);
    if (QString::fromLatin1(QTest::currentDataTag()) == QStringLiteral("crc-mismatch")) {
        QCOMPARE(result.error().path, QByteArray("safe.txt"));
    }
    QVERIFY(!result.error().message.contains(QStringLiteral("x")));

    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));
    const ArchiveResult extracted = Archive::extract(path, staging);
    QVERIFY(!extracted.hasValue());
    QCOMPARE(extracted.error().code, expectedCode);
    QVERIFY(QDir(staging).isEmpty());
}

void ArchiveTest::acceptsWhitelistedDosFileAttributes()
{
    auto dosEntry = [](QByteArray name, quint32 attributes) {
        RawEntry entry{std::move(name)};
        entry.versionMadeBy = 0x0014U;
        entry.externalAttributes = attributes;
        return entry;
    };
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary,
        makeZip({dosEntry(QByteArrayLiteral("plain.txt"), 0U),
                 dosEntry(QByteArrayLiteral("flags.txt"), 0x27U),
                 dosEntry(QByteArrayLiteral("normal.txt"), 0x80U)}));
    const ArchiveResult result = Archive::inspect(package);
    QVERIFY2(result.hasValue(), qPrintable(result.error().message));
    QCOMPARE(result.entries().size(), 3);
}

void ArchiveTest::enforcesResourceLimits()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ArchiveLimits limits;
    limits.maximumArchiveBytes = 4096;
    limits.maximumEntries = 2;
    limits.maximumEntryBytes = 2;
    limits.maximumTotalBytes = 2;
    limits.maximumCompressionRatio = 2;
    limits.maximumPathBytes = 8;
    limits.maximumComponentBytes = 8;
    limits.maximumPathUtf16Units = 8;
    limits.maximumComponentUtf16Units = 8;

    auto inspect = [&](const QVector<RawEntry> &entries) {
        const QString path = writeArchive(temporary, makeZip(entries));
        return Archive::inspect(path, limits);
    };

    QVERIFY(inspect({RawEntry{QByteArrayLiteral("a")},
                     RawEntry{QByteArrayLiteral("b")}})
                .hasValue());

    const ArchiveResult tooMany = inspect(
        {RawEntry{QByteArrayLiteral("a")},
         RawEntry{QByteArrayLiteral("b")},
         RawEntry{QByteArrayLiteral("c")}});
    QCOMPARE(tooMany.error().code, ArchiveErrorCode::EntryCountLimit);

    RawEntry maxEntry{QByteArrayLiteral("a")};
    maxEntry.data = QByteArray(2, 'a');
    QVERIFY(inspect({maxEntry}).hasValue());
    RawEntry oversized = maxEntry;
    oversized.data.append('a');
    QCOMPARE(
        inspect({oversized}).error().code,
        ArchiveErrorCode::EntrySizeLimit);

    RawEntry one{QByteArrayLiteral("a")};
    one.data = QByteArray(1, 'a');
    RawEntry oneB{QByteArrayLiteral("b")};
    oneB.data = QByteArray(1, 'b');
    QVERIFY(inspect({one, oneB}).hasValue());
    RawEntry oneC{QByteArrayLiteral("c")};
    oneC.data = QByteArray(1, 'c');
    QCOMPARE(
        inspect({one, oneB, oneC}).error().code,
        ArchiveErrorCode::EntryCountLimit);

    ArchiveLimits aggregateLimits = limits;
    aggregateLimits.maximumEntries = 3;
    const QString aggregatePath = writeArchive(
        temporary,
        makeZip({one, oneB, oneC}));
    QCOMPARE(
        Archive::inspect(aggregatePath, aggregateLimits).error().code,
        ArchiveErrorCode::TotalSizeLimit);

    RawEntry ratio{QByteArrayLiteral("a")};
    ratio.data = QByteArray::fromHex("4b4ca43d0000");
    ratio.method = 8;
    ratio.crc = crc32(QByteArray(100, 'a'));
    ratio.uncompressedSize = 100;
    ArchiveLimits ratioLimits = limits;
    ratioLimits.maximumEntryBytes = 100;
    ratioLimits.maximumTotalBytes = 100;
    ratioLimits.maximumCompressionRatio = 16;
    const QString ratioPath = writeArchive(temporary, makeZip({ratio}));
    QCOMPARE(
        Archive::inspect(ratioPath, ratioLimits).error().code,
        ArchiveErrorCode::CompressionRatioLimit);
    ratioLimits.maximumCompressionRatio = 17;
    QVERIFY(Archive::inspect(ratioPath, ratioLimits).hasValue());

    QVERIFY(inspect({RawEntry{QByteArrayLiteral("12345678")}}).hasValue());
    QCOMPARE(
        inspect({RawEntry{QByteArrayLiteral("123456789")}}).error().code,
        ArchiveErrorCode::InvalidEntryPath);

    const QByteArray archiveBytes = makeZip({RawEntry{QByteArrayLiteral("a")}});
    const QString archivePath = writeArchive(temporary, archiveBytes);
    ArchiveLimits archiveLimits = limits;
    archiveLimits.maximumArchiveBytes = static_cast<quint64>(archiveBytes.size());
    QVERIFY(Archive::inspect(archivePath, archiveLimits).hasValue());
    --archiveLimits.maximumArchiveBytes;
    QCOMPARE(
        Archive::inspect(archivePath, archiveLimits).error().code,
        ArchiveErrorCode::ArchiveSizeLimit);

    ArchiveLimits componentLimits = limits;
    componentLimits.maximumComponentBytes = 3;
    componentLimits.maximumPathBytes = 16;
    const QString componentPath = writeArchive(
        temporary,
        makeZip({RawEntry{QByteArrayLiteral("abcd")}}));
    QCOMPARE(
        Archive::inspect(componentPath, componentLimits).error().code,
        ArchiveErrorCode::InvalidEntryPath);

    ArchiveLimits utf16Limits = limits;
    utf16Limits.maximumPathBytes = 8;
    utf16Limits.maximumComponentBytes = 8;
    utf16Limits.maximumPathUtf16Units = 1;
    utf16Limits.maximumComponentUtf16Units = 1;
    const QString utf16Path = writeArchive(
        temporary,
        makeZip({RawEntry{QString::fromUtf8("\xF0\x90\x80\x80").toUtf8()}}));
    QCOMPARE(
        Archive::inspect(utf16Path, utf16Limits).error().code,
        ArchiveErrorCode::InvalidEntryPath);
}

void ArchiveTest::boundsArchiveReadsAfterOpen()
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray bytes = makeZip({RawEntry{QByteArrayLiteral("safe.txt")}});
    ArchiveLimits limits;
    limits.maximumArchiveBytes = static_cast<quint64>(bytes.size());

    auto appendAfterSizeCheck = [](const QString &path) {
        QFile file(path);
        if (file.open(QIODevice::Append)) {
            (void)file.write("!", 1);
        }
    };
    {
        const QString path = writeArchive(temporary, bytes);
        ArchiveHookGuard guard({appendAfterSizeCheck});
        QCOMPARE(
            Archive::inspect(path, limits).error().code,
            ArchiveErrorCode::ArchiveSizeLimit);
    }
    {
        const QString path = writeArchive(temporary, bytes);
        const QString staging = temporary.filePath(QStringLiteral("staging"));
        QVERIFY(QDir().mkdir(staging));
        ArchiveHookGuard guard({appendAfterSizeCheck});
        QCOMPARE(
            Archive::extract(path, staging, limits).error().code,
            ArchiveErrorCode::ArchiveSizeLimit);
        QVERIFY(QDir(staging).isEmpty());
    }
#else
    QSKIP("archive test hooks are unavailable");
#endif
}

void ArchiveTest::rejectsUnrepresentableArchiveLimit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = writeArchive(
        temporary, makeZip({RawEntry{QByteArrayLiteral("safe.txt")}}));
    ArchiveLimits limits;
    limits.maximumArchiveBytes = std::numeric_limits<quint64>::max();
    QCOMPARE(
        Archive::inspect(path, limits).error().code,
        ArchiveErrorCode::ArchiveSizeLimit);
}

void ArchiveTest::rejectsSourceGrowthAfterPreflight()
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    QVERIFY(writeFile(source + QStringLiteral("/data.txt"), "safe"));
    const QString output = temporary.filePath(QStringLiteral("output.qapkg"));
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforeSourceRead = [](const QString &path, const QByteArray &) {
        QFile file(path);
        if (file.open(QIODevice::Append)) {
            (void)file.write("!", 1);
        }
    };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::create(source, output);
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, ArchiveErrorCode::SourceUnavailable);
    QVERIFY(!QFileInfo::exists(output));
#else
    QSKIP("archive test hooks are unavailable");
#endif
}

void ArchiveTest::rejectsSourceShrinkAfterPreflight()
{
#ifdef Q_BROWSER_ARCHIVE_TESTING
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    QVERIFY(writeFile(source + QStringLiteral("/data.txt"), "safe"));
    const QString output = temporary.filePath(QStringLiteral("output.qapkg"));
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforeSourceRead = [](const QString &path, const QByteArray &) {
        QFile file(path);
        if (file.open(QIODevice::ReadWrite)) {
            (void)file.resize(2);
        }
    };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::create(source, output);
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, ArchiveErrorCode::SourceUnavailable);
    QVERIFY(!QFileInfo::exists(output));
#else
    QSKIP("archive test hooks are unavailable");
#endif
}

void ArchiveTest::exposesCanonicalContentDigestView()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = writeArchive(
        temporary,
        makeZip({RawEntry{QByteArrayLiteral("z.txt")},
                 RawEntry{QByteArrayLiteral("metadata/signature.ed25519")},
                 RawEntry{QByteArrayLiteral("a.txt")},
                 RawEntry{QByteArrayLiteral("metadata/other.ed25519")}}));
    const ArchiveResult result = Archive::inspect(path);
    QVERIFY(result.hasValue());
    const QVector<ArchiveEntry> contentEntries = result.contentDigestEntries();
    QCOMPARE(contentEntries.size(), 3);
    QCOMPARE(contentEntries.at(0).path, QByteArray("a.txt"));
    QCOMPARE(contentEntries.at(1).path, QByteArray("metadata/other.ed25519"));
    QCOMPARE(contentEntries.at(2).path, QByteArray("z.txt"));
}

void ArchiveTest::extractsOnlyAfterCompleteVerification()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RawEntry main{QByteArrayLiteral("qml/Main.qml")};
    main.data = QByteArrayLiteral("import QtQuick\n");
    RawEntry asset{QByteArrayLiteral("assets/a.txt")};
    asset.data = QByteArrayLiteral("asset-data");
    RawEntry empty{QByteArrayLiteral("empty.txt")};
    empty.data.clear();
    const QString package = writeArchive(temporary, makeZip({main, asset, empty}));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));

    const ArchiveResult result = Archive::extract(package, staging);
    QVERIFY2(result.hasValue(), qPrintable(result.error().message));
    QCOMPARE(result.entries().size(), 3);

    QFile mainFile(staging + QStringLiteral("/qml/Main.qml"));
    QVERIFY(mainFile.open(QIODevice::ReadOnly));
    QCOMPARE(mainFile.readAll(), main.data);
    QFile assetFile(staging + QStringLiteral("/assets/a.txt"));
    QVERIFY(assetFile.open(QIODevice::ReadOnly));
    QCOMPARE(assetFile.readAll(), asset.data);
    QFile emptyFile(staging + QStringLiteral("/empty.txt"));
    QVERIFY(emptyFile.open(QIODevice::ReadOnly));
    QCOMPARE(emptyFile.size(), 0);
}

void ArchiveTest::leavesStagingEmptyWhenVerificationFails()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));

    RawEntry good{QByteArrayLiteral("first.txt")};
    good.data = QByteArrayLiteral("good");
    RawEntry bad{QByteArrayLiteral("second.txt")};
    bad.data = QByteArrayLiteral("secret-content");
    bad.crc = 0;
    const QString corrupt = writeArchive(temporary, makeZip({good, bad}));
    const ArchiveResult corruptResult = Archive::extract(corrupt, staging);
    QVERIFY(!corruptResult.hasValue());
    QCOMPARE(corruptResult.error().code, ArchiveErrorCode::InvalidArchive);
    QVERIFY(!corruptResult.error().message.contains(QStringLiteral("secret-content")));
    QVERIFY(QDir(staging).isEmpty());

    const QString traversal = writeArchive(
        temporary,
        makeZip({RawEntry{QByteArrayLiteral("../outside.txt")}}));
    const ArchiveResult traversalResult = Archive::extract(traversal, staging);
    QVERIFY(!traversalResult.hasValue());
    QCOMPARE(traversalResult.error().code, ArchiveErrorCode::InvalidEntryPath);
    QVERIFY(QDir(staging).isEmpty());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("outside.txt"))));
}

void ArchiveTest::requiresEmptyNonReparseStagingRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary,
        makeZip({RawEntry{QByteArrayLiteral("safe.txt")}}));

    const QString nonempty = temporary.filePath(QStringLiteral("nonempty"));
    QVERIFY(QDir().mkdir(nonempty));
    QFile marker(nonempty + QStringLiteral("/marker"));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();
    QCOMPARE(
        Archive::extract(package, nonempty).error().code,
        ArchiveErrorCode::UnsafeStagingRoot);
    QVERIFY(QFileInfo::exists(marker.fileName()));

    const QString target = temporary.filePath(QStringLiteral("target"));
    const QString junction = temporary.filePath(QStringLiteral("junction"));
    QVERIFY(QDir().mkdir(target));
    const int junctionExit = QProcess::execute(
        QStringLiteral("cmd.exe"),
        {QStringLiteral("/d"),
         QStringLiteral("/c"),
         QStringLiteral("mklink"),
         QStringLiteral("/J"),
         QDir::toNativeSeparators(junction),
         QDir::toNativeSeparators(target)});
    if (junctionExit != 0) {
        QSKIP("Windows junction creation is unavailable");
    }
    QCOMPARE(
        Archive::extract(package, junction).error().code,
        ArchiveErrorCode::UnsafeStagingRoot);
    QVERIFY(QDir(target).isEmpty());
}

void ArchiveTest::stagingRootCannotBeReplacedAfterGuard()
{
#if defined(Q_OS_WIN) && defined(Q_BROWSER_ARCHIVE_TESTING)
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary, makeZip({RawEntry{QByteArrayLiteral("safe.txt")}}));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    const QString moved = temporary.filePath(QStringLiteral("moved-staging"));
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkdir(staging));
    QVERIFY(QDir().mkdir(outside));
    const QString marker = outside + QStringLiteral("/marker.txt");
    QVERIFY(writeFile(marker, "outside"));

    bool hookRan = false;
    bool renameSucceeded = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.afterStagingGuardOpened = [&](const QString &) {
        hookRan = true;
        renameSucceeded = movePathNoReplace(staging, moved);
        if (renameSucceeded) {
            (void)createJunction(staging, outside);
        }
    };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::extract(package, staging);
    QVERIFY(hookRan);
    QVERIFY(!renameSucceeded);
    QVERIFY2(result.hasValue(), qPrintable(result.error().message));
    QVERIFY(QFileInfo::exists(staging + QStringLiteral("/safe.txt")));
    QVERIFY(QFileInfo::exists(marker));
    QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/safe.txt")));
    QVERIFY(movePathNoReplace(staging, moved));
    QVERIFY(movePathNoReplace(moved, staging));
#else
    QSKIP("Windows archive race tests are unavailable");
#endif
}

void ArchiveTest::createdParentCannotBeReplacedAfterGuard()
{
#if defined(Q_OS_WIN) && defined(Q_BROWSER_ARCHIVE_TESTING)
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary, makeZip({RawEntry{QByteArrayLiteral("parent/safe.txt")}}));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    const QString moved = staging + QStringLiteral("/moved-parent");
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkdir(staging));
    QVERIFY(QDir().mkdir(outside));
    const QString marker = outside + QStringLiteral("/marker.txt");
    QVERIFY(writeFile(marker, "outside"));

    bool hookRan = false;
    bool renameSucceeded = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.afterParentGuardOpened =
        [&](const QString &parent, const QByteArray &) {
            hookRan = true;
            renameSucceeded = movePathNoReplace(parent, moved);
            if (renameSucceeded) {
                (void)createJunction(parent, outside);
            }
        };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::extract(package, staging);
    QVERIFY(hookRan);
    QVERIFY(!renameSucceeded);
    QVERIFY2(result.hasValue(), qPrintable(result.error().message));
    QVERIFY(QFileInfo::exists(staging + QStringLiteral("/parent/safe.txt")));
    QVERIFY(QFileInfo::exists(marker));
    QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/safe.txt")));
#else
    QSKIP("Windows archive race tests are unavailable");
#endif
}

void ArchiveTest::doesNotOverwriteConcurrentTargetOrDeleteUserFiles()
{
#if defined(Q_OS_WIN) && defined(Q_BROWSER_ARCHIVE_TESTING)
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary, makeZip({RawEntry{QByteArrayLiteral("safe.txt")}}));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));
    const QString target = staging + QStringLiteral("/safe.txt");
    const QString marker = staging + QStringLiteral("/user.marker");

    bool hookRan = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforePublish = [&](const QString &path, const QByteArray &) {
        hookRan = true;
        QCOMPARE(path, target);
        QVERIFY(writeFile(target, "user"));
        QVERIFY(writeFile(marker, "marker"));
    };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::extract(package, staging);
    QVERIFY(hookRan);
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, ArchiveErrorCode::ExtractionFailed);
    QFile targetFile(target);
    QVERIFY(targetFile.open(QIODevice::ReadOnly));
    QCOMPARE(targetFile.readAll(), QByteArray("user"));
    QVERIFY(QFileInfo::exists(marker));
#else
    QSKIP("Windows archive race tests are unavailable");
#endif
}

void ArchiveTest::cleansOnlyOwnedObjectsAfterFailure()
{
#if defined(Q_OS_WIN) && defined(Q_BROWSER_ARCHIVE_TESTING)
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString package = writeArchive(
        temporary,
        makeZip({RawEntry{QByteArrayLiteral("owned/a.txt")},
                 RawEntry{QByteArrayLiteral("owned/b.txt")}}));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    const QString parent = staging + QStringLiteral("/owned");
    const QString moved = staging + QStringLiteral("/moved-owned");
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkdir(staging));
    QVERIFY(QDir().mkdir(outside));
    const QString outsideMarker = outside + QStringLiteral("/outside.marker");
    QVERIFY(writeFile(outsideMarker, "outside"));
    const QString marker = staging + QStringLiteral("/user.marker");
    const QString target = parent + QStringLiteral("/b.txt");

    bool cleanupHookRan = false;
    bool cleanupRenameSucceeded = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforePublish = [&](const QString &path, const QByteArray &entry) {
        if (entry == QByteArrayLiteral("owned/b.txt")) {
            QCOMPARE(path, target);
            QVERIFY(writeFile(target, "user"));
            QVERIFY(writeFile(marker, "marker"));
        }
    };
    hooks.beforeFailureCleanup = [&](const QString &) {
        cleanupHookRan = true;
        cleanupRenameSucceeded = movePathNoReplace(parent, moved);
        if (cleanupRenameSucceeded) {
            (void)createJunction(parent, outside);
        }
    };
    ArchiveHookGuard guard(std::move(hooks));
    const ArchiveResult result = Archive::extract(package, staging);
    QVERIFY(!result.hasValue());
    QVERIFY(cleanupHookRan);
    QVERIFY(!cleanupRenameSucceeded);
    QVERIFY(!QFileInfo::exists(parent + QStringLiteral("/a.txt")));
    QFile targetFile(target);
    QVERIFY(targetFile.open(QIODevice::ReadOnly));
    QCOMPARE(targetFile.readAll(), QByteArray("user"));
    QVERIFY(QFileInfo::exists(marker));
    QVERIFY(QFileInfo::exists(outsideMarker));
    QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/a.txt")));
#else
    QSKIP("Windows archive race tests are unavailable");
#endif
}

void ArchiveTest::sourceParentsRemainStableDuringRead()
{
#if defined(Q_OS_WIN) && defined(Q_BROWSER_ARCHIVE_TESTING)
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    const QString parent = source + QStringLiteral("/parent");
    const QString moved = source + QStringLiteral("/moved-parent");
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkpath(parent));
    QVERIFY(QDir().mkdir(outside));
    QVERIFY(writeFile(parent + QStringLiteral("/safe.txt"), "safe"));
    QVERIFY(writeFile(outside + QStringLiteral("/safe.txt"), "secret"));

    bool hookRan = false;
    bool renameSucceeded = false;
    qbrowser_archive_testing::ArchiveTestHooks hooks;
    hooks.beforeSourceRead = [&](const QString &, const QByteArray &) {
        hookRan = true;
        renameSucceeded = movePathNoReplace(parent, moved);
        if (renameSucceeded) {
            (void)createJunction(parent, outside);
        }
    };
    ArchiveHookGuard guard(std::move(hooks));
    const QString package = temporary.filePath(QStringLiteral("output.qapkg"));
    const ArchiveResult result = Archive::create(source, package);
    QVERIFY(hookRan);
    QVERIFY(!renameSucceeded);
    QVERIFY2(result.hasValue(), qPrintable(result.error().message));

    QVERIFY(movePathNoReplace(parent, moved));
    QVERIFY(movePathNoReplace(moved, parent));

    qbrowser_archive_testing::resetArchiveTestHooks();
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkdir(staging));
    QVERIFY(Archive::extract(package, staging).hasValue());
    QFile extracted(staging + QStringLiteral("/parent/safe.txt"));
    QVERIFY(extracted.open(QIODevice::ReadOnly));
    QCOMPARE(extracted.readAll(), QByteArray("safe"));
#else
    QSKIP("Windows archive race tests are unavailable");
#endif
}

void ArchiveTest::writesByteForByteDeterministicArchives()
{
    TimeZoneGuard timeZone;
    timeZone.set(QByteArrayLiteral("UTC0"));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    const QString bmpName = QStringLiteral("\uE000.txt");
    const QString supplementaryName = QString::fromUtf8("\xF0\x90\x80\x80.txt");
    QVERIFY(writeFile(source + QLatin1Char('/') + supplementaryName, "supplementary"));
    QVERIFY(writeFile(source + QLatin1Char('/') + bmpName, "bmp"));

    const QString firstPath = temporary.filePath(QStringLiteral("first.qapkg"));
    const ArchiveResult first = Archive::create(source, firstPath);
    QVERIFY2(first.hasValue(), qPrintable(first.error().message));
    QFile firstFile(firstPath);
    QVERIFY(firstFile.open(QIODevice::ReadOnly));
    const QByteArray firstBytes = firstFile.readAll();

    QFile bmp(source + QLatin1Char('/') + bmpName);
    QVERIFY(bmp.open(QIODevice::ReadWrite));
    QVERIFY(bmp.setFileTime(
        QDateTime::currentDateTime().addDays(-7),
        QFileDevice::FileModificationTime));
    bmp.close();
    QVERIFY(QFile::remove(source + QLatin1Char('/') + supplementaryName));
    QVERIFY(QFile::remove(source + QLatin1Char('/') + bmpName));
    QVERIFY(writeFile(source + QLatin1Char('/') + bmpName, "bmp"));
    QVERIFY(writeFile(source + QLatin1Char('/') + supplementaryName, "supplementary"));
    timeZone.set(QByteArrayLiteral("UTC+12"));

    const QString secondPath = temporary.filePath(QStringLiteral("second.qapkg"));
    const ArchiveResult second = Archive::create(source, secondPath);
    QVERIFY2(second.hasValue(), qPrintable(second.error().message));
    QFile secondFile(secondPath);
    QVERIFY(secondFile.open(QIODevice::ReadOnly));
    const QByteArray secondBytes = secondFile.readAll();

    QCOMPARE(secondBytes, firstBytes);
    QCOMPARE(
        QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256),
        QCryptographicHash::hash(firstBytes, QCryptographicHash::Sha256));

    const QVector<CentralMetadata> metadata = centralMetadata(firstBytes);
    QCOMPARE(metadata.size(), 2);
    QCOMPARE(metadata.at(0).name, bmpName.toUtf8());
    QCOMPARE(metadata.at(1).name, supplementaryName.toUtf8());
    for (const CentralMetadata &entry : metadata) {
        QCOMPARE(entry.versionMadeBy, static_cast<quint16>(0x0314U));
        QCOMPARE(entry.time, static_cast<quint16>(0U));
        QCOMPARE(entry.date, static_cast<quint16>(0x0021U));
        QCOMPARE(entry.localTime, static_cast<quint16>(0U));
        QCOMPARE(entry.localDate, static_cast<quint16>(0x0021U));
        QCOMPARE(entry.externalAttributes, 0x81A40000U);
    }

    QFile sourceFile(QStringLiteral(Q_BROWSER_ARCHIVE_SOURCE_FILE));
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceCode = sourceFile.readAll();
    QVERIFY(!sourceCode.contains("std::mktime"));
    QVERIFY(!sourceCode.contains("MZ_TIME_T"));
    QVERIFY(!sourceCode.contains("#include <ctime>"));
}

void ArchiveTest::rejectsUnsafeSourceTreesBeforeWriting()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));
    QVERIFY(writeFile(source + QStringLiteral("/a"), "a"));
    QVERIFY(writeFile(source + QStringLiteral("/b"), "b"));

    ArchiveLimits limits;
    limits.maximumEntries = 1;
    QString output = temporary.filePath(QStringLiteral("count.qapkg"));
    QCOMPARE(
        Archive::create(source, output, limits).error().code,
        ArchiveErrorCode::EntryCountLimit);
    QVERIFY(!QFileInfo::exists(output));

    limits.maximumEntries = 2;
    limits.maximumEntryBytes = 0;
    output = temporary.filePath(QStringLiteral("size.qapkg"));
    QCOMPARE(
        Archive::create(source, output, limits).error().code,
        ArchiveErrorCode::EntrySizeLimit);
    QVERIFY(!QFileInfo::exists(output));

    limits.maximumEntryBytes = 1;
    limits.maximumTotalBytes = 1;
    output = temporary.filePath(QStringLiteral("aggregate.qapkg"));
    QCOMPARE(
        Archive::create(source, output, limits).error().code,
        ArchiveErrorCode::TotalSizeLimit);
    QVERIFY(!QFileInfo::exists(output));

    QVERIFY(QFile::remove(source + QStringLiteral("/a")));
    QVERIFY(QFile::remove(source + QStringLiteral("/b")));
    QVERIFY(writeFile(source + QStringLiteral("/é"), "a"));
    QVERIFY(writeFile(source + QStringLiteral("/e\u0301"), "b"));
    output = temporary.filePath(QStringLiteral("collision.qapkg"));
    QCOMPARE(
        Archive::create(source, output).error().code,
        ArchiveErrorCode::DuplicateEntryPath);
    QVERIFY(!QFileInfo::exists(output));

    QVERIFY(QDir(source).removeRecursively());
    QVERIFY(QDir().mkdir(source));
    const QString target = temporary.filePath(QStringLiteral("outside-source"));
    const QString junction = source + QStringLiteral("/linked");
    QVERIFY(QDir().mkdir(target));
    QVERIFY(writeFile(target + QStringLiteral("/secret"), "must-not-pack"));
    QCOMPARE(
        QProcess::execute(
            QStringLiteral("cmd.exe"),
            {QStringLiteral("/d"),
             QStringLiteral("/c"),
             QStringLiteral("mklink"),
             QStringLiteral("/J"),
             QDir::toNativeSeparators(junction),
             QDir::toNativeSeparators(target)}),
        0);
    output = temporary.filePath(QStringLiteral("junction.qapkg"));
    QCOMPARE(
        Archive::create(source, output).error().code,
        ArchiveErrorCode::UnsupportedEntry);
    QVERIFY(!QFileInfo::exists(output));
}

QTEST_MAIN(ArchiveTest)

#include "tst_archive.moc"
