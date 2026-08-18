#pragma once

#include <QQmlNetworkAccessManagerFactory>
#include <QQuickView>

#include <memory>

class RuntimeFacade;

class WorkerWindow final
{
public:
    WorkerWindow();
    ~WorkerWindow();

    bool load(const QString &packageDirectory,
              const QString &entryPoint,
              RuntimeFacade *runtimeFacade);
    WId windowId() const noexcept;
    QString errorString() const;

private:
    std::unique_ptr<QQmlNetworkAccessManagerFactory> networkFactory_;
    QQuickView view_;
    QString errorString_;
};
