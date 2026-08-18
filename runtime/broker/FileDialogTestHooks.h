#pragma once

#ifdef Q_BROWSER_BROKER_TESTING

#include <QString>

#include <functional>

namespace qbrowser_broker_testing
{
struct FileDialogTestHooks final
{
    std::function<QString()> selectedPath;
    std::function<void(const QString &)> afterNativeHandleOpened;
    bool cancelAfterRejectedSelection = false;
};

void setFileDialogTestHooks(FileDialogTestHooks hooks);
void resetFileDialogTestHooks();
[[nodiscard]] const FileDialogTestHooks &fileDialogTestHooks();
}

#endif
