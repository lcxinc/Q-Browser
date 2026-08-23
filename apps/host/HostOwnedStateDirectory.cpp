#include "HostOwnedStateDirectory.h"

#include <QDir>
#include <QFileInfo>

#include <optional>
#include <utility>

namespace
{
std::optional<QString> canonicalExistingPath(const QString &path)
{
    const QFileInfo information(path);
    if (path.isEmpty() || !information.isAbsolute()
        || (!information.isDir() && !information.isFile())
        || information.isSymLink()) {
        return std::nullopt;
    }
    const QString canonical = information.isDir()
        ? QDir(path).canonicalPath() : information.canonicalFilePath();
    return canonical.isEmpty()
        ? std::nullopt
        : std::optional<QString>(QDir::cleanPath(canonical));
}

bool overlaps(const QString &left, const QString &right)
{
    const QString nativeLeft = QDir::toNativeSeparators(left);
    const QString nativeRight = QDir::toNativeSeparators(right);
    const QString leftPrefix = nativeLeft + QDir::separator();
    const QString rightPrefix = nativeRight + QDir::separator();
    return nativeLeft.compare(nativeRight, Qt::CaseInsensitive) == 0
        || nativeLeft.startsWith(rightPrefix, Qt::CaseInsensitive)
        || nativeRight.startsWith(leftPrefix, Qt::CaseInsensitive);
}

#ifndef Q_OS_WIN
bool hasSymlinkComponent(const QString &path)
{
    QString current = QFileInfo(path).absoluteFilePath();
    for (;;) {
        if (QFileInfo(current).isSymLink()) return true;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) return false;
        current = parent;
    }
}
#endif
}

std::shared_ptr<const HostOwnedStateDirectory> HostOwnedStateDirectory::open(
    const QString &path,
    const QStringList &disjointFrom)
{
    const QFileInfo supplied(path);
    if (path.isEmpty() || !supplied.isAbsolute() || !supplied.isDir()
        || supplied.isSymLink()) {
        return {};
    }
    auto authority = std::shared_ptr<HostOwnedStateDirectory>(
        new HostOwnedStateDirectory);
#ifdef Q_OS_WIN
    if (!authority->tree_.openRoot(supplied.absoluteFilePath())
        || !authority->tree_.rootHasRestrictedTrustAcl()) {
        return {};
    }
#else
    if (hasSymlinkComponent(supplied.absoluteFilePath())) return {};
#endif
    const auto canonical = canonicalExistingPath(path);
    if (!canonical.has_value()) return {};
    authority->canonicalPath_ = *canonical;
#ifdef Q_OS_WIN
    if (!authority->tree_.isSameRootIdentityAt(authority->canonicalPath_)) {
        return {};
    }
#endif
    for (const QString &candidate : disjointFrom) {
        const auto canonicalCandidate = canonicalExistingPath(candidate);
        if (!canonicalCandidate.has_value()
            || overlaps(authority->canonicalPath_, *canonicalCandidate)) {
            return {};
        }
        authority->disjointPaths_.push_back(*canonicalCandidate);
    }
    return authority;
}

const QString &HostOwnedStateDirectory::canonicalPath() const noexcept
{
    return canonicalPath_;
}

bool HostOwnedStateDirectory::revalidate() const
{
    const auto current = canonicalExistingPath(canonicalPath_);
    if (!current.has_value()
        || current->compare(canonicalPath_, Qt::CaseInsensitive) != 0) {
        return false;
    }
#ifdef Q_OS_WIN
    if (!tree_.isStable() || !tree_.isSameRootIdentityAt(canonicalPath_)
        || !tree_.rootHasRestrictedTrustAcl()) {
        return false;
    }
#else
    if (hasSymlinkComponent(canonicalPath_)) return false;
#endif
    for (const QString &candidate : disjointPaths_) {
        const auto currentCandidate = canonicalExistingPath(candidate);
        if (!currentCandidate.has_value()
            || overlaps(canonicalPath_, *currentCandidate)) {
            return false;
        }
    }
    return true;
}
