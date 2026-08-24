#include "BrowserCommand.h"

#include <array>

namespace
{
constexpr std::array<BrowserCommand, 20> Commands{
    BrowserCommand::NewTab,
    BrowserCommand::CloseTab,
    BrowserCommand::ReopenClosedTab,
    BrowserCommand::NextTab,
    BrowserCommand::PreviousTab,
    BrowserCommand::SelectTab1,
    BrowserCommand::SelectTab2,
    BrowserCommand::SelectTab3,
    BrowserCommand::SelectTab4,
    BrowserCommand::SelectTab5,
    BrowserCommand::SelectTab6,
    BrowserCommand::SelectTab7,
    BrowserCommand::SelectTab8,
    BrowserCommand::SelectLastTab,
    BrowserCommand::FocusAddress,
    BrowserCommand::Back,
    BrowserCommand::Forward,
    BrowserCommand::Reload,
    BrowserCommand::Stop,
    BrowserCommand::Home,
};

constexpr Qt::KeyboardModifiers ControlShift =
    Qt::ControlModifier | Qt::ShiftModifier;

constexpr std::array<BrowserCommandChord, 20> CommandChords{{
    {BrowserCommand::NewTab,
     QKeyCombination(Qt::ControlModifier, Qt::Key_T)},
    {BrowserCommand::CloseTab,
     QKeyCombination(Qt::ControlModifier, Qt::Key_W)},
    {BrowserCommand::ReopenClosedTab,
     QKeyCombination(ControlShift, Qt::Key_T)},
    {BrowserCommand::NextTab,
     QKeyCombination(Qt::ControlModifier, Qt::Key_Tab)},
    {BrowserCommand::PreviousTab,
     QKeyCombination(ControlShift, Qt::Key_Tab)},
    {BrowserCommand::SelectTab1,
     QKeyCombination(Qt::ControlModifier, Qt::Key_1)},
    {BrowserCommand::SelectTab2,
     QKeyCombination(Qt::ControlModifier, Qt::Key_2)},
    {BrowserCommand::SelectTab3,
     QKeyCombination(Qt::ControlModifier, Qt::Key_3)},
    {BrowserCommand::SelectTab4,
     QKeyCombination(Qt::ControlModifier, Qt::Key_4)},
    {BrowserCommand::SelectTab5,
     QKeyCombination(Qt::ControlModifier, Qt::Key_5)},
    {BrowserCommand::SelectTab6,
     QKeyCombination(Qt::ControlModifier, Qt::Key_6)},
    {BrowserCommand::SelectTab7,
     QKeyCombination(Qt::ControlModifier, Qt::Key_7)},
    {BrowserCommand::SelectTab8,
     QKeyCombination(Qt::ControlModifier, Qt::Key_8)},
    {BrowserCommand::SelectLastTab,
     QKeyCombination(Qt::ControlModifier, Qt::Key_9)},
    {BrowserCommand::FocusAddress,
     QKeyCombination(Qt::ControlModifier, Qt::Key_L)},
    {BrowserCommand::Back,
     QKeyCombination(Qt::AltModifier, Qt::Key_Left)},
    {BrowserCommand::Forward,
     QKeyCombination(Qt::AltModifier, Qt::Key_Right)},
    {BrowserCommand::Reload,
     QKeyCombination(Qt::ControlModifier, Qt::Key_R)},
    {BrowserCommand::Reload,
     QKeyCombination(Qt::NoModifier, Qt::Key_F5)},
    {BrowserCommand::Stop,
     QKeyCombination(Qt::NoModifier, Qt::Key_Escape)},
}};

QString commandSlug(BrowserCommand command)
{
    switch (command) {
    case BrowserCommand::NewTab:
        return QStringLiteral("new-tab");
    case BrowserCommand::CloseTab:
        return QStringLiteral("close-tab");
    case BrowserCommand::ReopenClosedTab:
        return QStringLiteral("reopen-closed-tab");
    case BrowserCommand::NextTab:
        return QStringLiteral("next-tab");
    case BrowserCommand::PreviousTab:
        return QStringLiteral("previous-tab");
    case BrowserCommand::SelectTab1:
        return QStringLiteral("select-tab-1");
    case BrowserCommand::SelectTab2:
        return QStringLiteral("select-tab-2");
    case BrowserCommand::SelectTab3:
        return QStringLiteral("select-tab-3");
    case BrowserCommand::SelectTab4:
        return QStringLiteral("select-tab-4");
    case BrowserCommand::SelectTab5:
        return QStringLiteral("select-tab-5");
    case BrowserCommand::SelectTab6:
        return QStringLiteral("select-tab-6");
    case BrowserCommand::SelectTab7:
        return QStringLiteral("select-tab-7");
    case BrowserCommand::SelectTab8:
        return QStringLiteral("select-tab-8");
    case BrowserCommand::SelectLastTab:
        return QStringLiteral("select-last-tab");
    case BrowserCommand::FocusAddress:
        return QStringLiteral("focus-address");
    case BrowserCommand::Back:
        return QStringLiteral("back");
    case BrowserCommand::Forward:
        return QStringLiteral("forward");
    case BrowserCommand::Reload:
        return QStringLiteral("reload");
    case BrowserCommand::Stop:
        return QStringLiteral("stop");
    case BrowserCommand::Home:
        return QStringLiteral("home");
    }
    return {};
}
}

QList<BrowserCommand> browserCommands()
{
    QList<BrowserCommand> commands;
    commands.reserve(static_cast<qsizetype>(Commands.size()));
    for (BrowserCommand command : Commands) commands.append(command);
    return commands;
}

QList<BrowserCommandChord> browserCommandChords()
{
    QList<BrowserCommandChord> chords;
    chords.reserve(static_cast<qsizetype>(CommandChords.size()));
    for (const BrowserCommandChord &chord : CommandChords) {
        chords.append(chord);
    }
    return chords;
}

QList<QKeySequence> browserCommandShortcuts(BrowserCommand command)
{
    QList<QKeySequence> shortcuts;
    for (const BrowserCommandChord &mapping : CommandChords) {
        if (mapping.command == command) {
            shortcuts.append(QKeySequence(mapping.keyCombination));
        }
    }
    return shortcuts;
}

std::optional<BrowserCommand> browserCommandForKeyCombination(
    QKeyCombination keyCombination)
{
    for (const BrowserCommandChord &mapping : CommandChords) {
        if (mapping.keyCombination == keyCombination) return mapping.command;
    }
    return std::nullopt;
}

std::optional<BrowserCommand> browserCommandForShortcut(
    const QKeySequence &shortcut)
{
    if (shortcut.count() != 1) return std::nullopt;
    return browserCommandForKeyCombination(shortcut[0]);
}

QString browserCommandText(BrowserCommand command)
{
    switch (command) {
    case BrowserCommand::NewTab:
        return QStringLiteral("New tab");
    case BrowserCommand::CloseTab:
        return QStringLiteral("Close tab");
    case BrowserCommand::ReopenClosedTab:
        return QStringLiteral("Reopen closed tab");
    case BrowserCommand::NextTab:
        return QStringLiteral("Next tab");
    case BrowserCommand::PreviousTab:
        return QStringLiteral("Previous tab");
    case BrowserCommand::SelectTab1:
        return QStringLiteral("Select tab 1");
    case BrowserCommand::SelectTab2:
        return QStringLiteral("Select tab 2");
    case BrowserCommand::SelectTab3:
        return QStringLiteral("Select tab 3");
    case BrowserCommand::SelectTab4:
        return QStringLiteral("Select tab 4");
    case BrowserCommand::SelectTab5:
        return QStringLiteral("Select tab 5");
    case BrowserCommand::SelectTab6:
        return QStringLiteral("Select tab 6");
    case BrowserCommand::SelectTab7:
        return QStringLiteral("Select tab 7");
    case BrowserCommand::SelectTab8:
        return QStringLiteral("Select tab 8");
    case BrowserCommand::SelectLastTab:
        return QStringLiteral("Select last tab");
    case BrowserCommand::FocusAddress:
        return QStringLiteral("Focus address");
    case BrowserCommand::Back:
        return QStringLiteral("Back");
    case BrowserCommand::Forward:
        return QStringLiteral("Forward");
    case BrowserCommand::Reload:
        return QStringLiteral("Reload");
    case BrowserCommand::Stop:
        return QStringLiteral("Stop");
    case BrowserCommand::Home:
        return QStringLiteral("Home");
    }
    return {};
}

QString browserCommandObjectName(BrowserCommand command)
{
    const QString slug = commandSlug(command);
    return slug.isEmpty() ? QString()
                          : QStringLiteral("browser-command-") + slug;
}
