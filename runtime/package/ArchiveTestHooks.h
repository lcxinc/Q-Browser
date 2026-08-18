#pragma once

#ifdef Q_BROWSER_ARCHIVE_TESTING

#include <QByteArray>
#include <QString>
#include <QtTypes>

#include <functional>

namespace qbrowser_archive_testing
{
struct ArchiveTestHooks final
{
    std::function<void(const QString &)> afterArchiveSizeChecked;
    std::function<void(const QString &, const QByteArray &)> beforeSourceRead;
    std::function<void(const QString &)> afterStagingGuardOpened;
    std::function<void(const QString &, const QByteArray &)> afterParentGuardOpened;
    std::function<void(const QString &, const QByteArray &)> afterTemporaryReady;
    std::function<void(const QString &, const QByteArray &)> beforePublish;
    std::function<void(const QString &)> beforeFailureCleanup;
    std::function<void(const QString &)> beforeOwnedDirectoryDelete;
    std::function<void(const QString &, quint32, bool)> afterWindowsHandleOpened;
    std::function<quint32(quint32)> limitWindowsWriteRequest;
    std::function<bool()> allowWindowsFlush;
};

void setArchiveTestHooks(ArchiveTestHooks hooks);
void resetArchiveTestHooks();
[[nodiscard]] const ArchiveTestHooks &archiveTestHooks();
}

#endif
