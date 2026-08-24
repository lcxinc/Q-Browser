#pragma once

#include <QKeySequence>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

enum class BrowserCommand
{
    NewTab,
    CloseTab,
    ReopenClosedTab,
    NextTab,
    PreviousTab,
    SelectTab1,
    SelectTab2,
    SelectTab3,
    SelectTab4,
    SelectTab5,
    SelectTab6,
    SelectTab7,
    SelectTab8,
    SelectLastTab,
    FocusAddress,
    Back,
    Forward,
    Reload,
    Stop,
    Home,
};

Q_DECLARE_METATYPE(BrowserCommand)

struct BrowserCommandChord final
{
    BrowserCommand command;
    QKeyCombination keyCombination;

    friend bool operator==(const BrowserCommandChord &,
                           const BrowserCommandChord &) = default;
};

[[nodiscard]] QList<BrowserCommand> browserCommands();
[[nodiscard]] QList<BrowserCommandChord> browserCommandChords();
[[nodiscard]] QList<QKeySequence> browserCommandShortcuts(
    BrowserCommand command);
[[nodiscard]] std::optional<BrowserCommand> browserCommandForKeyCombination(
    QKeyCombination keyCombination);
[[nodiscard]] std::optional<BrowserCommand> browserCommandForShortcut(
    const QKeySequence &shortcut);
[[nodiscard]] QString browserCommandText(BrowserCommand command);
[[nodiscard]] QString browserCommandObjectName(BrowserCommand command);
