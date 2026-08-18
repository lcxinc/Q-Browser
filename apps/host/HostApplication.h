#pragma once

#include <QObject>
#include <QUrl>

#include <memory>

class MainWindow;

class HostApplication final : public QObject
{
    Q_OBJECT

public:
    explicit HostApplication(QUrl mockOrigin, QObject *parent = nullptr);
    ~HostApplication() override;

    [[nodiscard]] bool start();
    [[nodiscard]] MainWindow *mainWindow() const noexcept;

private:
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
};
