#include "NavigationBar.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>

NavigationBar::NavigationBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("navigation-bar"));
    backButton_ = new QToolButton(this);
    backButton_->setObjectName(QStringLiteral("navigation-back"));
    backButton_->setText(QStringLiteral("<"));
    backButton_->setToolTip(QStringLiteral("Back"));
    backButton_->setEnabled(false);

    forwardButton_ = new QToolButton(this);
    forwardButton_->setObjectName(QStringLiteral("navigation-forward"));
    forwardButton_->setText(QStringLiteral(">"));
    forwardButton_->setToolTip(QStringLiteral("Forward"));
    forwardButton_->setEnabled(false);

    address_ = new QLineEdit(this);
    address_->setObjectName(QStringLiteral("navigation-address"));
    address_->setPlaceholderText(QStringLiteral("app://pilot/..."));
    address_->setClearButtonEnabled(true);

    goButton_ = new QToolButton(this);
    goButton_->setObjectName(QStringLiteral("navigation-go"));
    goButton_->setText(QStringLiteral("Go"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(backButton_);
    layout->addWidget(forwardButton_);
    layout->addWidget(address_, 1);
    layout->addWidget(goButton_);

    connect(backButton_, &QToolButton::clicked, this, &NavigationBar::backRequested);
    connect(forwardButton_, &QToolButton::clicked, this, &NavigationBar::forwardRequested);
    connect(address_, &QLineEdit::returnPressed, this,
            [this] { emit navigateRequested(address_->text()); });
    connect(goButton_, &QToolButton::clicked, this,
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
