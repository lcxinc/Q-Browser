# Browser-Style Frameless Tabs Design

**Status:** Approved on 2026-09-03
**Scope:** Integrate the existing browser tab strip into the Windows title area
without changing tab identity, navigation, session, or runtime isolation.

## Objective

Make Q-Browser look and behave like a modern desktop browser: the tab strip
occupies the top of the window, there is no separate title-bar band, the Qt
Windows caption controls remain on the right, and unused strip space moves or
maximizes the window.

## Chosen approach

Use Qt 6.11's `Qt::ExpandedClientAreaHint` together with
`Qt::NoTitleBarBackgroundHint`. `Qt::CustomizeWindowHint` suppresses the Qt
title text and icon while retaining the minimize, maximize/restore, and close
buttons supplied by Qt's Windows platform integration. The native frame still
provides edge resize, DPI handling, and snapping behavior.

The alternatives were rejected:

- `Qt::FramelessWindowHint` plus fully custom controls would require Q-Browser
  to reproduce resize, shadow, DPI, system-menu, snapping, and accessibility
  behavior.
- Direct `WM_NCCALCSIZE` and `WM_NCHITTEST` handling would offer more control
  but would add unnecessary Windows-specific non-client complexity.

## Structure and responsibilities

`MainWindow` enables the expanded client-area and customized-caption flags
before the native window is shown. Once its `QWindow` exists, it observes
`safeAreaMarginsChanged` and sends content insets to `BrowserChrome`. Qt 6.11's
Windows backend reports the title-bar height as the top safe inset; the right
inset therefore reserves three Qt caption buttons, each 1.5 times that height.

`BrowserChrome` keeps its existing two-row structure:

1. The title-area row contains the movable `QTabBar`, tab close buttons, the
   New Tab button, and unused drag space.
2. The navigation row contains Back, Forward, Reload/Stop, Home, the address
   field, and the content-identity presentation.

A small title drag-area widget owns only pointer gestures on unused space. It
records a primary-button press and calls `QWindow::startSystemMove()` only after
movement reaches `QApplication::startDragDistance()`. A double-click toggles the
owning `MainWindow` between maximized and normal state without starting a move.
Tabs and the New Tab button remain outside this drag hit region, so their
existing activation, reorder, close, and creation behavior is unchanged.

`BrowserChrome` applies the derived left and right content insets on top of its
base `QMargins(8, 3, 8, 3)` title-row margins so tabs and the New Tab button do
not overlap the caption controls. It refreshes the inset when the platform
reports changes caused by DPI, screen, or window-state transitions.

No data-model path changes. `BrowserTabModel`, stable tab IDs, command actions,
session persistence, per-tab controllers, and runtime authority remain the sole
sources of browser state.

## Visual behavior

- The title row is approximately 42 logical pixels high and touches the top
  window edge.
- Active tabs use a rounded panel that visually connects to the navigation row.
- Inactive tabs blend into the title row and receive a restrained hover state.
- Tab widths stay between approximately 120 and 240 logical pixels; long titles
  are elided and overflow uses the existing tab-bar scrolling behavior.
- The New Tab action is a compact plus button after the tab list.
- Existing content-identity, loading, recovery, crash, and close indicators are
  preserved.
- Colors derive from the active Qt palette so light, dark, high-contrast, and
  inactive-window states remain legible.
- Qt's Windows caption buttons remain visible in the reserved right region.

Only unused title-row space moves the window. Dragging a tab continues to
reorder it. Double-clicking unused space toggles maximize/restore. System edge
resize and drag-to-edge snapping continue to be provided by the native frame.

If `startSystemMove()` declines a request, the window remains in place; no
manual coordinate-drag fallback is used. On a platform that ignores expanded
client-area hints, the application remains usable with its platform-provided
frame rather than emulating an incomplete one.

## Accessibility

The existing `QTabBar` retains its standard `PageTabList` and `PageTab` roles,
keyboard operation, stable accessible names, and close/reorder semantics. The
drag area does not accept keyboard focus. The Qt Windows caption buttons retain
their platform accessibility and system-command behavior.

## Verification

Focused tests will cover:

- the expanded client-area window flags;
- propagation and application of safe-area insets;
- title-row height and tab-width constraints;
- drag hit regions excluding tabs and New Tab;
- double-click maximize/restore behavior;
- preservation of tab activation, movement, closure, keyboard commands, and
  accessibility roles.

Host integration tests will continue to cover tab creation, switching,
reordering, closure, content lifetime, and session restoration. A real Windows
smoke check will exercise DPI scaling, Qt's caption controls, edge resize,
window dragging, drag-to-edge snapping, and maximize/restore.

Final verification runs the complete Qt/C++ test suite, the complete tools test
suite, diff checks, and the relevant Release build inventory check. The visual
change must not add test-only interfaces to production binaries.

## References

- [Qt window flags](https://doc.qt.io/qt-6/qt.html#WindowType-enum)
- [QWindow safe-area margins](https://doc.qt.io/qt-6/qwindow.html#safeAreaMargins-prop)
- [QWindow system move](https://doc.qt.io/qt-6/qwindow.html#startSystemMove)
