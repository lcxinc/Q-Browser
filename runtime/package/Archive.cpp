#include "Archive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#include <miniz.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{
ArchiveResult fail(ArchiveErrorCode code, QByteArray path, QString message)
{
    return ArchiveResult::failure({code, std::move(path), std::move(message)});
}

ArchiveResult inspectBytes(
    const QByteArray &bytes,
    const ArchiveLimits &limits);
bool isReparsePoint(const QString &path);
bool pathChainContainsReparsePoint(const QString &path);

struct SourceEntry final
{
    QString filesystemPath;
    QByteArray archiveName;
    quint64 size = 0;
};

bool bytewiseLess(const QByteArray &left, const QByteArray &right)
{
    const qsizetype common = std::min(left.size(), right.size());
    const int comparison = std::memcmp(
        left.constData(), right.constData(), static_cast<size_t>(common));
    return comparison < 0 || (comparison == 0 && left.size() < right.size());
}

quint16 readLittle16(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint32 readLittle32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

void writeLittle16(QByteArray &bytes, qsizetype offset, quint16 value)
{
    qToLittleEndian<quint16>(
        value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

void writeLittle32(QByteArray &bytes, qsizetype offset, quint32 value)
{
    qToLittleEndian<quint32>(
        value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

bool applyDeterministicCentralMetadata(QByteArray &bytes)
{
    const qsizetype endOffset = bytes.lastIndexOf(QByteArrayLiteral("PK\x05\x06"));
    if (endOffset < 0 || endOffset + 22 > bytes.size()) {
        return false;
    }
    const quint16 count = readLittle16(bytes, endOffset + 10);
    qsizetype offset = static_cast<qsizetype>(
        readLittle32(bytes, endOffset + 16));
    for (quint16 index = 0; index < count; ++index) {
        if (offset + 46 > bytes.size()
            || readLittle32(bytes, offset) != 0x02014B50U) {
            return false;
        }
        writeLittle16(bytes, offset + 4, 0x0314U);
        writeLittle16(bytes, offset + 12, 0U);
        writeLittle16(bytes, offset + 14, 0x0021U);
        writeLittle16(bytes, offset + 36, 0U);
        writeLittle32(bytes, offset + 38, 0x81A40000U);
        const qsizetype localOffset = static_cast<qsizetype>(
            readLittle32(bytes, offset + 42));
        if (localOffset < 0 || localOffset + 30 > bytes.size()
            || readLittle32(bytes, localOffset) != 0x04034B50U) {
            return false;
        }
        writeLittle16(bytes, localOffset + 10, 0U);
        writeLittle16(bytes, localOffset + 12, 0x0021U);
        const quint16 nameLength = readLittle16(bytes, offset + 28);
        const quint16 extraLength = readLittle16(bytes, offset + 30);
        const quint16 commentLength = readLittle16(bytes, offset + 32);
        offset += 46 + nameLength + extraLength + commentLength;
    }
    return offset == endOffset;
}

struct ZipEnvelope final
{
    qsizetype centralOffset = 0;
    qsizetype endOffset = 0;
    quint16 entryCount = 0;
};

struct ZipEnvelopeResult final
{
    std::optional<ZipEnvelope> envelope;
    ArchiveError error;
};

ZipEnvelopeResult preflightZipEnvelope(
    const QByteArray &bytes,
    const ArchiveLimits &limits)
{
    constexpr qsizetype endHeaderSize = 22;
    if (bytes.size() < endHeaderSize) {
        return {{}, {ArchiveErrorCode::InvalidArchive, {},
                     QStringLiteral("archive structure is invalid")}};
    }
    const qsizetype endOffset = bytes.size() - endHeaderSize;
    if (readLittle32(bytes, endOffset) != 0x06054B50U
        || readLittle16(bytes, endOffset + 20) != 0U) {
        return {{}, {ArchiveErrorCode::InvalidArchive, {},
                     QStringLiteral("archive structure is invalid")}};
    }
    const quint16 disk = readLittle16(bytes, endOffset + 4);
    const quint16 centralDisk = readLittle16(bytes, endOffset + 6);
    const quint16 diskEntries = readLittle16(bytes, endOffset + 8);
    const quint16 totalEntries = readLittle16(bytes, endOffset + 10);
    const quint32 centralSize = readLittle32(bytes, endOffset + 12);
    const quint32 centralOffset = readLittle32(bytes, endOffset + 16);
    if (disk != 0U || centralDisk != 0U || diskEntries != totalEntries
        || totalEntries == 0xFFFFU
        || static_cast<quint64>(centralOffset) + centralSize
            != static_cast<quint64>(endOffset)) {
        return {{}, {ArchiveErrorCode::InvalidArchive, {},
                     QStringLiteral("archive structure is invalid")}};
    }
    if (static_cast<quint64>(totalEntries) > limits.maximumEntries) {
        return {{}, {ArchiveErrorCode::EntryCountLimit, {},
                     QStringLiteral("archive entry count is too large")}};
    }
    return {ZipEnvelope{static_cast<qsizetype>(centralOffset),
                        endOffset,
                        totalEntries}, {}};
}

bool centralAndLocalMetadataMatch(
    const QByteArray &bytes,
    qsizetype &centralCursor,
    const QByteArray &name)
{
    constexpr qsizetype centralHeaderSize = 46;
    constexpr qsizetype localHeaderSize = 30;
    if (centralCursor < 0 || centralCursor + centralHeaderSize > bytes.size()
        || readLittle32(bytes, centralCursor) != 0x02014B50U) {
        return false;
    }
    const quint16 nameLength = readLittle16(bytes, centralCursor + 28);
    const quint16 centralExtraLength = readLittle16(bytes, centralCursor + 30);
    const quint16 commentLength = readLittle16(bytes, centralCursor + 32);
    const qsizetype centralRecordSize = centralHeaderSize + nameLength
        + centralExtraLength + commentLength;
    if (centralCursor + centralRecordSize > bytes.size()
        || centralExtraLength != 0U || commentLength != 0U
        || readLittle16(bytes, centralCursor + 34) != 0U
        || readLittle16(bytes, centralCursor + 36) != 0U
        || bytes.mid(centralCursor + centralHeaderSize, nameLength) != name) {
        return false;
    }

    const qsizetype localOffset = static_cast<qsizetype>(
        readLittle32(bytes, centralCursor + 42));
    if (localOffset < 0 || localOffset + localHeaderSize > bytes.size()
        || readLittle32(bytes, localOffset) != 0x04034B50U) {
        return false;
    }
    const quint16 localNameLength = readLittle16(bytes, localOffset + 26);
    const quint16 localExtraLength = readLittle16(bytes, localOffset + 28);
    if (localExtraLength != 0U || localNameLength != nameLength
        || localOffset + localHeaderSize + localNameLength > bytes.size()
        || bytes.mid(localOffset + localHeaderSize, localNameLength) != name) {
        return false;
    }

    const bool fixedFieldsMatch =
        readLittle16(bytes, localOffset + 4)
            == readLittle16(bytes, centralCursor + 6)
        && readLittle16(bytes, localOffset + 6)
            == readLittle16(bytes, centralCursor + 8)
        && readLittle16(bytes, localOffset + 8)
            == readLittle16(bytes, centralCursor + 10)
        && readLittle16(bytes, localOffset + 10)
            == readLittle16(bytes, centralCursor + 12)
        && readLittle16(bytes, localOffset + 12)
            == readLittle16(bytes, centralCursor + 14)
        && readLittle32(bytes, localOffset + 14)
            == readLittle32(bytes, centralCursor + 16)
        && readLittle32(bytes, localOffset + 18)
            == readLittle32(bytes, centralCursor + 20)
        && readLittle32(bytes, localOffset + 22)
            == readLittle32(bytes, centralCursor + 24);
    centralCursor += centralRecordSize;
    return fixedFieldsMatch;
}

std::optional<QByteArray> readArchiveName(mz_zip_archive *zip, mz_uint index)
{
    const mz_uint required = mz_zip_reader_get_filename(zip, index, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    QByteArray name(static_cast<qsizetype>(required - 1U), Qt::Uninitialized);
    QVector<char> buffer(static_cast<qsizetype>(required));
    if (mz_zip_reader_get_filename(zip, index, buffer.data(), required) != required) {
        return std::nullopt;
    }
    if (!name.isEmpty()) {
        std::memcpy(
            name.data(),
            buffer.constData(),
            static_cast<size_t>(name.size()));
    }
    return name;
}

bool isWindowsDeviceName(const QString &component)
{
    const QString base = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> fixedNames{
        QStringLiteral("CON"),
        QStringLiteral("PRN"),
        QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("CLOCK$")};
    if (fixedNames.contains(base)) {
        return true;
    }
    if (base.size() == 4
        && (base.startsWith(QStringLiteral("COM"))
            || base.startsWith(QStringLiteral("LPT")))) {
        const QChar suffix = base.back();
        return (suffix >= QLatin1Char('1') && suffix <= QLatin1Char('9'))
            || suffix == QChar(0x00B9U) || suffix == QChar(0x00B2U)
            || suffix == QChar(0x00B3U);
    }
    return false;
}

struct CheckedPath final
{
    QString collisionKey;
    bool isCanonical = true;
};

struct CollisionPath final
{
    QString key;
    QByteArray path;
    bool isCanonical = true;
};

std::optional<QByteArray> findPathCollision(
    const QVector<CollisionPath> &paths)
{
    QHash<QString, QByteArray> firstPathByKey;
    QSet<QString> keys;
    QVector<QByteArray> candidates;
    for (const CollisionPath &path : paths) {
        const auto existing = firstPathByKey.constFind(path.key);
        if (existing != firstPathByKey.cend()) {
            candidates.push_back(
                bytewiseLess(*existing, path.path) ? path.path : *existing);
        } else {
            firstPathByKey.insert(path.key, path.path);
            keys.insert(path.key);
        }
    }
    for (const CollisionPath &path : paths) {
        qsizetype separator = path.key.indexOf(QLatin1Char('/'));
        while (separator >= 0) {
            if (keys.contains(path.key.left(separator))) {
                candidates.push_back(path.path);
                break;
            }
            separator = path.key.indexOf(QLatin1Char('/'), separator + 1);
        }
    }
    if (candidates.isEmpty()) {
        return std::nullopt;
    }
    return *std::min_element(
        candidates.cbegin(),
        candidates.cend(),
        [](const QByteArray &left, const QByteArray &right) {
            return bytewiseLess(left, right);
        });
}

std::optional<QByteArray> firstNonCanonicalPath(
    const QVector<CollisionPath> &paths)
{
    std::optional<QByteArray> result;
    for (const CollisionPath &path : paths) {
        if (!path.isCanonical
            && (!result.has_value() || bytewiseLess(path.path, *result))) {
            result = path.path;
        }
    }
    return result;
}

std::optional<CheckedPath> validateEntryPath(
    const QByteArray &path,
    const ArchiveLimits &limits)
{
    if (path.isEmpty() || path.size() > limits.maximumPathBytes
        || path.contains('\0') || path.contains('\\') || path.startsWith('/')) {
        return std::nullopt;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(path);
    if (decoder.hasError() || decoded.isEmpty()
        || decoded.size() > limits.maximumPathUtf16Units) {
        return std::nullopt;
    }
    const QString normalized = decoded.normalized(QString::NormalizationForm_C);

    const QList<QByteArray> rawComponents = path.split('/');
    const QStringList components = decoded.split(QLatin1Char('/'));
    if (rawComponents.size() != components.size()) {
        return std::nullopt;
    }
    static const QString reserved = QStringLiteral("<>:\"|?*");
    for (qsizetype index = 0; index < components.size(); ++index) {
        const QByteArray &raw = rawComponents.at(index);
        const QString &component = components.at(index);
        if (raw.isEmpty() || raw.size() > limits.maximumComponentBytes
            || component.isEmpty()
            || component.size() > limits.maximumComponentUtf16Units
            || component == QStringLiteral(".")
            || component == QStringLiteral("..")
            || component.endsWith(QLatin1Char('.'))
            || component.endsWith(QLatin1Char(' '))
            || isWindowsDeviceName(component)) {
            return std::nullopt;
        }
        for (const QChar character : component) {
            if (character.unicode() < 0x20U || reserved.contains(character)) {
                return std::nullopt;
            }
        }
    }
    return CheckedPath{normalized.toCaseFolded(), normalized == decoded};
}

class ZipReader final
{
public:
    ZipReader()
    {
        mz_zip_zero_struct(&m_zip);
    }

    ~ZipReader()
    {
        if (m_initialized) {
            (void)mz_zip_reader_end(&m_zip);
        }
    }

    [[nodiscard]] bool initialize(const QByteArray &bytes)
    {
        m_initialized = mz_zip_reader_init_mem(
                            &m_zip,
                            bytes.constData(),
                            static_cast<size_t>(bytes.size()),
                            0)
            == MZ_TRUE;
        return m_initialized;
    }

    [[nodiscard]] mz_zip_archive *get() noexcept
    {
        return &m_zip;
    }

private:
    mz_zip_archive m_zip{};
    bool m_initialized = false;
};

size_t readMemory(
    void *opaque,
    mz_uint64 offset,
    void *destination,
    size_t requested)
{
    const auto *bytes = static_cast<const QByteArray *>(opaque);
    if (offset > static_cast<mz_uint64>(bytes->size())) {
        return 0;
    }
    const size_t remaining = static_cast<size_t>(bytes->size())
        - static_cast<size_t>(offset);
    const size_t count = std::min(remaining, requested);
    if (count > 0) {
        std::memcpy(
            destination,
            bytes->constData() + static_cast<qsizetype>(offset),
            count);
    }
    return count;
}
}

bool ArchiveEntry::isIncludedInContentDigest() const noexcept
{
    return path != QByteArrayLiteral("metadata/signature.ed25519");
}

ArchiveResult ArchiveResult::success(QVector<ArchiveEntry> entries)
{
    ArchiveResult result;
    result.m_entries.emplace(std::move(entries));
    return result;
}

ArchiveResult ArchiveResult::failure(ArchiveError error)
{
    ArchiveResult result;
    result.m_error = std::move(error);
    return result;
}

bool ArchiveResult::hasValue() const noexcept
{
    return m_entries.has_value();
}

const QVector<ArchiveEntry> &ArchiveResult::entries() const noexcept
{
    static const QVector<ArchiveEntry> empty;
    return m_entries.has_value() ? *m_entries : empty;
}

QVector<ArchiveEntry> ArchiveResult::contentDigestEntries() const
{
    QVector<ArchiveEntry> result;
    if (!m_entries.has_value()) {
        return result;
    }
    for (const ArchiveEntry &entry : *m_entries) {
        if (entry.isIncludedInContentDigest()) {
            result.push_back(entry);
        }
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const ArchiveEntry &left, const ArchiveEntry &right) {
            return bytewiseLess(left.path, right.path);
        });
    return result;
}

const ArchiveError &ArchiveResult::error() const noexcept
{
    return m_error;
}

ArchiveResult Archive::create(
    const QString &sourceRoot,
    const QString &archivePath,
    const ArchiveLimits &limits)
{
    const QFileInfo rootInfo(sourceRoot);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink()
        || pathChainContainsReparsePoint(sourceRoot)) {
        return fail(
            ArchiveErrorCode::SourceUnavailable,
            {},
            QStringLiteral("archive source is unavailable"));
    }

    QVector<SourceEntry> sourceEntries;
    QVector<CollisionPath> collisionPaths;
    quint64 totalSize = 0;
    const QDir root(sourceRoot);
    QDirIterator iterator(
        sourceRoot,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const QString relativePath = QDir::fromNativeSeparators(
            root.relativeFilePath(info.absoluteFilePath()));
        const QByteArray archiveName = relativePath.toUtf8();
        if (info.isSymLink() || isReparsePoint(info.absoluteFilePath())) {
            return fail(
                ArchiveErrorCode::UnsupportedEntry,
                archiveName,
                QStringLiteral("archive source entry is unsupported"));
        }
        if (info.isDir()) {
            continue;
        }
        if (!info.isFile()) {
            return fail(
                ArchiveErrorCode::UnsupportedEntry,
                archiveName,
                QStringLiteral("archive source entry is unsupported"));
        }

        const std::optional<CheckedPath> checkedPath = validateEntryPath(
            archiveName, limits);
        if (!checkedPath.has_value()) {
            return fail(
                ArchiveErrorCode::InvalidEntryPath,
                archiveName,
                QStringLiteral("archive source path is invalid"));
        }
        collisionPaths.push_back(
            {checkedPath->collisionKey, archiveName, checkedPath->isCanonical});

        if (static_cast<quint64>(sourceEntries.size())
            >= limits.maximumEntries) {
            return fail(
                ArchiveErrorCode::EntryCountLimit,
                {},
                QStringLiteral("archive entry count is too large"));
        }
        if (info.size() < 0
            || static_cast<quint64>(info.size()) > limits.maximumEntryBytes) {
            return fail(
                ArchiveErrorCode::EntrySizeLimit,
                archiveName,
                QStringLiteral("archive entry is too large"));
        }
        const quint64 size = static_cast<quint64>(info.size());
        if (totalSize > limits.maximumTotalBytes
            || size > limits.maximumTotalBytes - totalSize) {
            return fail(
                ArchiveErrorCode::TotalSizeLimit,
                archiveName,
                QStringLiteral("archive contents are too large"));
        }
        totalSize += size;
        sourceEntries.push_back({info.absoluteFilePath(), archiveName, size});
    }
    if (const std::optional<QByteArray> collision =
            findPathCollision(collisionPaths);
        collision.has_value()) {
        return fail(
            ArchiveErrorCode::DuplicateEntryPath,
            *collision,
            QStringLiteral("archive source paths collide"));
    }
    if (const std::optional<QByteArray> nonCanonical =
            firstNonCanonicalPath(collisionPaths);
        nonCanonical.has_value()) {
        return fail(
            ArchiveErrorCode::InvalidEntryPath,
            *nonCanonical,
            QStringLiteral("archive source path is invalid"));
    }
    std::sort(
        sourceEntries.begin(),
        sourceEntries.end(),
        [](const SourceEntry &left, const SourceEntry &right) {
            return bytewiseLess(left.archiveName, right.archiveName);
        });

    mz_zip_archive writer{};
    mz_zip_zero_struct(&writer);
    if (mz_zip_writer_init_heap(&writer, 0, 0) != MZ_TRUE) {
        return fail(
            ArchiveErrorCode::DestinationUnavailable,
            {},
            QStringLiteral("archive writer could not be initialized"));
    }

    bool writeSucceeded = true;
    for (const SourceEntry &sourceEntry : sourceEntries) {
        if (isReparsePoint(sourceEntry.filesystemPath)) {
            writeSucceeded = false;
            break;
        }
        QFile input(sourceEntry.filesystemPath);
        if (!input.open(QIODevice::ReadOnly)) {
            writeSucceeded = false;
            break;
        }
        QByteArray bytes = input.readAll();
        if (static_cast<quint64>(bytes.size()) != sourceEntry.size) {
            writeSucceeded = false;
            break;
        }
        constexpr mz_uint writerFlags = static_cast<mz_uint>(6)
            | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE;
        if (mz_zip_writer_add_read_buf_callback(
                &writer,
                sourceEntry.archiveName.constData(),
                readMemory,
                &bytes,
                static_cast<mz_uint64>(bytes.size()),
                nullptr,
                nullptr,
                0,
                writerFlags,
                nullptr,
                0,
                nullptr,
                0)
            != MZ_TRUE) {
            writeSucceeded = false;
            break;
        }
    }

    void *archiveBytes = nullptr;
    size_t archiveSize = 0;
    if (!writeSucceeded
        || mz_zip_writer_finalize_heap_archive(
               &writer, &archiveBytes, &archiveSize)
            != MZ_TRUE) {
        (void)mz_zip_writer_end(&writer);
        return fail(
            ArchiveErrorCode::DestinationUnavailable,
            {},
            QStringLiteral("archive could not be created"));
    }
    (void)mz_zip_writer_end(&writer);

    if (archiveSize > static_cast<size_t>(std::numeric_limits<qint64>::max())
        || static_cast<quint64>(archiveSize) > limits.maximumArchiveBytes) {
        mz_free(archiveBytes);
        return fail(
            ArchiveErrorCode::ArchiveSizeLimit,
            {},
            QStringLiteral("archive size is too large"));
    }
    QByteArray deterministicBytes(
        static_cast<const char *>(archiveBytes),
        static_cast<qsizetype>(archiveSize));
    mz_free(archiveBytes);
    if (!applyDeterministicCentralMetadata(deterministicBytes)) {
        return fail(
            ArchiveErrorCode::DestinationUnavailable,
            {},
            QStringLiteral("archive metadata could not be finalized"));
    }
    const ArchiveResult verified = inspectBytes(deterministicBytes, limits);
    if (!verified.hasValue()) {
        return verified;
    }

    QSaveFile output(archivePath);
    const bool opened = output.open(QIODevice::WriteOnly);
    const qint64 expectedSize = deterministicBytes.size();
    const bool wrote = opened
        && output.write(deterministicBytes) == expectedSize;
    if (!wrote || !output.commit()) {
        output.cancelWriting();
        return fail(
            ArchiveErrorCode::DestinationUnavailable,
            {},
            QStringLiteral("archive destination is unavailable"));
    }

    return verified;
}

namespace
{
ArchiveResult inspectBytes(
    const QByteArray &bytes,
    const ArchiveLimits &limits)
{
    const ZipEnvelopeResult envelopeResult = preflightZipEnvelope(bytes, limits);
    if (!envelopeResult.envelope.has_value()) {
        return ArchiveResult::failure(envelopeResult.error);
    }
    const ZipEnvelope &envelope = *envelopeResult.envelope;
    ZipReader reader;
    if (!reader.initialize(bytes)) {
        return fail(
            ArchiveErrorCode::InvalidArchive,
            {},
            QStringLiteral("archive structure is invalid"));
    }

    QVector<ArchiveEntry> entries;
    QVector<CollisionPath> collisionPaths;
    const mz_uint count = mz_zip_reader_get_num_files(reader.get());
    if (count != static_cast<mz_uint>(envelope.entryCount)) {
        return fail(
            ArchiveErrorCode::InvalidArchive,
            {},
            QStringLiteral("archive structure is invalid"));
    }
    entries.reserve(static_cast<qsizetype>(count));
    quint64 totalSize = 0;
    qsizetype centralCursor = envelope.centralOffset;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(reader.get(), index, &stat) != MZ_TRUE) {
            return fail(
                ArchiveErrorCode::InvalidArchive,
                {},
                QStringLiteral("archive entry metadata is invalid"));
        }
        const std::optional<QByteArray> name = readArchiveName(reader.get(), index);
        if (!name.has_value()) {
            return fail(
                ArchiveErrorCode::InvalidArchive,
                {},
                QStringLiteral("archive entry metadata is invalid"));
        }
        if (!centralAndLocalMetadataMatch(bytes, centralCursor, *name)) {
            return fail(
                ArchiveErrorCode::InvalidArchive,
                *name,
                QStringLiteral("archive entry metadata is invalid"));
        }
        const std::optional<CheckedPath> checkedPath = validateEntryPath(*name, limits);
        if (!checkedPath.has_value()) {
            return fail(
                ArchiveErrorCode::InvalidEntryPath,
                *name,
                QStringLiteral("archive entry path is invalid"));
        }
        collisionPaths.push_back(
            {checkedPath->collisionKey, *name, checkedPath->isCanonical});

        constexpr quint16 allowedFlags = 0x0800U;
        const quint16 unsupportedFlags = static_cast<quint16>(
            stat.m_bit_flag & static_cast<quint16>(~allowedFlags));
        if (stat.m_is_directory == MZ_TRUE || stat.m_is_encrypted == MZ_TRUE
            || stat.m_is_supported != MZ_TRUE || unsupportedFlags != 0U
            || (stat.m_method != 0U
                && stat.m_method != static_cast<quint16>(MZ_DEFLATED))) {
            return fail(
                ArchiveErrorCode::UnsupportedEntry,
                *name,
                QStringLiteral("archive entry type is unsupported"));
        }

        const quint16 creatorSystem = static_cast<quint16>(
            stat.m_version_made_by >> 8U);
        if (creatorSystem != 0U && creatorSystem != 3U) {
            return fail(
                ArchiveErrorCode::UnsupportedEntry,
                *name,
                QStringLiteral("archive entry type is unsupported"));
        }
        if (creatorSystem == 3U) {
            constexpr quint32 unixTypeMask = 0170000U;
            constexpr quint32 unixRegularFile = 0100000U;
            const quint32 unixMode = stat.m_external_attr >> 16U;
            if ((unixMode & unixTypeMask) != unixRegularFile) {
                return fail(
                    ArchiveErrorCode::UnsupportedEntry,
                    *name,
                    QStringLiteral("archive entry type is unsupported"));
            }
        } else {
            constexpr quint32 allowedDosAttributes =
                0x01U | 0x02U | 0x04U | 0x20U | 0x80U;
            if ((stat.m_external_attr & ~allowedDosAttributes) != 0U) {
                return fail(
                    ArchiveErrorCode::UnsupportedEntry,
                    *name,
                    QStringLiteral("archive entry type is unsupported"));
            }
        }
        if (stat.m_uncomp_size > limits.maximumEntryBytes) {
            return fail(
                ArchiveErrorCode::EntrySizeLimit,
                *name,
                QStringLiteral("archive entry is too large"));
        }
        if (totalSize > limits.maximumTotalBytes
            || stat.m_uncomp_size > limits.maximumTotalBytes - totalSize) {
            return fail(
                ArchiveErrorCode::TotalSizeLimit,
                *name,
                QStringLiteral("archive contents are too large"));
        }
        totalSize += stat.m_uncomp_size;
        const bool ratioExceeded = stat.m_uncomp_size > 0U
            && (stat.m_comp_size == 0U
                || (limits.maximumCompressionRatio > 0U
                    && stat.m_comp_size
                        <= std::numeric_limits<quint64>::max()
                            / limits.maximumCompressionRatio
                    && stat.m_uncomp_size
                        > stat.m_comp_size * limits.maximumCompressionRatio));
        if (limits.maximumCompressionRatio == 0U || ratioExceeded) {
            return fail(
                ArchiveErrorCode::CompressionRatioLimit,
                *name,
                QStringLiteral("archive compression ratio is too large"));
        }
        if (mz_zip_validate_file(
                reader.get(), index, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)
            != MZ_TRUE) {
            return fail(
                ArchiveErrorCode::InvalidArchive,
                *name,
                QStringLiteral("archive entry metadata is invalid"));
        }
        entries.push_back({*name, stat.m_uncomp_size, stat.m_comp_size});
    }
    if (const std::optional<QByteArray> collision =
            findPathCollision(collisionPaths);
        collision.has_value()) {
        return fail(
            ArchiveErrorCode::DuplicateEntryPath,
            *collision,
            QStringLiteral("archive entry path collides"));
    }
    if (const std::optional<QByteArray> nonCanonical =
            firstNonCanonicalPath(collisionPaths);
        nonCanonical.has_value()) {
        return fail(
            ArchiveErrorCode::InvalidEntryPath,
            *nonCanonical,
            QStringLiteral("archive entry path is invalid"));
    }
    if (centralCursor != envelope.endOffset) {
        return fail(
            ArchiveErrorCode::InvalidArchive,
            {},
            QStringLiteral("archive structure is invalid"));
    }
    for (mz_uint index = 0; index < count; ++index) {
        if (mz_zip_validate_file(reader.get(), index, 0) != MZ_TRUE) {
            return fail(
                ArchiveErrorCode::InvalidArchive,
                entries.at(static_cast<qsizetype>(index)).path,
                QStringLiteral("archive data is invalid"));
        }
    }
    return ArchiveResult::success(std::move(entries));
}

bool isReparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    return QFileInfo(path).isSymLink();
#endif
}

bool pathChainContainsReparsePoint(const QString &path)
{
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty()) {
        if (isReparsePoint(current)) {
            return true;
        }
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return false;
}

bool isEmptyDirectory(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isDir() && !info.isSymLink()
        && QDir(path).entryList(
               QDir::AllEntries | QDir::Hidden | QDir::System
                   | QDir::NoDotAndDotDot)
               .isEmpty();
}

void clearStagingDirectory(const QString &stagingRoot)
{
    QDir root(stagingRoot);
    const QFileInfoList children = root.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &child : children) {
        if (child.isDir() && !child.isSymLink()
            && !isReparsePoint(child.absoluteFilePath())) {
            (void)QDir(child.absoluteFilePath()).removeRecursively();
        } else if (child.isDir()) {
            (void)root.rmdir(child.fileName());
        } else {
            (void)QFile::remove(child.absoluteFilePath());
        }
    }
}

bool outputIsWithinRoot(const QString &root, const QString &output)
{
    const QString normalizedRoot = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
    const QString normalizedOutput = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(output).absoluteFilePath()));
    return normalizedOutput.startsWith(
        normalizedRoot + QLatin1Char('/'),
        Qt::CaseInsensitive);
}
}

ArchiveResult Archive::inspect(
    const QString &archivePath,
    const ArchiveLimits &limits)
{
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(
            ArchiveErrorCode::SourceUnavailable,
            {},
            QStringLiteral("archive source is unavailable"));
    }
    if (input.size() < 0
        || static_cast<quint64>(input.size()) > limits.maximumArchiveBytes) {
        return fail(
            ArchiveErrorCode::ArchiveSizeLimit,
            {},
            QStringLiteral("archive size is too large"));
    }
    const QByteArray bytes = input.readAll();
    if (bytes.size() != input.size()) {
        return fail(
            ArchiveErrorCode::SourceUnavailable,
            {},
            QStringLiteral("archive source could not be read"));
    }
    return inspectBytes(bytes, limits);
}

ArchiveResult Archive::extract(
    const QString &archivePath,
    const QString &stagingRoot,
    const ArchiveLimits &limits)
{
    if (!isEmptyDirectory(stagingRoot)
        || pathChainContainsReparsePoint(stagingRoot)) {
        return fail(
            ArchiveErrorCode::UnsafeStagingRoot,
            {},
            QStringLiteral("staging root is not an empty regular directory"));
    }

    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(
            ArchiveErrorCode::SourceUnavailable,
            {},
            QStringLiteral("archive source is unavailable"));
    }
    if (input.size() < 0
        || static_cast<quint64>(input.size()) > limits.maximumArchiveBytes) {
        return fail(
            ArchiveErrorCode::ArchiveSizeLimit,
            {},
            QStringLiteral("archive size is too large"));
    }
    const QByteArray bytes = input.readAll();
    if (bytes.size() != input.size()) {
        return fail(
            ArchiveErrorCode::SourceUnavailable,
            {},
            QStringLiteral("archive source could not be read"));
    }
    const ArchiveResult inspected = inspectBytes(bytes, limits);
    if (!inspected.hasValue()) {
        return inspected;
    }

    ZipReader reader;
    if (!reader.initialize(bytes)) {
        return fail(
            ArchiveErrorCode::InvalidArchive,
            {},
            QStringLiteral("archive structure is invalid"));
    }

    const QDir root(stagingRoot);
    QVector<char> buffer(64 * 1024);
    for (mz_uint index = 0;
         index < static_cast<mz_uint>(inspected.entries().size());
         ++index) {
        const ArchiveEntry &entry = inspected.entries().at(
            static_cast<qsizetype>(index));
        const QString relativePath = QString::fromUtf8(entry.path);
        const QString outputPath = root.absoluteFilePath(relativePath);
        if (!outputIsWithinRoot(stagingRoot, outputPath)) {
            clearStagingDirectory(stagingRoot);
            return fail(
                ArchiveErrorCode::InvalidEntryPath,
                entry.path,
                QStringLiteral("archive entry path is invalid"));
        }

        const QString parentPath = QFileInfo(outputPath).dir().absolutePath();
        if (!root.mkpath(root.relativeFilePath(parentPath))
            || pathChainContainsReparsePoint(parentPath)
            || pathChainContainsReparsePoint(stagingRoot)) {
            clearStagingDirectory(stagingRoot);
            return fail(
                ArchiveErrorCode::UnsafeStagingRoot,
                entry.path,
                QStringLiteral("staging path is not a regular directory"));
        }

        QSaveFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly)) {
            clearStagingDirectory(stagingRoot);
            return fail(
                ArchiveErrorCode::ExtractionFailed,
                entry.path,
                QStringLiteral("archive entry could not be written"));
        }

        mz_zip_reader_extract_iter_state *state =
            mz_zip_reader_extract_iter_new(reader.get(), index, 0);
        if (state == nullptr) {
            output.cancelWriting();
            clearStagingDirectory(stagingRoot);
            return fail(
                ArchiveErrorCode::ExtractionFailed,
                entry.path,
                QStringLiteral("archive entry could not be read"));
        }

        quint64 written = 0;
        bool writeSucceeded = true;
        while (written < entry.uncompressedSize) {
            const size_t request = static_cast<size_t>(std::min<quint64>(
                entry.uncompressedSize - written,
                static_cast<quint64>(buffer.size())));
            const size_t extracted = mz_zip_reader_extract_iter_read(
                state, buffer.data(), request);
            if (extracted == 0U
                || output.write(
                       buffer.constData(), static_cast<qint64>(extracted))
                    != static_cast<qint64>(extracted)) {
                writeSucceeded = false;
                break;
            }
            written += static_cast<quint64>(extracted);
        }
        if (mz_zip_reader_extract_iter_free(state) != MZ_TRUE) {
            writeSucceeded = false;
        }
        if (!writeSucceeded || written != entry.uncompressedSize
            || !output.commit()) {
            output.cancelWriting();
            clearStagingDirectory(stagingRoot);
            return fail(
                ArchiveErrorCode::ExtractionFailed,
                entry.path,
                QStringLiteral("archive entry extraction failed"));
        }
    }
    return ArchiveResult::success(inspected.entries());
}
