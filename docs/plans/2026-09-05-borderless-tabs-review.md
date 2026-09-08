# Borderless Tabs Review Follow-up

**Goal:** Finish the approved browser-style tab appearance and keep the title
strip usable when tabs overflow. The user requested direct implementation.

**Architecture:** Retain the existing QTabBar, stable tab IDs, command routing,
palette-based styling, and native expanded title area. Update BrowserChrome
layout/theme handling and the user-cancelled TabController lifecycle.

## Review findings and implementation

1. Remove the per-tab outline and close-button outline. Inactive tabs blend
   into the window-colored strip; the active tab uses the navigation background.
2. Replace the symmetric vertical margins with a top inset and zero bottom
   inset so the active tab joins the navigation row without a gap.
3. Check narrow and wide overflowing strips with both short and long titles.
   Preserve a usable empty title drag area and the 120-pixel minimum tab width;
   reserve the existing native caption inset throughout.
4. Retain native close-button painting: a stylesheet background replaces Qt's
   close glyph instead of simply decorating it.
5. Refresh the scoped stylesheet after application palette changes. Qt excludes
   stylesheet children from inherited palette propagation; queue the refresh
   using the chrome as its lifetime context.
6. Keep cancelled application tabs idle (Active/Background) instead of Dormant,
   which automatically starts the descriptor on tab activation. Explicit Reload
   or navigation still starts a new incarnation.

## Verification

- Extend the existing browser chrome layout/overflow tests, reproduce layout
  failures before fixes, and rerun after the implementation.
- Render the real Qt widgets in light/dark palettes for visual inspection.
- Build the host and run browser chrome, browser shell, model, and session
  tests; run the complete existing CTest/tools suites for project review.
- Independently review tab-state/session code and the final change. Reproduce
  and fix the Stop/activation bug found in that review.

## Scope

No changes to Worker authority, routing policy, WebEngine isolation, or session
format. Existing untracked audit and temporary directories remain user-owned.

## Evidence (2026-09-05)

- Layout RED: the active tab ended at y=39 while navigation began at y=42;
  all four overflow cases had a zero-width window drag area. GREEN: all six
  layout/margin/overflow cases pass.
- Stop RED: switching, reordering, and selecting the current tab each produced
  two launch requests instead of one. GREEN: all three cases retain the stopped
  incarnation until Reload.
- Theme RED: a nested chrome retained a white active-tab background after a
  dark palette update. GREEN: rendered background pixels follow both the dark
  palette and the restored original palette.
- Real Qt light/dark renders inspected; temporary screenshot instrumentation
  removed. Existing close buttons, stable IDs, and keyboard roles retained.
- Full Debug build passed. Tools suite: 9 files / 172 tests passed.
- Full CTest suite: 55/55 passed, zero failures (643.57 seconds), including
  browser chrome/shell, session restore, production updates, and all end-to-end
  cases. `git diff --check` passed.
- Independent static review found no blocking issues in the final changes.

Qt implementation reference for palette propagation:
[QWidget 6.11.1](https://raw.githubusercontent.com/qt/qtbase/v6.11.1/src/widgets/kernel/qwidget.cpp),
[QApplication 6.11.1](https://raw.githubusercontent.com/qt/qtbase/v6.11.1/src/widgets/kernel/qapplication.cpp).
