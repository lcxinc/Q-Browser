#pragma once

#include "WorkerSupervisor.h"

#include <QWidget>

#include <qt_windows.h>

class QWindow;

class WorkerSurface final : public QWidget
{
    Q_OBJECT

public:
    static WorkerSurface *create(const QString &windowHandle,
                                 HANDLE workerProcess,
                                 WorkerAttemptId attemptId,
                                 QWidget *parent = nullptr);
    ~WorkerSurface() override;

    bool isValid();
    WId nativeWindowId() const noexcept;
    WorkerAttemptId attemptId() const noexcept;

protected:
    void focusInEvent(QFocusEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    WorkerSurface(WId windowId,
                  QWindow *foreignWindow,
                  HANDLE stableProcess,
                  DWORD processId,
                  DWORD guiThreadId,
                  WorkerAttemptId attemptId,
                  QWidget *parent);
    bool refreshValidity();
    void invalidate();

    WId windowId_ = 0;
    QWindow *foreignWindow_ = nullptr;
    QWidget *container_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    DWORD guiThreadId_ = 0;
    WorkerAttemptId attemptId_;
    bool invalidated_ = false;
};
