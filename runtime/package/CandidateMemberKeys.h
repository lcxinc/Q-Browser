#pragma once

#include <QDir>
#include <QSet>
#include <QString>

namespace qbrowser_package_detail
{
inline QString canonicalCandidatePath(const QString &relativePath)
{
    return QDir::fromNativeSeparators(relativePath)
        .normalized(QString::NormalizationForm_C)
        .toCaseFolded();
}

inline bool insertCanonicalCandidatePath(
    QSet<QString> &paths,
    const QString &relativePath)
{
    const QString key = canonicalCandidatePath(relativePath);
    if (paths.contains(key)) {
        return false;
    }
    paths.insert(key);
    return true;
}
}
