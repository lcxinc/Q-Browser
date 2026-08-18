#include "PosixStableIo.h"

#ifndef Q_OS_WIN

#include <QDir>
#include <QFile>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#endif

namespace qbrowser_archive_detail
{
UniquePosixFd::UniquePosixFd(int fd) noexcept
    : m_fd(fd)
{
}

UniquePosixFd::~UniquePosixFd()
{
    reset();
}

UniquePosixFd::UniquePosixFd(UniquePosixFd &&other) noexcept
    : m_fd(other.m_fd)
{
    other.m_fd = -1;
}

UniquePosixFd &UniquePosixFd::operator=(UniquePosixFd &&other) noexcept
{
    if (this != &other) {
        reset(other.m_fd);
        other.m_fd = -1;
    }
    return *this;
}

int UniquePosixFd::get() const noexcept
{
    return m_fd;
}

bool UniquePosixFd::isValid() const noexcept
{
    return m_fd >= 0;
}

void UniquePosixFd::reset(int fd) noexcept
{
    if (m_fd >= 0) {
        (void)::close(m_fd);
    }
    m_fd = fd;
}

bool PosixStableDirectory::openAbsolute(const QString &absoluteDirectory)
{
    m_fd.reset();
    const QString normalized = QDir::fromNativeSeparators(absoluteDirectory);
    if (!QDir::isAbsolutePath(normalized)
        || QDir::cleanPath(normalized) != normalized) {
        return false;
    }

    UniquePosixFd current(
        ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current.isValid()) {
        return false;
    }
    const QStringList components = normalized.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QLatin1String(".")
            || component == QLatin1String("..")) {
            return false;
        }
        const QByteArray encoded = QFile::encodeName(component);
        const int next = ::openat(
            current.get(),
            encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            return false;
        }
        current.reset(next);
    }
    m_fd = std::move(current);
    return true;
}

bool PosixStableDirectory::isOpen() const noexcept
{
    return m_fd.isValid();
}

int PosixStableDirectory::fd() const noexcept
{
    return m_fd.get();
}

bool PosixStableDirectory::sync() const noexcept
{
    return m_fd.isValid() && ::fsync(m_fd.get()) == 0;
}

bool PosixOwnedOutput::create(
    const PosixStableDirectory &directory,
    mode_t mode)
{
    m_fd.reset();
    m_published = false;
    if (!directory.isOpen()) {
        return false;
    }
#if defined(__linux__)
    const int fd = ::openat(
        directory.fd(),
        ".",
        O_WRONLY | O_CLOEXEC | O_TMPFILE,
        mode);
    if (fd < 0) {
        return false;
    }
    m_fd.reset(fd);
    return true;
#else
    Q_UNUSED(mode);
    return false;
#endif
}

bool PosixOwnedOutput::writeAll(const char *bytes, size_t size)
{
    if (!m_fd.isValid() || bytes == nullptr) {
        return size == 0U && m_fd.isValid();
    }
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(m_fd.get(), bytes + offset, size - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool PosixOwnedOutput::flush() const noexcept
{
    return m_fd.isValid() && ::fsync(m_fd.get()) == 0;
}

bool PosixOwnedOutput::setModeExact(mode_t mode) const noexcept
{
    return m_fd.isValid() && ::fchmod(m_fd.get(), mode) == 0
        && modeIs(mode);
}

bool PosixOwnedOutput::modeIs(mode_t mode) const noexcept
{
    struct stat metadata{};
    return m_fd.isValid() && ::fstat(m_fd.get(), &metadata) == 0
        && (metadata.st_mode & static_cast<mode_t>(0777)) == mode;
}

bool PosixOwnedOutput::publishNoReplace(
    const QByteArray &destinationName,
    const PosixStableDirectory &directory)
{
    if (!m_fd.isValid() || m_published || !directory.isOpen()
        || destinationName.isEmpty() || destinationName.contains('/')) {
        return false;
    }
#if defined(__linux__)
    int result = ::linkat(
        m_fd.get(),
        "",
        directory.fd(),
        destinationName.constData(),
        AT_EMPTY_PATH);
    if (result != 0 && (errno == EPERM || errno == ENOENT)) {
        const QByteArray descriptorPath = QByteArrayLiteral("/proc/self/fd/")
            + QByteArray::number(m_fd.get());
        result = ::linkat(
            AT_FDCWD,
            descriptorPath.constData(),
            directory.fd(),
            destinationName.constData(),
            AT_SYMLINK_FOLLOW);
    }
    if (result != 0) {
        return false;
    }
    m_published = true;
    return directory.sync();
#else
    Q_UNUSED(destinationName);
    Q_UNUSED(directory);
    return false;
#endif
}
}

#endif
