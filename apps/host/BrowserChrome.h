#pragma once

#include "BrowserCommand.h"
#include "BrowserTabModel.h"

#include <QVector>
#include <QWidget>

class QAction;
class NavigationBar;
class QTabBar;
class QToolButton;

class BrowserChrome final : public QWidget
{
    Q_OBJECT

public:
    explicit BrowserChrome(QWidget *parent = nullptr);

    [[nodiscard]] bool synchronizeTabs(
        const QVector<BrowserTabSnapshot> &snapshots,
        const QVector<BrowserTabPresentation> &presentations,
        const QString &activeTabId);

    [[nodiscard]] QTabBar *tabBar() const noexcept;
    [[nodiscard]] NavigationBar *navigationBar() const noexcept;
    [[nodiscard]] QAction *actionForCommand(BrowserCommand command) const noexcept;

public slots:
    void dispatchCommand(BrowserCommand command);

signals:
    void commandRequested(BrowserCommand command);
    void addressSubmitted(const QString &address);
    void tabActivationRequested(const QString &tabId);
    void tabMoveRequested(const QString &tabId, int from, int to);
    void tabCloseRequested(const QString &tabId);

private:
    struct CommandAction final
    {
        BrowserCommand command;
        QAction *action = nullptr;
    };

    void createActions();
    void triggerAction(BrowserCommand command);
    void updateActionAvailability(bool hasActiveTab,
                                  int tabCount,
                                  bool canGoBack,
                                  bool canGoForward,
                                  bool loading);
    [[nodiscard]] int tabIndexForId(const QString &tabId) const noexcept;

    QTabBar *tabBar_ = nullptr;
    QToolButton *newTabButton_ = nullptr;
    NavigationBar *navigationBar_ = nullptr;
    QVector<CommandAction> commandActions_;
};
