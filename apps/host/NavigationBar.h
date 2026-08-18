#pragma once

#include <QWidget>

class QLineEdit;
class QToolButton;

class NavigationBar final : public QWidget
{
    Q_OBJECT

public:
    explicit NavigationBar(QWidget *parent = nullptr);

    [[nodiscard]] QString addressText() const;
    void setAddressText(const QString &address);
    void setNavigationAvailability(bool canGoBack, bool canGoForward);

signals:
    void navigateRequested(const QString &address);
    void backRequested();
    void forwardRequested();

private:
    QToolButton *backButton_ = nullptr;
    QToolButton *forwardButton_ = nullptr;
    QLineEdit *address_ = nullptr;
    QToolButton *goButton_ = nullptr;
};
