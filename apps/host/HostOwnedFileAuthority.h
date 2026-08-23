#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#endif

class HostOwnedFileAuthority final
{
public:
    [[nodiscard]] static std::shared_ptr<const HostOwnedFileAuthority> open(
        const QString &path);
    [[nodiscard]] static std::shared_ptr<const HostOwnedFileAuthority>
    openCurrentProcessExecutable();

    HostOwnedFileAuthority(const HostOwnedFileAuthority &) = delete;
    HostOwnedFileAuthority &operator=(const HostOwnedFileAuthority &) = delete;
    HostOwnedFileAuthority(HostOwnedFileAuthority &&) noexcept = default;
    HostOwnedFileAuthority &operator=(HostOwnedFileAuthority &&) noexcept = default;

    [[nodiscard]] const QString &canonicalPath() const noexcept;
    [[nodiscard]] const QString &parentCanonicalPath() const noexcept;
    [[nodiscard]] bool readBounded(quint64 maximum, QByteArray &bytes) const;
    [[nodiscard]] bool revalidate() const;

private:
    HostOwnedFileAuthority() = default;

    QString canonicalPath_;
    QString parentCanonicalPath_;
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree parentTree_;
    mutable qbrowser_archive_detail::WindowsStableFile file_;
#endif
};
