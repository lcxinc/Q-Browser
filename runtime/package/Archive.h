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

class ArchiveResult final
{
public:
    [[nodiscard]] static ArchiveResult success(QVector<ArchiveEntry> entries);
    [[nodiscard]] static ArchiveResult failure(ArchiveError error);

    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const QVector<ArchiveEntry> &entries() const noexcept;
    [[nodiscard]] QVector<ArchiveEntry> contentDigestEntries() const;
    [[nodiscard]] const ArchiveError &error() const noexcept;

private:
    std::optional<QVector<ArchiveEntry>> m_entries;
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
    [[nodiscard]] static ArchiveResult inspect(
        const QString &archivePath,
        const ArchiveLimits &limits = {});
    // stagingRoot must already exist as an empty, non-reparse directory.
    // Any write-phase failure removes all output created by this operation.
    [[nodiscard]] static ArchiveResult extract(
        const QString &archivePath,
        const QString &stagingRoot,
        const ArchiveLimits &limits = {});
};
