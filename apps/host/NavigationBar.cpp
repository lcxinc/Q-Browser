#include "NavigationBar.h"

#include "BrowserTabModel.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVariant>

#include <optional>

namespace
{
std::optional<QString> contentIdentityText(BrowserContentIdentity identity)
{
    switch (identity) {
    case BrowserContentIdentity::QBrowser:
        return QStringLiteral("Q-Browser");
    case BrowserContentIdentity::SignedApplication:
        return QStringLiteral("Signed application");
    case BrowserContentIdentity::RestrictedWeb:
        return QStringLiteral("Restricted web");
    }
    return std::nullopt;
}

std::optional<QString> visualStateText(
    const BrowserTabPresentation &presentation)
{
    switch (presentation.visualState) {
    case BrowserVisualState::Normal:
        return presentation.loading
            ? QStringLiteral("Loading %1%").arg(
                  presentation.progress)
            : QStringLiteral("Ready");
    case BrowserVisualState::Recovering:
        return QStringLiteral("Recovering");
    case BrowserVisualState::Crashed:
        return QStringLiteral("Crashed");
    case BrowserVisualState::TrustedError:
        return QStringLiteral("Error");
    }
    return std::nullopt;
}

void configureButton(QToolButton *button,
                     const QString &objectName,
                     const QString &text,
                     const QString &accessibleName,
                     const QString &accessibleDescription)
{
    button->setObjectName(objectName);
    button->setText(text);
    button->setAccessibleName(accessibleName);
    button->setAccessibleDescription(accessibleDescription);
    button->setToolTip(accessibleDescription);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
}
}

NavigationBar::NavigationBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("navigation-bar"));
    setAccessibleName(QStringLiteral("Navigation toolbar"));
    setAccessibleDescription(
        QStringLiteral("Browser navigation and address controls"));

    backButton_ = new QToolButton(this);
    configureButton(backButton_,
                    QStringLiteral("navigation-back"),
                    QStringLiteral("Back"),
                    QStringLiteral("Back"),
                    QStringLiteral("Go to the previous page"));
    backButton_->setEnabled(false);

    forwardButton_ = new QToolButton(this);
    configureButton(forwardButton_,
                    QStringLiteral("navigation-forward"),
                    QStringLiteral("Forward"),
                    QStringLiteral("Forward"),
                    QStringLiteral("Go to the next page"));
    forwardButton_->setEnabled(false);

    reloadStopButton_ = new QToolButton(this);
    configureButton(reloadStopButton_,
                    QStringLiteral("navigation-reload-stop"),
                    QStringLiteral("Reload"),
                    QStringLiteral("Reload"),
                    QStringLiteral("Reload the active tab"));
    reloadStopButton_->setProperty("loadProgress", 0);
    reloadStopButton_->setEnabled(false);
    reloadStopButton_->hide();

    homeButton_ = new QToolButton(this);
    configureButton(homeButton_,
                    QStringLiteral("navigation-home"),
                    QStringLiteral("Home"),
                    QStringLiteral("Home"),
                    QStringLiteral("Open the Q-Browser home page"));
    homeButton_->setEnabled(false);
    homeButton_->hide();

    contentIdentity_ = new QLabel(this);
    contentIdentity_->setObjectName(
        QStringLiteral("navigation-content-identity"));
    contentIdentity_->setTextFormat(Qt::PlainText);
    contentIdentity_->clear();
    contentIdentity_->setAccessibleName(QStringLiteral("Content identity"));
    contentIdentity_->setAccessibleDescription(
        QStringLiteral("No active content"));
    contentIdentity_->hide();

    address_ = new QLineEdit(this);
    address_->setObjectName(QStringLiteral("navigation-address"));
    address_->setAccessibleName(QStringLiteral("Address"));
    address_->setAccessibleDescription(
        QStringLiteral("Enter a Q-Browser address and press Enter"));
    address_->setPlaceholderText(QStringLiteral("qbrowser:// or app://pilot/"));
    address_->setClearButtonEnabled(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(backButton_);
    layout->addWidget(forwardButton_);
    layout->addWidget(reloadStopButton_);
    layout->addWidget(homeButton_);
    layout->addWidget(contentIdentity_);
    layout->addWidget(address_, 1);

    connect(backButton_, &QToolButton::clicked, this, &NavigationBar::backRequested);
    connect(forwardButton_, &QToolButton::clicked, this, &NavigationBar::forwardRequested);
    connect(homeButton_, &QToolButton::clicked, this, &NavigationBar::homeRequested);
    connect(address_, &QLineEdit::returnPressed, this,
            [this] { emit navigateRequested(address_->text()); });
}

QString NavigationBar::addressText() const
{
    return address_->text();
}

void NavigationBar::setAddressText(const QString &address)
{
    address_->setText(address);
}

void NavigationBar::setNavigationAvailability(const bool canGoBack,
                                               const bool canGoForward)
{
    backButton_->setEnabled(canGoBack);
    forwardButton_->setEnabled(canGoForward);
}

void NavigationBar::setActivePresentation(
    const BrowserTabPresentation &presentation,
    bool canGoBack,
    bool canGoForward)
{
    const std::optional<QString> identity =
        contentIdentityText(presentation.contentIdentity);
    const std::optional<QString> state = visualStateText(presentation);
    if (!actionsBound_ || !identity.has_value() || !state.has_value()
        || presentation.progress < 0 || presentation.progress > 100) {
        clearActivePresentation();
        return;
    }

    hasActivePresentation_ = true;
    setNavigationAvailability(canGoBack, canGoForward);
    loading_ = presentation.loading;
    loadProgress_ = presentation.progress;
    applyReloadStopAction();

    reloadStopButton_->show();
    homeButton_->show();
    homeButton_->setEnabled(true);
    contentIdentity_->show();
    contentIdentity_->setText(*identity);
    contentIdentity_->setAccessibleDescription(
        QStringLiteral("Active content: %1, %2")
            .arg(*identity, *state));
    contentIdentity_->setProperty(
        "visualState", static_cast<int>(presentation.visualState));
}

void NavigationBar::clearActivePresentation()
{
    hasActivePresentation_ = false;
    setNavigationAvailability(false, false);
    loading_ = false;
    loadProgress_ = 0;
    applyReloadStopAction();
    reloadStopButton_->setEnabled(false);
    reloadStopButton_->hide();
    homeButton_->setEnabled(false);
    homeButton_->hide();
    contentIdentity_->clear();
    contentIdentity_->setAccessibleDescription(
        QStringLiteral("No active content"));
    contentIdentity_->setProperty("visualState", QVariant());
    contentIdentity_->hide();
    setAddressText(QString());
}

void NavigationBar::setReloadStopActions(QAction *reloadAction,
                                         QAction *stopAction)
{
    Q_ASSERT(reloadAction != nullptr);
    Q_ASSERT(stopAction != nullptr);
    reloadAction_ = reloadAction;
    stopAction_ = stopAction;
    actionsBound_ = true;
    applyReloadStopAction();
}

void NavigationBar::focusAddressAndSelectAll()
{
    address_->setFocus(Qt::ShortcutFocusReason);
    address_->selectAll();
}

void NavigationBar::applyReloadStopAction()
{
    QAction *const activeAction = loading_ ? stopAction_ : reloadAction_;
    if (activeAction != nullptr
        && reloadStopButton_->defaultAction() != activeAction) {
        const QList<QAction *> existingActions = reloadStopButton_->actions();
        for (QAction *const action : existingActions) {
            if (action != activeAction) reloadStopButton_->removeAction(action);
        }
        reloadStopButton_->setDefaultAction(activeAction);
    }

    reloadStopButton_->setObjectName(
        QStringLiteral("navigation-reload-stop"));
    reloadStopButton_->setProperty("loadProgress", loadProgress_);
    if (loading_) {
        reloadStopButton_->setText(QStringLiteral("Stop"));
        reloadStopButton_->setAccessibleName(QStringLiteral("Stop"));
        reloadStopButton_->setAccessibleDescription(
            QStringLiteral("Stop loading the active tab (%1%)")
                .arg(loadProgress_));
    } else {
        reloadStopButton_->setText(QStringLiteral("Reload"));
        reloadStopButton_->setAccessibleName(QStringLiteral("Reload"));
        reloadStopButton_->setAccessibleDescription(
            QStringLiteral("Reload the active tab"));
    }
    reloadStopButton_->setToolTip(
        reloadStopButton_->accessibleDescription());
    if (!hasActivePresentation_) reloadStopButton_->setEnabled(false);
}
