#pragma once

#include "IpcSession.h"

#include <QObject>
#include <QUrl>

#include <memory>
#include <functional>

class MainWindow;
class HostWorkerSessionController;
class WorkerSurface;

struct HostWorkerAttachContext final
{
    std::unique_ptr<IpcSession> session;
    WorkerSurface *surface = nullptr;
    std::shared_ptr<void> processLifetime;
    std::function<void()> stopProcess;
};

class HostApplication final : public QObject
{
    Q_OBJECT

public:
    explicit HostApplication(QUrl mockOrigin, QObject *parent = nullptr);
    ~HostApplication() override;

    [[nodiscard]] bool start();
    [[nodiscard]] bool attachWorkerSession(std::unique_ptr<IpcSession> session);
    [[nodiscard]] bool attachWorkerContext(HostWorkerAttachContext context);
    void detachWorkerContext(const QString &reason);
    [[nodiscard]] bool hasWorkerContext() const noexcept;
    [[nodiscard]] MainWindow *mainWindow() const noexcept;
    [[nodiscard]] HostWorkerSessionController *workerSessionController() const noexcept;

private:
    QUrl mockOrigin_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<HostWorkerSessionController> workerSessionController_;
    std::shared_ptr<void> workerProcessLifetime_;
    std::function<void()> stopWorkerProcess_;
};
