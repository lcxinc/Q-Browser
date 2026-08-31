#include "AppTabRuntimeController.h"
#include "BrowserTabModel.h"
#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "HostWorkerSessionController.h"
#include "MainWindow.h"
#include "TabController.h"
#include "TestEnvironment.h"
#include "WorkerSurface.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <qt_windows.h>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace {
void recordStage(const QByteArray &stage)
{
    QFile file(QDir::temp().filePath(QStringLiteral("qbrowser-pilot-capabilities-stage.txt")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        (void)file.write(QByteArray::number(GetTickCount64()));
        (void)file.write(" ");
        (void)file.write(stage);
        (void)file.write("\n");
    }
}

void recordFileAutomation(const QByteArray &stage)
{
    QFile file(QDir::temp().filePath(QStringLiteral("qbrowser-file-automation.txt")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        (void)file.write(stage);
        (void)file.write("\n");
    }
}

bool waitUntil(const std::function<bool()> &predicate, const int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return predicate();
}

bool navigateAndWait(MainWindow *window,
                     HostWorkerSessionController *controller,
                     QSignalSpy &acknowledgements,
                     const QString &route)
{
    const int before = acknowledgements.count();
    if (!window->navigate(QStringLiteral("app://pilot") + route)) return false;
    return waitUntil([&] {
        return acknowledgements.count() == before + 1
            && acknowledgements.last().at(0).toString() == route
            && controller->pendingRouteLoadCount() == 0;
    }, 10'000);
}

bool waitForCapabilityIdle(HostWorkerSessionController *controller)
{
    return controller != nullptr
        && waitUntil([&] { return controller->pendingCapabilityCount() == 0; },
                     10'000);
}

int waitForCapability(QSignalSpy &requests,
                      const int from,
                      const QString &capability,
                      const QString &operation,
                      const std::function<bool(const QVariantMap &)> &payloadMatches,
                      const int timeoutMs = 10'000)
{
    int found = -1;
    const bool observed = waitUntil([&] {
        for (int index = from; index < requests.count(); ++index) {
            const QList<QVariant> request = requests.at(index);
            if (request.at(0).toString() == capability
                && request.at(1).toString() == operation
                && (!payloadMatches || payloadMatches(request.at(2).toMap()))) {
                found = index;
                return true;
            }
        }
        return false;
    }, timeoutMs);
    return observed ? found : -1;
}

HWND workerWindow(MainWindow *window)
{
    return window != nullptr && window->workerSurface() != nullptr
        ? reinterpret_cast<HWND>(window->workerSurface()->nativeWindowId())
        : nullptr;
}

bool sendInputs(std::vector<INPUT> inputs)
{
    return !inputs.empty()
        && SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT))
            == inputs.size();
}

bool activateHostWindow(const HWND host)
{
    if (host == nullptr || !IsWindow(host)) return false;
    ShowWindow(host, SW_RESTORE);
    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground != nullptr
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool needsAttach = foregroundThread != 0
        && foregroundThread != currentThread;
    const bool attached = !needsAttach
        || AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    if (attached) {
        (void)BringWindowToTop(host);
        (void)SetForegroundWindow(host);
        (void)SetActiveWindow(host);
    }
    if (attached && needsAttach)
        (void)AttachThreadInput(currentThread, foregroundThread, FALSE);
    return waitUntil([&] {
        return GetAncestor(GetForegroundWindow(), GA_ROOT) == host;
    }, 1'000);
}

bool clickWorker(const HWND worker, const int x, const int y)
{
    auto recordFailure = [](const char *step) {
        recordStage(QByteArray("input-failure-") + step
                    + QByteArray("-error-") + QByteArray::number(GetLastError()));
    };
    if (worker == nullptr || !IsWindow(worker)) {
        recordFailure("worker-window");
        return false;
    }
    const HWND host = GetAncestor(worker, GA_ROOT);
    if (!activateHostWindow(host)) {
        recordFailure("host-activation");
        return false;
    }
    POINT point{x, y};
    if (ClientToScreen(worker, &point) == FALSE) {
        recordFailure("client-to-screen");
        return false;
    }
    if (SetCursorPos(point.x, point.y) == FALSE) {
        recordFailure("set-cursor");
        return false;
    }
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    INPUT up = down;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (!sendInputs({down, up})) {
        recordFailure("send-input");
        return false;
    }
    const bool foreground = waitUntil([&] {
        return GetAncestor(GetForegroundWindow(), GA_ROOT) == host;
    }, 1'000);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(50);
    if (!foreground) {
        const HWND currentForeground = GetForegroundWindow();
        const HWND currentRoot = currentForeground != nullptr
            ? GetAncestor(currentForeground, GA_ROOT) : nullptr;
        DWORD foregroundPid = 0;
        DWORD rootPid = 0;
        (void)GetWindowThreadProcessId(currentForeground, &foregroundPid);
        (void)GetWindowThreadProcessId(currentRoot, &rootPid);
        const DWORD workerThread = GetWindowThreadProcessId(worker, nullptr);
        GUITHREADINFO gui{sizeof(gui)};
        (void)GetGUIThreadInfo(workerThread, &gui);
        recordStage(QByteArray("input-foreground host=")
                    + QByteArray::number(reinterpret_cast<quintptr>(host), 16)
                    + " foreground="
                    + QByteArray::number(reinterpret_cast<quintptr>(currentForeground), 16)
                    + " root="
                    + QByteArray::number(reinterpret_cast<quintptr>(currentRoot), 16)
                    + " foreground-pid=" + QByteArray::number(foregroundPid)
                    + " root-pid=" + QByteArray::number(rootPid)
                    + " worker-focus="
                    + QByteArray::number(reinterpret_cast<quintptr>(gui.hwndFocus), 16));
        recordFailure("foreground-retention");
    }
    return foreground;
}

bool focusWorker(MainWindow *window, const HWND worker)
{
    if (window == nullptr || window->workerSurface() == nullptr
        || worker == nullptr || !IsWindow(worker)) return false;
    const HWND host = GetAncestor(worker, GA_ROOT);
    if (!activateHostWindow(host)) return false;
    window->workerSurface()->clearFocus();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    window->workerSurface()->setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!window->workerSurface()->focusNativeWindow()) return false;
    const DWORD workerThread = GetWindowThreadProcessId(worker, nullptr);
    GUITHREADINFO gui{sizeof(gui)};
    return workerThread != 0 && GetGUIThreadInfo(workerThread, &gui) != FALSE
        && gui.hwndFocus != nullptr
        && (gui.hwndFocus == worker || IsChild(worker, gui.hwndFocus));
}

bool workerHasTrustedInputFocus(const HWND worker)
{
    if (worker == nullptr || !IsWindow(worker)) return false;
    const HWND host = GetAncestor(worker, GA_ROOT);
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != host) return false;
    const DWORD workerThread = GetWindowThreadProcessId(worker, nullptr);
    GUITHREADINFO gui{sizeof(gui)};
    return workerThread != 0 && GetGUIThreadInfo(workerThread, &gui) != FALSE
        && gui.hwndFocus != nullptr
        && (gui.hwndFocus == worker || IsChild(worker, gui.hwndFocus));
}

bool clickWorkerIdempotentFocus(MainWindow *window,
                                const HWND worker,
                                const int x,
                                const int y)
{
    constexpr int maximumAttempts = 3;
    for (int attempt = 1; attempt <= maximumAttempts; ++attempt) {
        if (focusWorker(window, worker) && clickWorker(worker, x, y)) {
            return true;
        }
        recordStage(QByteArrayLiteral("input-idempotent-focus-retry-")
                    + QByteArray::number(attempt));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
    return false;
}

bool sendText(MainWindow *window, const HWND worker, const QString &text)
{
    for (const QChar character : text) {
        if (!workerHasTrustedInputFocus(worker)
            && !focusWorker(window, worker)) {
            return false;
        }
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = character.unicode();
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (!sendInputs({down, up})) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(5);
    }
    return true;
}

bool sendKey(const WORD key, const bool alt = false)
{
    std::vector<INPUT> inputs;
    if (alt) {
        INPUT altDown{};
        altDown.type = INPUT_KEYBOARD;
        altDown.ki.wVk = VK_MENU;
        inputs.push_back(altDown);
    }
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = key;
    inputs.push_back(down);
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(up);
    if (alt) {
        INPUT altUp{};
        altUp.type = INPUT_KEYBOARD;
        altUp.ki.wVk = VK_MENU;
        altUp.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(altUp);
    }
    return sendInputs(std::move(inputs));
}

struct DialogSearch final { DWORD processId = 0; HWND result = nullptr; };
struct DialogAction final {
    DWORD processId = 0;
    int count = 0;
    bool cancel = false;
};

bool isOwnedFileDialog(const HWND window, const DWORD processId)
{
    DWORD windowProcessId = 0;
    wchar_t className[32]{};
    (void)GetWindowThreadProcessId(window, &windowProcessId);
    return windowProcessId == processId && IsWindowVisible(window)
        && GetClassNameW(window, className, 32) > 0
        && QString::fromWCharArray(className) == QStringLiteral("#32770");
}

BOOL CALLBACK findDialogCallback(const HWND window, const LPARAM parameter)
{
    auto *const search = reinterpret_cast<DialogSearch *>(parameter);
    if (isOwnedFileDialog(window, search->processId)) {
        search->result = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK actOnDialogCallback(const HWND window, const LPARAM parameter)
{
    auto *const action = reinterpret_cast<DialogAction *>(parameter);
    if (!isOwnedFileDialog(window, action->processId)) return TRUE;
    ++action->count;
    if (action->cancel) {
        (void)PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        (void)PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

int ownedFileDialogCount(const bool cancel = false)
{
    DialogAction action{GetCurrentProcessId(), 0, cancel};
    (void)EnumWindows(actOnDialogCallback, reinterpret_cast<LPARAM>(&action));
    return action.count;
}

bool waitForOwnedFileDialogsClosed(const int timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    do {
        if (ownedFileDialogCount() == 0) return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return ownedFileDialogCount() == 0;
}

HWND waitForOwnedFileDialog(const int timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    do {
        const HWND foreground = GetForegroundWindow();
        if (isOwnedFileDialog(foreground, GetCurrentProcessId())) return foreground;
        DialogSearch search{GetCurrentProcessId(), nullptr};
        (void)EnumWindows(findDialogCallback, reinterpret_cast<LPARAM>(&search));
        if (search.result != nullptr) return search.result;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

std::thread automateFileDialog(const QString &path,
                               const bool cancel,
                               std::atomic_bool &succeeded)
{
    return std::thread([path, cancel, &succeeded] {
        const HWND dialog = waitForOwnedFileDialog(5'000);
        if (dialog == nullptr || SetForegroundWindow(dialog) == FALSE) return;
        Sleep(250);
        if (cancel) {
            if (ownedFileDialogCount(true) == 0) return;
            succeeded.store(waitForOwnedFileDialogsClosed(2'000),
                            std::memory_order_release);
            return;
        }
        if (!sendKey('N', true)) return;
        Sleep(100);
        GUITHREADINFO gui{sizeof(gui)};
        const DWORD dialogThread = GetWindowThreadProcessId(dialog, nullptr);
        if (dialogThread == 0 || GetGUIThreadInfo(dialogThread, &gui) == FALSE
            || gui.hwndFocus == nullptr) {
            recordFileAutomation("file-success-focus-missing");
            (void)ownedFileDialogCount(true);
            (void)waitForOwnedFileDialogsClosed(2'000);
            return;
        }
        wchar_t focusClass[64]{};
        (void)GetClassNameW(gui.hwndFocus, focusClass, 64);
        recordFileAutomation(QByteArray("file-success-focus-")
                             + QString::fromWCharArray(focusClass).toLatin1()
                             + QByteArray("-id-")
                             + QByteArray::number(GetDlgCtrlID(gui.hwndFocus)));
        const QString nativePath = QDir::toNativeSeparators(path);
        if (SendMessageW(gui.hwndFocus, WM_SETTEXT, 0,
                         reinterpret_cast<LPARAM>(nativePath.utf16())) == FALSE) {
            recordFileAutomation("file-success-set-text-failed");
            (void)ownedFileDialogCount(true);
            (void)waitForOwnedFileDialogsClosed(2'000);
            return;
        }
        wchar_t readBack[1024]{};
        const int readLength = GetWindowTextW(gui.hwndFocus, readBack, 1024);
        const HWND outer = GetAncestor(gui.hwndFocus, GA_ROOT);
        const HWND acceptButton = outer != nullptr ? GetDlgItem(outer, IDOK) : nullptr;
        if (readLength <= 0 || QString::fromWCharArray(readBack) != nativePath
            || acceptButton == nullptr
            || PostMessageW(acceptButton, BM_CLICK, 0, 0) == FALSE) {
            recordFileAutomation("file-success-accept-target-invalid");
            (void)ownedFileDialogCount(true);
            (void)waitForOwnedFileDialogsClosed(2'000);
            return;
        }
        const bool closed = waitForOwnedFileDialogsClosed(2'000);
        recordFileAutomation(closed ? QByteArray("file-success-native-closed")
                                    : QByteArray("file-success-native-still-open"));
        succeeded.store(closed, std::memory_order_release);
        if (!closed) {
            (void)ownedFileDialogCount(true);
            (void)waitForOwnedFileDialogsClosed(2'000);
        }
    });
}

QImage captureWorker(const HWND worker)
{
    RECT rectangle{};
    if (worker == nullptr || GetWindowRect(worker, &rectangle) == FALSE) return {};
    return QGuiApplication::primaryScreen()
        ->grabWindow(0, rectangle.left, rectangle.top,
                     rectangle.right - rectangle.left,
                     rectangle.bottom - rectangle.top).toImage();
}

bool waitForBlueControl(const HWND worker, const int x, const int y)
{
    return waitUntil([&] {
        const QImage image = captureWorker(worker);
        if (image.isNull() || x < 0 || y < 0
            || x >= image.width() || y >= image.height()) return false;
        const QRgb pixel = image.pixel(x, y);
        return qBlue(pixel) > 140 && qRed(pixel) < 100 && qGreen(pixel) < 150;
    }, 5'000);
}

bool waitForDarkPixels(const HWND worker, const QRect &region, const int minimum)
{
    return waitUntil([&] {
        const QImage image = captureWorker(worker);
        if (image.isNull()) return false;
        const QRect visibleRegion = image.rect().intersected(region);
        if (visibleRegion.isEmpty()) return false;
        int dark = 0;
        for (int y = visibleRegion.top(); y <= visibleRegion.bottom(); ++y) {
            for (int x = visibleRegion.left(); x <= visibleRegion.right(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                if (qRed(pixel) < 100 && qGreen(pixel) < 100
                    && qBlue(pixel) < 100) ++dark;
            }
        }
        return dark >= minimum;
    }, 10'000);
}

qsizetype differentPixels(const QImage &left, const QImage &right)
{
    if (left.size() != right.size() || left.isNull()) return 0;
    qsizetype count = 0;
    for (int y = 0; y < left.height(); y += 2)
        for (int x = 0; x < left.width(); x += 2)
            if (left.pixel(x, y) != right.pixel(x, y)) ++count;
    return count;
}

const QByteArray undeclaredClipboardQml = R"QML(
import QtQuick
import Company.Design
Rectangle {
    id: root
    width: 1100; height: 720
    color: Theme.background
    property string noGestureId: ""
    property string noGestureStoreId: ""
    property string undeclaredId: ""
    property string firstId: ""
    property string replayId: ""
    property bool firstOk: false
    property string firstError: ""
    PilotRouter {
        anchors.fill: parent
        runtime: Runtime
        route: Runtime.route
    }
    Rectangle {
        z: 10
        anchors.centerIn: parent
        width: 240; height: 96
        radius: 8
        color: "#2563eb"
        MouseArea {
            anchors.fill: parent
            onClicked: root.firstId = Runtime.invoke("clipboard", "read", {})
        }
    }
    Connections {
        target: Runtime
        function onRouteChanged() {
            if (root.noGestureId.length === 0)
                root.noGestureId = Runtime.invoke("clipboard", "read", {})
        }
        function onCapabilityFinished(id, response) {
            if (id === root.noGestureId) {
                root.noGestureStoreId = Runtime.invoke("storage", "set", {
                    key: "clipboard-no-gesture", value: response.error.code
                })
            } else if (id === root.noGestureStoreId) {
                root.undeclaredId = Runtime.invoke(
                    "clipboard", "write", { text: "must-not-write" })
            } else if (id === root.undeclaredId) {
                Runtime.invoke("storage", "set", {
                    key: "clipboard-undeclared", value: response.error.code
                })
            } else if (id === root.firstId) {
                root.firstOk = response.ok === true
                    && response.result !== undefined
                    && response.result.text === "gesture-canary"
                root.firstError = response.ok === true
                    ? "" : response.error.code
                root.replayId = Runtime.invoke("clipboard", "read", {})
            } else if (id === root.replayId) {
                Runtime.invoke("storage", "set", {
                    key: "clipboard-gesture",
                    value: (root.firstOk ? "first-ok:"
                        : "first-failed-" + root.firstError + ":")
                        + response.error.code
                })
            }
        }
    }
}
)QML";

}

class PilotCapabilitiesE2eTest final : public QObject
{
    Q_OBJECT
private slots:
    void productionPilotBusinessCapabilities();
};

void PilotCapabilitiesE2eTest::productionPilotBusinessCapabilities()
{
    (void)QFile::remove(QDir::temp().filePath(
        QStringLiteral("qbrowser-pilot-capabilities-stage.txt")));
    recordStage("start");
    TestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    QVERIFY2(environment.start(), qPrintable(environment.error()));
    MainWindow *const window = environment.host()->mainWindow();
    HostWorkerSessionController *const controller =
        environment.host()->workerSessionController();
    QVERIFY(window != nullptr);
    QVERIFY(controller != nullptr);
    QSignalSpy requests(environment.host(),
                       &HostApplication::workerCapabilityRequestObserved);
    QSignalSpy acknowledgements(controller,
                                &HostWorkerSessionController::routeLoadAcknowledged);
    QSignalSpy responsesQueued(
        controller, &HostWorkerSessionController::capabilityResponseQueued);
    QSignalSpy responsesSent(controller,
                             &HostWorkerSessionController::capabilityResponseSent);
    QSignalSpy workerExits(environment.host(), &HostApplication::packageWorkerExited);
    QVERIFY(requests.isValid());
    QVERIFY(acknowledgements.isValid());
    QVERIFY(responsesQueued.isValid());
    QVERIFY(responsesSent.isValid());
    QVERIFY(workerExits.isValid());
    connect(environment.host(), &HostApplication::packageWorkerReady, this,
            [](const QString &, const QString &version, const QString &,
               const quint64 activation, const quint64 attempt, const quint32 pid) {
                recordStage(QByteArray("signal-ready-") + version.toLatin1()
                            + '-' + QByteArray::number(activation) + '-'
                            + QByteArray::number(attempt) + '-'
                            + QByteArray::number(pid));
            });
    connect(environment.host(), &HostApplication::packageWorkerExited, this,
            [](const quint64 activation, const quint64 attempt) {
                recordStage(QByteArray("signal-unexpected-exit-")
                            + QByteArray::number(activation) + '-'
                            + QByteArray::number(attempt));
            });
    connect(environment.host(), &HostApplication::updateLifecycleFailed, this,
            [](const QString &error) {
                recordStage(QByteArray("signal-lifecycle-failed-")
                            + error.toLatin1());
            });
    connect(controller, &HostWorkerSessionController::failed, this,
            [](const QString &error) {
                recordStage(QByteArray("signal-session-failed-")
                            + error.toLatin1());
            });

    recordStage("initial-route-barrier");
    QVERIFY(waitUntil([&] {
        return controller->pendingRouteLoadCount() == 0;
    }, 10'000));
    recordStage("login");
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/login")));
    HWND worker = workerWindow(window);
    QVERIFY(worker != nullptr);
    QVERIFY(activateHostWindow(GetAncestor(worker, GA_ROOT)));
    QVERIFY(focusWorker(window, worker));
    QVERIFY(waitForBlueControl(worker, 550, 442));
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-login-before.png")));
    recordStage("login-email");
    QVERIFY(clickWorkerIdempotentFocus(window, worker, 550, 308));
    QVERIFY(sendText(window, worker, QStringLiteral("pilot@example.com")));
    recordStage("login-password");
    QVERIFY(workerHasTrustedInputFocus(worker) || focusWorker(window, worker));
    QVERIFY(sendKey(VK_TAB));
    QVERIFY(sendText(window, worker, QStringLiteral("pilot-pass")));
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-login-after.png")));
    recordStage("login-submit");
    const int loginAck = acknowledgements.count();
    QVERIFY(workerHasTrustedInputFocus(worker) || focusWorker(window, worker));
    QVERIFY(sendKey(VK_TAB));
    QVERIFY(workerHasTrustedInputFocus(worker) || focusWorker(window, worker));
    QVERIFY(sendKey(VK_RETURN));
    QVERIFY(environment.waitForMockRequest(QStringLiteral("POST"),
                                           QStringLiteral("/api/login")));
    QVERIFY(waitUntil([&] {
        return acknowledgements.count() == loginAck + 1
            && acknowledgements.last().at(0).toString()
                == QStringLiteral("/dashboard")
            && controller->pendingRouteLoadCount() == 0;
    }, 10'000));
    QVERIFY(environment.waitForMockRequest(QStringLiteral("GET"),
                                           QStringLiteral("/api/dashboard")));
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("order");
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/orders/ORD-1001")));
    QVERIFY(environment.waitForMockRequest(QStringLiteral("GET"),
                                           QStringLiteral("/api/orders/ORD-1001")));
    QVERIFY(waitForCapabilityIdle(controller));
    // Route acknowledgement and broker completion can precede the QML model
    // consuming the response. Wait for the enabled "processing" destination.
    const int orderControlTop = captureWorker(worker).height() - 67;
    QVERIFY(orderControlTop >= 0);
    QVERIFY(waitForDarkPixels(worker, QRect(340, orderControlTop, 95, 48), 30));
    const int orderMutationFrom = requests.count();
    QVERIFY(clickWorker(worker, 383, orderControlTop + 21));
    const int orderMutation = waitForCapability(
        requests, orderMutationFrom, QStringLiteral("network"),
        QStringLiteral("request"), [](const QVariantMap &payload) {
            if (payload.value(QStringLiteral("method")).toString()
                    != QStringLiteral("PATCH")) return false;
            const QJsonObject body = QJsonDocument::fromJson(
                QByteArray::fromBase64(payload.value(QStringLiteral("bodyBase64"))
                                           .toString().toLatin1())).object();
            return payload.value(QStringLiteral("url")).toString()
                       .endsWith(QStringLiteral("/api/orders/ORD-1001"))
                && body == QJsonObject{{QStringLiteral("status"),
                                        QStringLiteral("processing")}};
        });
    QVERIFY(orderMutation >= 0);
    QVERIFY(environment.waitForMockRequest(QStringLiteral("PATCH"),
                                           QStringLiteral("/api/orders/ORD-1001")));
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("customers");
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/customers")));
    QVERIFY(environment.waitForMockRequest(
        QStringLiteral("GET"),
        QStringLiteral("/api/customers?page=1&pageSize=20&query=")));
    QVERIFY(waitForCapabilityIdle(controller));
    recordStage("customers-list");
    QTest::qWait(100);
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-customers-list.png")));
    const int customerAck = acknowledgements.count();
    QVERIFY(clickWorker(worker, 650, 170));
    recordStage("customers-selected");
    const int customerActionY = captureWorker(worker).height() - 44;
    QVERIFY(customerActionY >= 0);
    QVERIFY(clickWorker(worker, 900, customerActionY));
    recordStage("customers-open");
    QVERIFY(waitUntil([&] {
        return acknowledgements.count() == customerAck + 1
            && acknowledgements.last().at(0).toString()
                == QStringLiteral("/customers/CUS-001")
            && controller->pendingRouteLoadCount() == 0;
    }, 10'000));
    QVERIFY(environment.waitForMockRequest(QStringLiteral("GET"),
                                           QStringLiteral("/api/customers/CUS-001")));
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("settings-initial");
    int settingsFrom = requests.count();
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/settings")));
    QVERIFY(waitForCapability(
        requests, settingsFrom, QStringLiteral("storage"), QStringLiteral("get"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key")) == QStringLiteral("theme");
        }) >= 0);
    QVERIFY(waitForCapabilityIdle(controller));
    QVERIFY(clickWorker(worker, 432, 140));
    QVERIFY(waitForCapability(
        requests, settingsFrom, QStringLiteral("storage"), QStringLiteral("set"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key")) == QStringLiteral("theme")
                && payload.value(QStringLiteral("value")) == QStringLiteral("dark");
        }) >= 0);
    QVERIFY(waitForCapabilityIdle(controller));
    const QString update = environment.createPackage(QStringLiteral("1.0.1"));
    QVERIFY(!update.isEmpty());
    QVERIFY(environment.install(update));
    QVERIFY(environment.waitForVerified(QStringLiteral("1.0.1")));
    QVERIFY(environment.reloadActiveTab());
    QVERIFY(environment.waitForReady(QStringLiteral("1.0.1")));
    worker = workerWindow(window);
    QVERIFY(worker != nullptr);
    settingsFrom = requests.count();
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/settings")));
    QVERIFY(waitForCapability(
        requests, settingsFrom, QStringLiteral("storage"), QStringLiteral("get"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key")) == QStringLiteral("theme");
        }) >= 0);
    QVERIFY(waitForCapabilityIdle(controller));
    const int persistedFrom = requests.count();
    QVERIFY(clickWorker(worker, 328, 140));
    QVERIFY(waitForCapability(
        requests, persistedFrom, QStringLiteral("storage"), QStringLiteral("set"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key")) == QStringLiteral("theme")
                && payload.value(QStringLiteral("value")) == QStringLiteral("light");
        }) >= 0);
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("file");
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/files")));
    recordStage("file-route");
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-files.png")));
    QVERIFY(waitForBlueControl(worker, 280, 127));
    recordStage("file-rendered");
    const QImage beforeFile = captureWorker(worker);
    QVERIFY(!beforeFile.isNull());
    std::atomic_bool dialogAutomated{false};
    std::thread dialog = automateFileDialog({}, true, dialogAutomated);
    const int cancelFrom = requests.count();
    const bool cancelClicked = clickWorker(worker, 312, 127);
    recordStage("file-cancel-clicked");
    const int cancelRequest = waitForCapability(
        requests, cancelFrom, QStringLiteral("file"), QStringLiteral("open"), {});
    recordStage("file-cancel-request");
    dialog.join();
    recordStage("file-cancel-joined");
    QVERIFY(cancelClicked);
    QVERIFY(cancelRequest >= 0);
    QVERIFY(dialogAutomated.load(std::memory_order_acquire));
    recordStage("file-cancel-dialog-closed");
    QVERIFY(waitForCapabilityIdle(controller));
    recordStage("file-cancel-complete");

    QTemporaryDir userFiles;
    QVERIFY(userFiles.isValid());
    const QString selectedPath = userFiles.filePath(QStringLiteral("pilot-note.txt"));
    QFile selected(selectedPath);
    QVERIFY(selected.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(selected.write("pilot-file"), qint64(10));
    selected.close();
    dialogAutomated.store(false, std::memory_order_release);
    recordStage("file-success-start");
    dialog = automateFileDialog(selectedPath, false, dialogAutomated);
    const int openFrom = requests.count();
    const bool openClicked = clickWorker(worker, 312, 127);
    const int openRequest = waitForCapability(
        requests, openFrom, QStringLiteral("file"), QStringLiteral("open"), {});
    recordStage("file-success-request");
    dialog.join();
    recordStage("file-success-joined");
    QVERIFY(openClicked);
    QVERIFY(openRequest >= 0);
    QVERIFY(dialogAutomated.load(std::memory_order_acquire));
    recordStage("file-success-dialog-closed");
    QVERIFY(waitForCapabilityIdle(controller));
    recordStage("file-success-complete");
    QVERIFY2(waitUntil([&] {
        return differentPixels(beforeFile, captureWorker(worker)) > 2'000;
    }, 10'000),
             "QML file metadata view did not consume the broker response");
    recordStage("file-success-rendered");

    QVERIFY(environment.waitForHealthyVersion(QStringLiteral("1.0.1")));
    const QString healthyCurrent = environment.currentVersionDirectory();
    QVERIFY(!healthyCurrent.isEmpty());
    QCOMPARE(environment.lastKnownGoodVersionDirectory(), healthyCurrent);
    recordStage("pilot-update-healthy");

    recordStage("undeclared-clipboard");
    const QString undeclared = environment.createPackage(
        QStringLiteral("1.0.2"), undeclaredClipboardQml, true);
    QVERIFY(!undeclared.isEmpty());
    const int undeclaredFrom = requests.count();
    QVERIFY(environment.install(undeclared));
    recordStage("undeclared-installed");
    QVERIFY(environment.waitForVerified(QStringLiteral("1.0.2")));
    QVERIFY(environment.reloadActiveTab());
    QVERIFY(environment.waitForReady(QStringLiteral("1.0.2")));
    recordStage("undeclared-ready");
    QVERIFY(navigateAndWait(window, controller, acknowledgements,
                            QStringLiteral("/files")));
    recordStage("undeclared-route-ready");
    worker = workerWindow(window);
    QVERIFY(worker != nullptr);
    DWORD undeclaredWindowPid = 0;
    (void)GetWindowThreadProcessId(worker, &undeclaredWindowPid);
    recordStage(QByteArray("undeclared-worker-pid-")
                + QByteArray::number(undeclaredWindowPid) + QByteArray("-expected-")
                + QByteArray::number(environment.currentWorkerProcessId()));
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-undeclared.png")));
    const bool undeclaredRendered = waitForBlueControl(worker, 550, 360);
    if (!undeclaredRendered) {
        const WorkerSurface *const currentSurface = window->workerSurface();
        recordStage(QByteArray("undeclared-render-failed-exits-")
                    + QByteArray::number(workerExits.count()) + "-surface-"
                    + (currentSurface == nullptr ? QByteArray("null")
                       : QByteArray::number(currentSurface->nativeWindowId())));
    }
    QVERIFY(undeclaredRendered);
    recordStage("undeclared-rendered");
    const int noGestureStorage = waitForCapability(
        requests, undeclaredFrom, QStringLiteral("storage"), QStringLiteral("set"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key"))
                       == QStringLiteral("clipboard-no-gesture");
        });
    QVERIFY(noGestureStorage >= 0);
    QCOMPARE(requests.at(noGestureStorage).at(2).toMap()
                 .value(QStringLiteral("value")).toString(),
             QStringLiteral("clipboard.gesture_required"));
    const int undeclaredStorage = waitForCapability(
        requests, undeclaredFrom, QStringLiteral("storage"), QStringLiteral("set"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key"))
                       == QStringLiteral("clipboard-undeclared");
        });
    QVERIFY(undeclaredStorage >= 0);
    const QString undeclaredValue = requests.at(undeclaredStorage).at(2).toMap()
                                        .value(QStringLiteral("value")).toString();
    recordStage(QByteArray("undeclared-value-") + undeclaredValue.toLatin1());
    QCOMPARE(undeclaredValue, QStringLiteral("capability.denied"));
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("gesture-clipboard");
    QGuiApplication::clipboard()->setText(QStringLiteral("gesture-canary"));
    (void)captureWorker(worker).save(
        QDir::temp().filePath(QStringLiteral("qbrowser-gesture.png")));
    const bool gestureRendered = waitForBlueControl(worker, 550, 360);
    if (!gestureRendered) {
        recordStage(QByteArray("gesture-render-failed-")
                    + environment.lastFailure().toLatin1() + QByteArray("-")
                    + controller->lastErrorCode().toLatin1());
    }
    QVERIFY(gestureRendered);
    recordStage("gesture-rendered");
    QVERIFY(focusWorker(window, worker));
    TabController *const activeTab = window->tabController(
        window->tabModel()->activeId());
    QVERIFY(activeTab != nullptr);
    AppTabRuntimeController *const activeRuntime =
        activeTab->appRuntimeController();
    QVERIFY(activeRuntime != nullptr);
    HostCapabilityRuntime *const activeCapability =
        activeRuntime->capabilityRuntime();
    HostGestureRouter *const gestureRouter =
        environment.host()->gestureRouterForTesting();
    QVERIFY(activeCapability != nullptr);
    QVERIFY(gestureRouter != nullptr);
    QVERIFY(waitUntil([&] {
        return gestureRouter->isActiveBinding(activeCapability->authority());
    }, 1'000));
    recordStage("gesture-focused");
    const int gestureFrom = requests.count();
    const int gestureQueuedFrom = responsesQueued.count();
    const int gestureSentFrom = responsesSent.count();
    QVERIFY(clickWorker(worker, 550, 360));
    recordStage("gesture-clicked");
    QVERIFY(waitForCapability(requests, gestureFrom, QStringLiteral("clipboard"),
                              QStringLiteral("read"), {}) >= 0);
    recordStage("gesture-clipboard-request");
    QVERIFY(waitUntil([&] {
        return responsesQueued.count() > gestureQueuedFrom;
    }, 10'000));
    const QList<QVariant> firstClipboardResponse = responsesQueued.last();
    recordStage(QByteArray("gesture-first-response-queued-")
                + (firstClipboardResponse.at(1).toBool() ? QByteArray("ok-")
                                                        : QByteArray("error-"))
                + firstClipboardResponse.at(2).toString().toLatin1());
    QVERIFY(waitUntil([&] {
        return responsesSent.count() > gestureSentFrom;
    }, 10'000));
    recordStage("gesture-first-response-sent");
    const int replayRequest = waitForCapability(
        requests, gestureFrom + 1, QStringLiteral("clipboard"),
        QStringLiteral("read"), {});
    QVERIFY(replayRequest >= 0);
    recordStage("gesture-replay-request");
    QVERIFY(waitUntil([&] {
        return responsesQueued.count() > gestureQueuedFrom + 1;
    }, 10'000));
    const QList<QVariant> replayResponse = responsesQueued.last();
    recordStage(QByteArray("gesture-replay-response-queued-")
                + (replayResponse.at(1).toBool() ? QByteArray("ok-")
                                                : QByteArray("error-"))
                + replayResponse.at(2).toString().toLatin1());
    QVERIFY(waitUntil([&] {
        return responsesSent.count() > gestureSentFrom + 1;
    }, 10'000));
    recordStage("gesture-replay-response-sent");
    const int gestureStorage = waitForCapability(
        requests, gestureFrom, QStringLiteral("storage"), QStringLiteral("set"),
        [](const QVariantMap &payload) {
            return payload.value(QStringLiteral("key"))
                       == QStringLiteral("clipboard-gesture");
        });
    QVERIFY(gestureStorage >= 0);
    const QString gestureValue = requests.at(gestureStorage).at(2).toMap()
                                     .value(QStringLiteral("value")).toString();
    recordStage(QByteArray("gesture-value-") + gestureValue.toLatin1());
    QCOMPARE(gestureValue,
             QStringLiteral("first-ok:clipboard.gesture_required"));
    QVERIFY(waitForCapabilityIdle(controller));

    recordStage("shutdown");
    QVERIFY2(environment.shutdown(), qPrintable(environment.error()));
}

QTEST_MAIN(PilotCapabilitiesE2eTest)
#include "tst_pilot_capabilities.moc"
