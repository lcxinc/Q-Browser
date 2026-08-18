#include "WorkerSurface.h"

#include <QFocusEvent>
#include <QResizeEvent>
#include <QWindow>

#include <limits>

WorkerSurface *WorkerSurface::create(const QString &windowHandle,
                                     const DWORD expectedProcessId,
                                     QWidget *parent)
{
    bool converted = false;
    const qulonglong raw = windowHandle.toULongLong(&converted, 10);
    if (!converted || raw == 0 || QString::number(raw) != windowHandle
        || raw > std::numeric_limits<quintptr>::max()) {
        return nullptr;
    }
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(raw));
    DWORD ownerProcessId = 0;
    if (!IsWindow(hwnd)
        || GetWindowThreadProcessId(hwnd, &ownerProcessId) == 0
        || ownerProcessId != expectedProcessId) {
        return nullptr;
    }
    QWindow *foreign = QWindow::fromWinId(static_cast<WId>(raw));
    if (foreign == nullptr) {
        return nullptr;
    }
    return new WorkerSurface(static_cast<WId>(raw), foreign, parent);
}

WorkerSurface::WorkerSurface(const WId windowId,
                             QWindow *foreignWindow,
                             QWidget *parent)
    : QWidget(parent), windowId_(windowId), foreignWindow_(foreignWindow)
{
    setFocusPolicy(Qt::StrongFocus);
    container_ = QWidget::createWindowContainer(foreignWindow_, this);
    container_->setFocusPolicy(Qt::StrongFocus);
    container_->setGeometry(rect());
    container_->show();
}

WorkerSurface::~WorkerSurface()
{
    // The foreign wrapper is owned by the window container. Destroying it
    // must never close or terminate the Worker-owned native window.
    foreignWindow_ = nullptr;
    container_ = nullptr;
}

bool WorkerSurface::isValid() const noexcept
{
    return windowId_ != 0 && foreignWindow_ != nullptr && container_ != nullptr
        && IsWindow(reinterpret_cast<HWND>(windowId_));
}

WId WorkerSurface::nativeWindowId() const noexcept
{
    return windowId_;
}

bool WorkerSurface::containerHasFocus() const noexcept
{
    return container_ != nullptr && container_->hasFocus();
}

void WorkerSurface::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (container_ != nullptr) {
        container_->setFocus(event->reason());
    }
    if (foreignWindow_ != nullptr) {
        foreignWindow_->requestActivate();
    }
}

void WorkerSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (container_ != nullptr) {
        container_->setGeometry(rect());
    }
}
