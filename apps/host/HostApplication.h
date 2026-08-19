#pragma once

#include <QObject>
#include <QUrl>

#include <memory>

class MainWindow;
class HostWorkerSessionController;
class IpcSession;

class HostApplication final : public QObject
{
    Q_OBJECT

public:
    explicit HostApplication(QUrl mockOrigin, QObject *parent = nullptr);
    ~HostApplication() override;

    [[nodiscard]] bool start();
    [[nodiscard]] bool attachWorkerSession(std::unique_ptr<IpcSession> session);
    [[nodiscard]] MainWindow *mainWindow() const noexcept;
    [[nodiscard]] HostWorkerSessionController *workerSessionController() const noexcept;

private:
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<HostWorkerSessionController> workerSessionController_;
};
