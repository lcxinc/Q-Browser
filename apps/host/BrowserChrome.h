#pragma once

#include "BrowserCommand.h"
#include "BrowserTabModel.h"

#include <QVector>
#include <QWidget>

#include <optional>

class QAction;
class NavigationBar;
class QTabBar;
class QToolButton;

class BrowserChrome final : public QWidget
{
    Q_OBJECT

public:
    explicit BrowserChrome(QWidget *parent = nullptr);

    [[nodiscard]] bool synchronizeTabs(const BrowserTabModel &model);

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

    struct OwnedTabPresentation final
    {
        BrowserTabSnapshot snapshot;
        BrowserTabPresentation presentation;
        BrowserTabAccessiblePresentation accessiblePresentation;
        QString displayTitle;
        QString accessibleTabName;

        friend bool operator==(const OwnedTabPresentation &,
                               const OwnedTabPresentation &) = default;
    };

    struct PresentationBatch final
    {
        QVector<OwnedTabPresentation> tabs;
        QString activeTabId;
        int tabCount = 0;
        int activeIndex = -1;

        friend bool operator==(const PresentationBatch &,
                               const PresentationBatch &) = default;
    };

    void createActions();
    void triggerAction(BrowserCommand command);
    [[nodiscard]] std::optional<PresentationBatch> batchFromModel(
        const BrowserTabModel &model) const;
    void applyBatch(const PresentationBatch &batch);
    void applyBatchWithObserverSignalsBlocked(
        const PresentationBatch &batch);
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
    QString selectedTabId_;
    bool synchronizationInProgress_ = false;
    std::optional<PresentationBatch> pendingBatch_;
};
