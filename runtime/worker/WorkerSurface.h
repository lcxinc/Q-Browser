#pragma once

#include <QWidget>

#include <qt_windows.h>

class QWindow;

class WorkerSurface final : public QWidget
{
    Q_OBJECT

public:
    static WorkerSurface *create(const QString &windowHandle,
                                 DWORD expectedProcessId,
                                 QWidget *parent = nullptr);
    ~WorkerSurface() override;

    bool isValid() const noexcept;
    WId nativeWindowId() const noexcept;
    bool containerHasFocus() const noexcept;

protected:
    void focusInEvent(QFocusEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    WorkerSurface(WId windowId, QWindow *foreignWindow, QWidget *parent);

    WId windowId_ = 0;
    QWindow *foreignWindow_ = nullptr;
    QWidget *container_ = nullptr;
};
