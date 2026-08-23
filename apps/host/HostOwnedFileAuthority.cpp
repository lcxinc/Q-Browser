#include "HostOwnedFileAuthority.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <vector>
#endif

std::shared_ptr<const HostOwnedFileAuthority> HostOwnedFileAuthority::open(
    const QString &path)
{
#ifndef Q_OS_WIN
    Q_UNUSED(path);
    return {};
#else
    const QFileInfo supplied(path);
    if (path.isEmpty() || !supplied.isAbsolute() || !supplied.isFile()
        || supplied.isSymLink()) {
        return {};
    }
    const QString canonical = supplied.canonicalFilePath();
    const QString parent = QDir(supplied.absolutePath()).canonicalPath();
    if (canonical.isEmpty() || parent.isEmpty()) return {};
    auto authority = std::shared_ptr<HostOwnedFileAuthority>(
        new HostOwnedFileAuthority);
    authority->canonicalPath_ = QDir::cleanPath(canonical);
    authority->parentCanonicalPath_ = QDir::cleanPath(parent);
    if (!authority->parentTree_.openRoot(supplied.absolutePath())
        || !authority->parentTree_.rootHasRestrictedTrustAcl()
        || !authority->parentTree_.isSameRootIdentityAt(
            authority->parentCanonicalPath_)
        || !authority->file_.openReadLocked(path, authority->parentTree_)
        || !authority->file_.hasRestrictedTrustAcl()
        || !authority->file_.hasSingleLink()
        || !authority->file_.isSameIdentityAt(authority->canonicalPath_)
        || !authority->file_.isStableWithin(authority->parentTree_)) {
        return {};
    }
    return authority;
#endif
}

std::shared_ptr<const HostOwnedFileAuthority>
HostOwnedFileAuthority::openCurrentProcessExecutable()
{
#ifdef Q_OS_WIN
    std::vector<wchar_t> buffer(32U * 1024U);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(
            GetCurrentProcess(), 0U, buffer.data(), &length) == FALSE
        || length == 0U || length >= buffer.size()) {
        return {};
    }
    return open(QString::fromWCharArray(
        buffer.data(), static_cast<qsizetype>(length)));
#else
    return {};
#endif
}

const QString &HostOwnedFileAuthority::canonicalPath() const noexcept
{
    return canonicalPath_;
}

const QString &HostOwnedFileAuthority::parentCanonicalPath() const noexcept
{
    return parentCanonicalPath_;
}

bool HostOwnedFileAuthority::readBounded(
    const quint64 maximum,
    QByteArray &bytes) const
{
#ifdef Q_OS_WIN
    return file_.readBounded(maximum, bytes);
#else
    Q_UNUSED(maximum);
    bytes.clear();
    return false;
#endif
}

bool HostOwnedFileAuthority::revalidate() const
{
#ifndef Q_OS_WIN
    return false;
#else
    const QFileInfo current(canonicalPath_);
    if (!current.isFile() || current.isSymLink()
        || current.canonicalFilePath().compare(canonicalPath_,
                                               Qt::CaseInsensitive) != 0) {
        return false;
    }
    return parentTree_.isStable()
        && parentTree_.rootHasRestrictedTrustAcl()
        && parentTree_.isSameRootIdentityAt(parentCanonicalPath_)
        && file_.hasRestrictedTrustAcl() && file_.hasSingleLink()
        && file_.isSameIdentityAt(canonicalPath_)
        && file_.isStableWithin(parentTree_);
#endif
}
