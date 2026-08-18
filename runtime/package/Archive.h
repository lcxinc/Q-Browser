#pragma once

#include "ArchiveLimits.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

enum class ArchiveErrorCode
{
    None,
    SourceUnavailable,
    DestinationUnavailable,
    InvalidArchive,
    NonCanonicalArchive,
    ArchiveSizeLimit,
    InvalidEntryPath,
    DuplicateEntryPath,
    UnsupportedEntry,
    EntryCountLimit,
    EntrySizeLimit,
    TotalSizeLimit,
    CompressionRatioLimit,
    ExtractionFailed,
    UnsafeStagingRoot,
};

struct ArchiveError final
{
    ArchiveErrorCode code = ArchiveErrorCode::None;
    QByteArray path;
    QString message;
};

struct ArchiveEntry final
{
    QByteArray path;
    quint64 uncompressedSize = 0;
    quint64 compressedSize = 0;

    [[nodiscard]] bool isIncludedInContentDigest() const noexcept;
};

struct ArchiveFile final
{
    QByteArray path;
    QByteArray contents;

    friend bool operator==(const ArchiveFile &, const ArchiveFile &) = default;
};

namespace qbrowser_archive_detail
{
class ArchiveResultFactory;
}

class ArchiveResult final
{
public:
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const QVector<ArchiveEntry> &entries() const noexcept;
    [[nodiscard]] QVector<ArchiveEntry> contentDigestEntries() const;
    [[nodiscard]] const ArchiveError &error() const noexcept;

private:
    friend class qbrowser_archive_detail::ArchiveResultFactory;

    explicit ArchiveResult(QVector<ArchiveEntry> entries);
    explicit ArchiveResult(ArchiveError error);

    std::optional<QVector<ArchiveEntry>> m_entries;
    ArchiveError m_error;
};

class ArchiveSnapshotResult final
{
public:
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const QVector<ArchiveFile> &files() const noexcept;
    [[nodiscard]] const ArchiveError &error() const noexcept;

private:
    friend class Archive;

    explicit ArchiveSnapshotResult(QVector<ArchiveFile> files);
    explicit ArchiveSnapshotResult(ArchiveError error);

    std::optional<QVector<ArchiveFile>> m_files;
    ArchiveError m_error;
};

class Archive final
{
public:
    // The source is fully preflighted before archivePath is opened.
    [[nodiscard]] static ArchiveResult create(
        const QString &sourceRoot,
        const QString &archivePath,
        const ArchiveLimits &limits = {});
    [[nodiscard]] static ArchiveResult createFromFiles(
        const QVector<ArchiveFile> &files,
        const QString &archivePath,
        const ArchiveLimits &limits = {});
    [[nodiscard]] static ArchiveResult inspect(
        const QString &archivePath,
        const ArchiveLimits &limits = {});
    // Reads, validates, and materializes entries from one immutable in-memory
    // image so callers cannot observe different package versions.
    [[nodiscard]] static ArchiveSnapshotResult snapshot(
        const QString &archivePath,
        const ArchiveLimits &limits = {});
    // stagingRoot must already exist as an empty, non-reparse directory.
    // Any write-phase failure removes all output created by this operation.
    [[nodiscard]] static ArchiveResult extract(
        const QString &archivePath,
        const QString &stagingRoot,
        const ArchiveLimits &limits = {});
};
