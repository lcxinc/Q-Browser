#pragma once

#ifdef Q_BROWSER_BROKER_TESTING

#include "FileBroker.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace qbrowser_broker_testing
{
struct FileDialogTestShowResult final
{
    FileDialogStatus status = FileDialogStatus::Failed;
    QString selectedPath;
};

using FileDialogTestShowCompletion =
    std::function<void(FileDialogTestShowResult)>;
using FileDialogTestShowHook =
    std::function<void(qint64, FileDialogTestShowCompletion)>;

struct FileDialogTestHooks final
{
    std::function<QString()> selectedPath;
    std::function<void(const QString &)> afterNativeHandleOpened;
    bool cancelAfterRejectedSelection = false;
    std::function<void()> coordinatorDialogCreated;
    FileDialogTestShowHook coordinatorShow;
    std::function<void(qint64)> coordinatorBeforeRead;
    std::function<void()> coordinatorBeforeEncode;
    std::function<void()> coordinatorCancel;
    std::function<void()> coordinatorBeforeImmediateCompletion;
    std::function<void()> coordinatorOperationQuiesced;
};

void setFileDialogTestHooks(FileDialogTestHooks hooks);
void resetFileDialogTestHooks();
[[nodiscard]] FileDialogTestHooks fileDialogTestHooks();
}

#endif
