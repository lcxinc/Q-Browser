#pragma once

#include <QtTypes>

struct ArchiveLimits final
{
    static constexpr quint64 DefaultMaximumArchiveBytes =
        64ULL * 1024ULL * 1024ULL;
    static constexpr quint64 DefaultMaximumEntries = 512;
    static constexpr quint64 DefaultMaximumEntryBytes =
        50ULL * 1024ULL * 1024ULL;
    static constexpr quint64 DefaultMaximumTotalBytes =
        50ULL * 1024ULL * 1024ULL;
    static constexpr quint64 DefaultMaximumCompressionRatio = 100;
    static constexpr qsizetype DefaultMaximumPathBytes = 512;
    static constexpr qsizetype DefaultMaximumComponentBytes = 255;
    static constexpr qsizetype DefaultMaximumPathUtf16Units = 240;
    static constexpr qsizetype DefaultMaximumComponentUtf16Units = 120;

    quint64 maximumArchiveBytes = DefaultMaximumArchiveBytes;
    quint64 maximumEntries = DefaultMaximumEntries;
    quint64 maximumEntryBytes = DefaultMaximumEntryBytes;
    quint64 maximumTotalBytes = DefaultMaximumTotalBytes;
    quint64 maximumCompressionRatio = DefaultMaximumCompressionRatio;
    qsizetype maximumPathBytes = DefaultMaximumPathBytes;
    qsizetype maximumComponentBytes = DefaultMaximumComponentBytes;
    qsizetype maximumPathUtf16Units = DefaultMaximumPathUtf16Units;
    qsizetype maximumComponentUtf16Units = DefaultMaximumComponentUtf16Units;
};
