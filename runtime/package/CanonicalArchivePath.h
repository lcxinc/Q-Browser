#pragma once

#include "ArchiveLimits.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace qbrowser_archive_detail
{
struct CheckedArchivePath final
{
    QString collisionKey;
    bool isCanonical = true;
};

struct ArchiveCollisionPath final
{
    QString key;
    QByteArray path;
    bool isCanonical = true;
};

[[nodiscard]] bool archivePathBytewiseLess(
    const QByteArray &left,
    const QByteArray &right);
[[nodiscard]] std::optional<CheckedArchivePath> validateArchivePath(
    const QByteArray &path,
    const ArchiveLimits &limits = {});
[[nodiscard]] std::optional<QByteArray> findArchivePathCollision(
    const QVector<ArchiveCollisionPath> &paths);
[[nodiscard]] std::optional<QByteArray> firstNonCanonicalArchivePath(
    const QVector<ArchiveCollisionPath> &paths);
}
