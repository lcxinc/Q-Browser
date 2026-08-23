#include "HostOwnedStateDirectory.h"

#include <QDir>
#include <QFileInfo>

#include <optional>
#include <utility>

namespace
{
QString relationPathKey(const QString &path)
{
    QString key = QDir::toNativeSeparators(path).toCaseFolded();
    key.replace(u'/', u'\\');
    return key;
}

QString relationRootKey(const QString &path)
{
    QString key = relationPathKey(path);
    const QString volumeRoot = relationPathKey(QDir(path).rootPath());
    while (key.size() > 1 && key.endsWith(u'\\') && key != volumeRoot) {
        key.chop(1);
    }
    return key;
}
}

namespace qbrowser_host_detail
{
bool pathWithinOrEqual(const QString &root, const QString &candidate)
{
    const QString rootKey = relationRootKey(root);
    const QString candidateKey = relationRootKey(candidate);
    if (rootKey.isEmpty() || candidateKey.isEmpty()) return false;
    if (candidateKey == rootKey) return true;
    return rootKey.endsWith(u'\\')
        ? candidateKey.startsWith(rootKey)
        : candidateKey.startsWith(rootKey + u'\\');
}

bool strictPathDescendant(const QString &root, const QString &candidate)
{
    return pathWithinOrEqual(root, candidate)
        && !pathWithinOrEqual(candidate, root);
}

bool pathsOverlap(const QString &left, const QString &right)
{
    return pathWithinOrEqual(left, right) || pathWithinOrEqual(right, left);
}
}

namespace
{
#ifdef Q_OS_WIN
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

#endif
}

std::shared_ptr<const HostOwnedStateDirectory> HostOwnedStateDirectory::open(
    const QString &path,
    const QStringList &disjointFrom)
{
#ifndef Q_OS_WIN
    Q_UNUSED(path);
    Q_UNUSED(disjointFrom);
    return {};
#else
    const QFileInfo supplied(path);
    if (path.isEmpty() || !supplied.isAbsolute() || !supplied.isDir()
        || supplied.isSymLink()) {
        return {};
    }
    auto authority = std::shared_ptr<HostOwnedStateDirectory>(
        new HostOwnedStateDirectory);
    if (!authority->tree_.openRoot(supplied.absoluteFilePath())
        || !authority->tree_.rootHasRestrictedTrustAcl()) {
        return {};
    }
    const auto canonical = canonicalExistingPath(path);
    if (!canonical.has_value()) return {};
    authority->canonicalPath_ = *canonical;
    if (!authority->tree_.isSameRootIdentityAt(authority->canonicalPath_)) {
        return {};
    }
    for (const QString &candidate : disjointFrom) {
        const auto canonicalCandidate = canonicalExistingPath(candidate);
        if (!canonicalCandidate.has_value()
            || qbrowser_host_detail::pathsOverlap(
                authority->canonicalPath_, *canonicalCandidate)) {
            return {};
        }
        authority->disjointPaths_.push_back(*canonicalCandidate);
    }
    return authority;
#endif
}

const QString &HostOwnedStateDirectory::canonicalPath() const noexcept
{
    return canonicalPath_;
}

bool HostOwnedStateDirectory::revalidate() const
{
#ifndef Q_OS_WIN
    return false;
#else
    const auto current = canonicalExistingPath(canonicalPath_);
    if (!current.has_value()
        || current->compare(canonicalPath_, Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (!tree_.isStable() || !tree_.isSameRootIdentityAt(canonicalPath_)
        || !tree_.rootHasRestrictedTrustAcl()) {
        return false;
    }
    for (const QString &candidate : disjointPaths_) {
        const auto currentCandidate = canonicalExistingPath(candidate);
        if (!currentCandidate.has_value()
            || qbrowser_host_detail::pathsOverlap(
                canonicalPath_, *currentCandidate)) {
            return false;
        }
    }
    return true;
#endif
}
