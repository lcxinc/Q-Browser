#pragma once

#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class QToolButton;
struct BrowserTabPresentation;

class NavigationBar final : public QWidget
{
    Q_OBJECT

public:
    explicit NavigationBar(QWidget *parent = nullptr);

    [[nodiscard]] QString addressText() const;
    void setAddressText(const QString &address);
    void setNavigationAvailability(bool canGoBack, bool canGoForward);
    void setActivePresentation(const BrowserTabPresentation &presentation,
                               bool canGoBack,
                               bool canGoForward);
    void clearActivePresentation();
    void setReloadStopActions(QAction *reloadAction, QAction *stopAction);
    void focusAddressAndSelectAll();

signals:
    void navigateRequested(const QString &address);
    void backRequested();
    void forwardRequested();
    void homeRequested();

private:
    void applyReloadStopAction();

    QToolButton *backButton_ = nullptr;
    QToolButton *forwardButton_ = nullptr;
    QToolButton *reloadStopButton_ = nullptr;
    QToolButton *homeButton_ = nullptr;
    QLabel *contentIdentity_ = nullptr;
    QLineEdit *address_ = nullptr;
    QAction *reloadAction_ = nullptr;
    QAction *stopAction_ = nullptr;
    bool loading_ = false;
    int loadProgress_ = 0;
};
