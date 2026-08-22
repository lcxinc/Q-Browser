#include "WorkerSurface.h"

#include <QFocusEvent>
#include <QResizeEvent>
#include <QWindow>

#include <limits>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

} // namespace

WorkerSurface *WorkerSurface::create(const QString &windowHandle,
                                     const HANDLE workerProcess,
                                     const WorkerAttemptId attemptId,
                                     QWidget *parent)
{
    bool converted = false;
    const qulonglong raw = windowHandle.toULongLong(&converted, 10);
    if (!converted || raw == 0 || QString::number(raw) != windowHandle
        || raw > std::numeric_limits<quintptr>::max() || !validHandle(workerProcess)
        || attemptId.value == 0) {
        return nullptr;
    }
    HANDLE stableProcess = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), workerProcess,
                         GetCurrentProcess(), &stableProcess,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                         FALSE, 0)) {
        return nullptr;
    }
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(raw));
    const DWORD processId = GetProcessId(stableProcess);
    DWORD windowProcessId = 0;
    const DWORD guiThreadId = GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (processId == 0 || WaitForSingleObject(stableProcess, 0) != WAIT_TIMEOUT
        || !IsWindow(hwnd) || guiThreadId == 0 || windowProcessId != processId) {
        CloseHandle(stableProcess);
        return nullptr;
    }
    QWindow *foreign = QWindow::fromWinId(static_cast<WId>(raw));
    if (foreign == nullptr) {
        CloseHandle(stableProcess);
        return nullptr;
    }
    return new WorkerSurface(static_cast<WId>(raw), foreign, stableProcess,
                             processId, guiThreadId, attemptId, parent);
}

WorkerSurface::WorkerSurface(const WId windowId,
                             QWindow *foreignWindow,
                             const HANDLE stableProcess,
                             const DWORD processId,
                             const DWORD guiThreadId,
                             const WorkerAttemptId attemptId,
                             QWidget *parent)
    : QWidget(parent), windowId_(windowId), foreignWindow_(foreignWindow),
      process_(stableProcess), processId_(processId), guiThreadId_(guiThreadId),
      attemptId_(attemptId)
{
    setFocusPolicy(Qt::StrongFocus);
    container_ = QWidget::createWindowContainer(foreignWindow_, this);
    container_->setFocusPolicy(Qt::StrongFocus);
    container_->setGeometry(rect());
    container_->show();
}

WorkerSurface::~WorkerSurface()
{
    foreignWindow_ = nullptr;
    container_ = nullptr;
    if (validHandle(process_)) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool WorkerSurface::isValid()
{
    return refreshValidity();
}

bool WorkerSurface::focusNativeWindow()
{
    if (!refreshValidity()) return false;
    if (container_ != nullptr) container_->setFocus(Qt::OtherFocusReason);
    const DWORD hostThreadId = GetCurrentThreadId();
    const bool needsAttach = hostThreadId != guiThreadId_;
    const bool attached = !needsAttach
        || AttachThreadInput(hostThreadId, guiThreadId_, TRUE) != FALSE;
    bool focused = false;
    if (attached) {
        const HWND workerWindow = reinterpret_cast<HWND>(windowId_);
        (void)SetFocus(workerWindow);
        focused = GetFocus() == workerWindow;
    }
    if (attached && needsAttach)
        (void)AttachThreadInput(hostThreadId, guiThreadId_, FALSE);
    return focused;
}

WId WorkerSurface::nativeWindowId() const noexcept { return windowId_; }
WorkerAttemptId WorkerSurface::attemptId() const noexcept { return attemptId_; }

bool WorkerSurface::refreshValidity()
{
    if (invalidated_ || !validHandle(process_)
        || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT
        || GetProcessId(process_) != processId_) {
        invalidate();
        return false;
    }
    const HWND hwnd = reinterpret_cast<HWND>(windowId_);
    DWORD currentProcessId = 0;
    const DWORD currentThreadId = GetWindowThreadProcessId(hwnd, &currentProcessId);
    if (!IsWindow(hwnd) || currentProcessId != processId_
        || currentThreadId != guiThreadId_) {
        invalidate();
        return false;
    }
    return true;
}

void WorkerSurface::invalidate()
{
    if (invalidated_) return;
    invalidated_ = true;
    windowId_ = 0;
    if (container_ != nullptr) container_->hide();
    setEnabled(false);
}

void WorkerSurface::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    (void)focusNativeWindow();
}

void WorkerSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (refreshValidity() && container_ != nullptr) {
        container_->setGeometry(rect());
    }
}
