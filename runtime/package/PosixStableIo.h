#pragma once

#include <QtGlobal>

#ifndef Q_OS_WIN

#include <QByteArray>
#include <QString>

#include <sys/types.h>

#include <cstddef>

namespace qbrowser_archive_detail
{
class UniquePosixFd final
{
public:
    UniquePosixFd() = default;
    explicit UniquePosixFd(int fd) noexcept;
    ~UniquePosixFd();

    UniquePosixFd(const UniquePosixFd &) = delete;
    UniquePosixFd &operator=(const UniquePosixFd &) = delete;
    UniquePosixFd(UniquePosixFd &&other) noexcept;
    UniquePosixFd &operator=(UniquePosixFd &&other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    void reset(int fd = -1) noexcept;

private:
    int m_fd = -1;
};

class PosixStableDirectory final
{
public:
    [[nodiscard]] bool openAbsolute(const QString &absoluteDirectory);
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool sync() const noexcept;

private:
    UniquePosixFd m_fd;
};

class PosixOwnedOutput final
{
public:
    [[nodiscard]] bool create(
        const PosixStableDirectory &directory,
        mode_t mode);
    [[nodiscard]] bool writeAll(const char *bytes, size_t size);
    [[nodiscard]] bool flush() const noexcept;
    [[nodiscard]] bool setModeExact(mode_t mode) const noexcept;
    [[nodiscard]] bool modeIs(mode_t mode) const noexcept;
    [[nodiscard]] bool publishNoReplace(
        const QByteArray &destinationName,
        const PosixStableDirectory &directory);

private:
    UniquePosixFd m_fd;
    bool m_published = false;
};
}

#endif
