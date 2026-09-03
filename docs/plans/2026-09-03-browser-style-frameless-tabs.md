# Browser-Style Frameless Tabs Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Place the existing Q-Browser tab strip in the Windows title area with browser-like sizing and styling while retaining Qt's Windows caption controls, native resize and snapping, tab behavior, and accessibility.

**Architecture:** `MainWindow` enables Qt 6.11's expanded client area, hides the Qt title text/icon with `CustomizeWindowHint`, and derives a right content inset from the Windows top safe inset for Qt's three caption buttons. `BrowserChrome` owns a compact browser tab row plus a dedicated empty drag region; it emits window-move and maximize/restore requests without changing the existing tab model or command flow.

**Tech Stack:** C++20, Qt 6.11 Core/Gui/Widgets/Test, CMake 3.30, CTest, Windows desktop window management

---

## Working rules

- Execute in a new ignored worktree created from `codex/q-browser-mvp`.
- Preserve `.codex-task3-audit-solmax/`, `.tmp/`, and all unrelated worktrees.
- Use @superpowers:test-driven-development for every behavior change.
- Use @superpowers:systematic-debugging for unexpected build, test, or runtime failures.
- Do not alter `BrowserTabModel`, session persistence, route policy, Worker authority, or WebEngine policy.
- Keep production interfaces free of test-only hooks; inspect real widgets through stable object names.
- Commit after each green task.

### Task 1: Establish the browser-style tab-row contract

**Files:**

- Modify: `apps/host/BrowserChrome.cpp:181-263`
- Modify: `tests/unit/host/tst_browser_chrome.cpp:272-304`
- Test: `tests/unit/host/tst_browser_chrome.cpp`

**Step 1: Add a failing visual-contract test**

Add `tabRowUsesBrowserSizingAndOverflow()` to `BrowserChromeTest`. Construct a
`BrowserChrome`, synchronize short and long tab titles, show it at 900 logical
pixels wide, and assert:

```cpp
auto *const tabRow = chrome.findChild<QWidget *>(
    QStringLiteral("browser-tab-row"));
auto *const newTab = chrome.findChild<QToolButton *>(
    QStringLiteral("browser-new-tab"));
QVERIFY(tabRow != nullptr);
QCOMPARE(tabRow->height(), 42);
QVERIFY(chrome.tabBar()->documentMode());
QVERIFY(chrome.tabBar()->usesScrollButtons());
QVERIFY(!chrome.tabBar()->drawBase());
QCOMPARE(newTab->text(), QStringLiteral("+"));
QCOMPARE(newTab->focusPolicy(), Qt::StrongFocus);
for (int index = 0; index < chrome.tabBar()->count(); ++index) {
    const int width = chrome.tabBar()->tabRect(index).width();
    QVERIFY(width >= 120);
    QVERIFY(width <= 240);
}
```

Also assert that the existing accessible name and description on New Tab are
still non-empty.

**Step 2: Run the focused test to verify RED**

Configure and build from the worktree root:

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug --target tst_browser_chrome -- /m:1 /nr:false
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^browser_chrome$' -j1
```

Expected: `browser_chrome` fails on the new 42-pixel/browser-sizing contract.

**Step 3: Implement the minimum browser-style tab bar**

In the anonymous namespace of `BrowserChrome.cpp`, add a private subclass that
clamps logical tab size without exposing a new production API:

```cpp
constexpr int BrowserTitleRowHeight = 42;
constexpr int BrowserTabMinimumWidth = 120;
constexpr int BrowserTabMaximumWidth = 240;
constexpr int BrowserTabHeight = 36;

class BrowserTabBar final : public QTabBar
{
public:
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override
    {
        QSize size = QTabBar::tabSizeHint(index);
        size.setWidth(std::clamp(size.width(), BrowserTabMinimumWidth,
                                 BrowserTabMaximumWidth));
        size.setHeight(BrowserTabHeight);
        return size;
    }
};
```

Include `<algorithm>`. Replace `new QTabBar` with `new BrowserTabBar`, set the
row fixed height, and configure:

```cpp
tabBar_->setDocumentMode(true);
tabBar_->setDrawBase(false);
tabBar_->setUsesScrollButtons(true);
tabBar_->setIconSize(QSize(16, 16));
tabBar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
```

After assigning the New Tab action, set its visible text to `+`, keep its
accessible name/description and tooltip as `New tab`, and give it a strong
focus policy.

Apply a scoped stylesheet using `palette(...)` roles only. Style
`browser-tab-row`, `browser-tab-bar::tab`, selected tabs, inactive hover, tab
close buttons, and `browser-new-tab`. The selected tab must visually connect to
the navigation row; do not set global application styling or fixed RGB theme
colors.

**Step 4: Run focused GREEN verification**

Repeat the build and `browser_chrome` CTest command from Step 2.

Expected: `browser_chrome` passes with all existing accessibility, command,
and model-synchronization tests unchanged.

**Step 5: Commit**

```powershell
git diff --check
git add -- apps/host/BrowserChrome.cpp tests/unit/host/tst_browser_chrome.cpp
git commit -m "feat: style browser tab strip"
```

### Task 2: Add an isolated title-row drag region

**Files:**

- Modify: `apps/host/BrowserChrome.h:16-93`
- Modify: `apps/host/BrowserChrome.cpp:181-263`
- Modify: `tests/unit/host/tst_browser_chrome.cpp:272-304`
- Test: `tests/unit/host/tst_browser_chrome.cpp`

**Step 1: Add failing interaction tests**

Add `emptyTitleAreaOwnsOnlyWindowGestures()` to `BrowserChromeTest`. Find
`browser-title-drag-area`, attach `QSignalSpy` instances to two new
`BrowserChrome` signals, and verify:

```cpp
QVERIFY(dragArea != nullptr);
QCOMPARE(dragArea->focusPolicy(), Qt::NoFocus);
QTest::mousePress(dragArea, Qt::LeftButton);
QCOMPARE(moveRequests.count(), 0);
// A held-button move at QApplication::startDragDistance() emits exactly once.
QTest::mouseDClick(dragArea, Qt::LeftButton);
QCOMPARE(maximizeRequests.count(), 1);
```

Send primary-button events to the tab bar and New Tab button and assert neither
window gesture signal is emitted. Send a right-button press to the drag area and
assert it is ignored.

**Step 2: Run focused RED verification**

Build `tst_browser_chrome` and run `ctest -R '^browser_chrome$' -j1` as in Task
1. Expected: compilation fails because the drag area and signals do not exist.

**Step 3: Implement the drag area and event routing**

Add legitimate production signals to `BrowserChrome`:

```cpp
void windowMoveRequested();
void windowMaximizeRestoreRequested();
```

Add a private `QWidget *titleDragArea_` member and override:

```cpp
bool eventFilter(QObject *watched, QEvent *event) override;
```

Create `browser-title-drag-area` after New Tab, give it an expanding horizontal
size policy, no focus, and no mouse-transparent attribute. Install
`BrowserChrome` as its event filter. Change the row layout order to:

```text
QTabBar | New Tab | expanding drag area | native safe-area inset
```

In `eventFilter`, record a left-button `MouseButtonPress`, emit one move request
only when a held-button move reaches `QApplication::startDragDistance()`, and
reset on release. Consume `MouseButtonDblClick` by clearing the pending move and
emitting only the maximize/restore signal. Forward every other object/event to
`QWidget::eventFilter`.

**Step 4: Run focused GREEN verification**

Rebuild and run `browser_chrome`. Expected: all tests pass, including exact
separation between tab input and window input.

**Step 5: Commit**

```powershell
git diff --check
git add -- apps/host/BrowserChrome.h apps/host/BrowserChrome.cpp tests/unit/host/tst_browser_chrome.cpp
git commit -m "feat: add browser title drag area"
```

### Task 3: Integrate the native expanded title area

**Files:**

- Modify: `apps/host/BrowserChrome.h:16-93`
- Modify: `apps/host/BrowserChrome.cpp:181-263`
- Modify: `apps/host/MainWindow.h:35-156`
- Modify: `apps/host/MainWindow.cpp:58-160`
- Modify: `tests/unit/host/tst_browser_chrome.cpp:272-304`
- Modify: `tests/integration/host/tst_browser_shell.cpp:218-268`
- Test: `tests/unit/host/tst_browser_chrome.cpp`
- Test: `tests/integration/host/tst_browser_shell.cpp`

**Step 1: Add a failing safe-area layout test**

Add `safeAreaInsetsReserveNativeCaptionControls()` to `BrowserChromeTest`.
Call a new production method with `QMargins(3, 11, 37, 13)`, inspect the title
row layout, and require the base horizontal margin plus safe left/right inset
while keeping the 3-pixel vertical margins:

```cpp
chrome.setTitleBarSafeAreaMargins(QMargins(3, 11, 37, 13));
const QMargins applied = tabRow->layout()->contentsMargins();
QCOMPARE(applied, QMargins(11, 3, 45, 3));
```

Repeat with negative margins and require clamping to the base
`QMargins(8, 3, 8, 3)`.

**Step 2: Add a failing MainWindow chrome test**

Add `mainWindowUsesExpandedNativeTitleArea()` to `BrowserShellTest`. Construct
and show a `MainWindow`, then assert:

```cpp
QVERIFY(window.windowFlags().testFlag(Qt::ExpandedClientAreaHint));
QVERIFY(window.windowFlags().testFlag(Qt::NoTitleBarBackgroundHint));
QVERIFY(!window.windowFlags().testFlag(Qt::FramelessWindowHint));
QVERIFY(window.windowFlags().testFlag(Qt::CustomizeWindowHint));
QVERIFY(!window.windowFlags().testFlag(Qt::WindowTitleHint));
QVERIFY(window.windowFlags().testFlag(Qt::WindowMinMaxButtonsHint));
QVERIFY(window.windowFlags().testFlag(Qt::WindowCloseButtonHint));
```

Spy on or directly invoke the two `BrowserChrome` window-gesture signals:
double-click must toggle normal/maximized state twice, while a move request must
be accepted without changing browser tab state. Do not require synthetic input
to complete a native move loop in automated CTest.

**Step 3: Run both focused tests to verify RED**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug --target tst_browser_chrome tst_browser_shell -- /m:1 /nr:false
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_chrome|browser_shell)$' -j1
```

Expected: the new safe-area method and window flags/state assertions fail.

**Step 4: Implement safe-area margins in BrowserChrome**

Expose:

```cpp
void setTitleBarSafeAreaMargins(const QMargins &margins);
```

Store the title-row `QHBoxLayout *` as a private member. Clamp only left and
right margins to nonnegative values and apply them on top of the base
`QMargins(8, 3, 8, 3)`. Ignore top and bottom safe-area values because the row
itself occupies the extended title area.

**Step 5: Enable and synchronize the expanded area in MainWindow**

Before creating the central widget, construct an explicit flag set that enables:

```cpp
Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint
    | Qt::CustomizeWindowHint | Qt::WindowSystemMenuHint
    | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
    | Qt::WindowCloseButtonHint
```

Clear `Qt::WindowTitleHint` so Qt does not draw a duplicate title/icon. Keep the
standard minimize/maximize/close flags and do not enable
`Qt::FramelessWindowHint`; apply the complete pre-handle flag set with
`overrideWindowFlags()` so QWidget does not restore `WindowTitleHint`.

Add a private `synchronizeTitleBarSafeArea()` method and a stored
`QMetaObject::Connection`. Override `showEvent(QShowEvent *)`, call the base
implementation, obtain `windowHandle()`, reconnect only when necessary, and
read `QWindow::safeAreaMargins()`, and refresh on `safeAreaMarginsChanged`. On
Windows, Qt 6.11 reports the title-bar height in the top inset rather than a
horizontal caption-button inset. Reserve the larger of the reported right inset
and three button widths, where each button width is 1.5 times the top inset,
then forward the derived content insets to `BrowserChrome`.

Connect `windowMoveRequested` to `windowHandle()->startSystemMove()`. Connect
`windowMaximizeRestoreRequested` to `showNormal()` when maximized and
`showMaximized()` otherwise. Guard a null `windowHandle()` without creating a
manual movement fallback.

**Step 6: Run focused GREEN verification**

Rebuild and run `browser_chrome` plus `browser_shell` serially. Expected: both
tests pass and existing tab/runtime assertions remain unchanged.

**Step 7: Commit**

```powershell
git diff --check
git add -- apps/host/BrowserChrome.h apps/host/BrowserChrome.cpp apps/host/MainWindow.h apps/host/MainWindow.cpp tests/unit/host/tst_browser_chrome.cpp tests/integration/host/tst_browser_shell.cpp
git commit -m "feat: integrate tabs with native title area"
```

### Task 4: Verify the complete browser shell and Windows behavior

**Files:**

- Modify production files only if test evidence identifies a defect in the
  already scoped feature implementation.
- Modify `tests/e2e/tst_pilot_capabilities.cpp` if the new chrome height exposes
  fixed-pixel interaction assumptions; derive centered controls from the actual
  Worker surface dimensions and retain explicit bounds/color/focus assertions.
- Update: `docs/verification/mvp-acceptance-report.md` only if final Release
  evidence is regenerated.

**Step 1: Run source and focused checks**

```powershell
git status --short
git diff --check
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug --target tst_browser_chrome tst_browser_shell qbrowser-host -- /m:1 /nr:false
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_chrome|browser_shell)$' -j1
```

Expected: clean diff checks and both focused suites pass.

**Step 2: Perform a real Windows visual/interaction smoke test**

Launch the Debug Host through the existing accepted development runtime
configuration. Verify at 100% and the available non-100% DPI scale:

- no separate title-bar band appears;
- Qt's Windows caption controls remain visible and clickable;
- tabs and New Tab do not overlap caption controls;
- empty strip space moves the window and double-click toggles maximize;
- tab drag still reorders instead of moving the window;
- edge resize, drag-to-edge snap, restore, and maximize work;
- active, inactive, hover, loading, crash, and long-title states remain legible.

Capture a screenshot for visual inspection but do not add generated screenshots
to the repository.

**Step 3: Run the complete verification suites**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug -- /m:1 /nr:false
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -j1
& 'C:\Program Files\nodejs\npm.cmd' test --prefix tools
```

Expected: the current 55-test Qt/C++ inventory and all 172 tools tests pass with
zero failures.

**Step 4: Check the Release host inventory**

Build `qbrowser-host` with `BUILD_TESTING=OFF` using the existing Release
configuration and run `ctest -N` against that build. Expected: the Host builds,
the test inventory remains `Total Tests: 0`, and no test-only window hook is
added.

**Step 5: Review and commit any evidence-only update**

Apply @superpowers:requesting-code-review and
@superpowers:verification-before-completion. Inspect the full change range and
resolve all actionable findings with focused RED/GREEN evidence.

If the acceptance report was updated, commit only that report:

```powershell
git add -- docs/verification/mvp-acceptance-report.md
git commit -m "docs: record frameless tab acceptance"
```

Otherwise make no empty evidence commit.

**Step 6: Finish the branch**

Use @superpowers:finishing-a-development-branch. Follow the user's selected
integration option, verify again after any local merge, and remove only the new
clean worktree. Preserve all pre-existing untracked directories and unrelated
worktrees.
