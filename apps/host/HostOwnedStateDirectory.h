#pragma once

#include <QString>
#include <QStringList>

#include <memory>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#endif

class HostOwnedStateDirectory final
{
public:
    [[nodiscard]] static std::shared_ptr<const HostOwnedStateDirectory> open(
        const QString &path,
        const QStringList &disjointFrom = {});

    HostOwnedStateDirectory(const HostOwnedStateDirectory &) = delete;
    HostOwnedStateDirectory &operator=(const HostOwnedStateDirectory &) = delete;
    HostOwnedStateDirectory(HostOwnedStateDirectory &&) noexcept = default;
    HostOwnedStateDirectory &operator=(HostOwnedStateDirectory &&) noexcept = default;

    [[nodiscard]] const QString &canonicalPath() const noexcept;
    [[nodiscard]] bool revalidate() const;

private:
    HostOwnedStateDirectory() = default;

    QString canonicalPath_;
    QStringList disjointPaths_;
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree tree_;
#endif
};
