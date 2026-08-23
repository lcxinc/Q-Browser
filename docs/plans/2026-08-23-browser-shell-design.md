# Q-Browser Trusted Browser Shell Design

**Status:** Approved on 2026-08-23  
**Scope:** First browser-oriented increment after the accepted Windows MVP  
**Constraint:** Preserve the signed-package, LPAC Worker, capability-broker, and restricted-WebEngine trust model

## 1. Objective

Evolve the single-surface Host into a familiar desktop browser shell without
turning Q-Browser into an unrestricted Internet browser. The increment adds
independent tabs, per-tab navigation history and lifecycle, browser keyboard
commands, safe session restoration, and standard loading/crash feedback.

The user can keep App/QML, restricted WebEngine, and Host-owned pages open at
the same time. Each App tab remains isolated in its own verified LPAC Worker.
The Host still rejects arbitrary `http`, `https`, `file`, and unknown URLs.

## 2. Scope and non-goals

### 2.1 Required behavior

- A trusted two-row browser chrome with a tab strip and navigation toolbar.
- Up to 16 independently ordered tabs.
- Host New Tab, signed App, restricted Web, and trusted error tab states.
- Independent address, title, loading state, history, surface, and lifecycle per
  tab.
- New, close, reopen, activate, and drag-reorder tab operations.
- Back, forward, reload, stop, home, and address-entry operations.
- Standard keyboard commands for all essential browser operations.
- Atomic restoration of tab order, addresses, histories, and the active tab.
- Per-tab crash isolation and app-wide coordination of package rollback.
- Existing capability, gesture, file-dialog, sandbox, update, and deployment
  security guarantees across multiple tabs.

### 2.2 Explicit non-goals for this increment

- Arbitrary Internet browsing or search-engine fallback.
- Persistent cookies, Web cache, site permissions, or form data.
- Bookmarks, downloads, extensions, password management, developer tools, or
  multiple browser windows.
- Restoring live Worker identity, process state, capabilities, or Web renderer
  state across a Host restart.
- Browser-complete compatibility or a change to the MVP's allowed Web origin.

## 3. Trusted architecture

`MainWindow` remains a Qt Widgets trusted boundary and becomes the browser
window. It owns browser chrome plus one visible content stack; it never imports
or evaluates package QML.

The main components are:

- `BrowserTabModel`: value-oriented tab order and presentation state.
- `BrowserTab`: stable ID, type, logical URL, title, loading/error state,
  independent bounded history, and restoration descriptor.
- `TabController`: the only component allowed to create, activate, freeze,
  retire, or destroy a tab's security-sensitive resources.
- `BrowserAddress`: strict parser for Host-internal and registered application
  addresses.
- `BrowserSessionStore`: bounded, atomic Host-owned persistence.
- `AppRuntimeCoordinator`: application-level version activation, candidate
  health, and rollback coordination across App tabs.
- `BrowserChrome`: trusted tab strip, navigation toolbar, keyboard actions, and
  identity/loading indicators.

The content stack supports three resource-backed tab types:

1. **Host tab** — a trusted QWidget page such as `qbrowser://newtab` or a
   trusted error page.
2. **App tab** — one independently verified LPAC Worker, Job Object, IPC
   generation, Supervisor, capability runtime, and embedded `WorkerSurface`.
3. **Web tab** — one `QWebEnginePage` and `QWebEngineView`. All Web tabs share
   one Host-owned, session-only isolated `QWebEngineProfile` so same-session
   Web behavior is coherent without persisting cookies or permissions.

Multiple App tabs for the same app ID share only Host-namespaced app-private
storage and the application version coordinator. They do not share Worker
identity, page memory, request correlation, gesture grants, or navigation
history.

## 4. Tab lifecycle and resource bounds

The Host accepts no more than 16 open tabs. Restored inactive tabs begin as
validated dormant descriptors. The active tab is instantiated immediately;
other tabs allocate a Worker or Web surface only on first activation.

Within one Host run, switching away does not rebuild a healthy tab:

- Web tabs enter `QWebEnginePage::Frozen` when safe and return to `Active` when
  selected.
- App tabs hide their native surface and receive an inactive-visibility event.
  The Worker continues its minimal heartbeat and should stop presentation
  timers. Phase one does not suspend the OS process because doing so while a
  broker operation or native dialog is active would corrupt Supervisor timing.

A tab follows the state machine:

```text
Dormant -> Starting -> Loading -> Active <-> Background
                    \-> TrustedError
Active/Background -> Closing -> Retired
```

All transitions are idempotent. Closing first blocks new requests, revokes the
generation and gesture grants, cancels tab-owned work, retires the surface, and
only then removes the model entry. Bounded late cleanup belongs to the existing
Host retirement mechanism. Results from a retired generation are discarded.

## 5. Browser chrome and address behavior

The first row is a movable tab strip with New Tab, close buttons, tab title,
content identity icon, loading state, and crash state. The second row contains
Back, Forward, Reload/Stop, Home, the address field, and a content-identity
indicator.

New Tab and Home open `qbrowser://newtab`. The trusted New Tab page provides
recent App routes and fixed Pilot entry points without launching a Worker until
one is selected.

The address field recognizes only:

- `qbrowser://...` for a small allowlist of Host-owned pages;
- `app://pilot/...` for routes resolved through the existing registry.

Direct `http://`, `https://`, `file://`, data, script, or unknown schemes fail
closed into a Host error state. A Web route continues to expose its logical
`app://` address instead of the underlying local mock origin.

The chrome labels the active content as `Q-Browser`, `Signed application`, or
`Restricted web`. Titles and metadata are always untrusted length-bounded plain
text; the chrome never interprets them as rich text.

Each tab implements Back, Forward, Reload, and Stop according to its type.
Reload revalidates and reloads an App route or invokes WebEngine reload. Stop
cancels only the current navigation/load and does not close the tab.

Required keyboard commands are:

| Command | Action |
|---|---|
| `Ctrl+T` | New Tab |
| `Ctrl+W` | Close active tab |
| `Ctrl+Shift+T` | Reopen most recently closed tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Cycle tabs |
| `Ctrl+1` through `Ctrl+8`, `Ctrl+9` | Select numbered/last tab |
| `Ctrl+L` | Focus and select the address |
| `Alt+Left` / `Alt+Right` | Back / Forward |
| `Ctrl+R` or `F5` | Reload |
| `Esc` | Stop loading |

The recently closed stack is bounded to 16 descriptors. Reopening never reuses
an old Worker, request, nonce, generation, or capability grant.

Closing the final tab replaces it in place with a fresh trusted New Tab. This
keeps the trusted Host window available without preserving the closed tab's
runtime authority. The application exits only through an explicit window-close
operation; an empty or invalid restored session follows the same New Tab rule.

## 6. Navigation and metadata data flow

Browser chrome dispatches an action to the active `TabController`. The
controller parses and resolves the logical address before it mutates history or
creates a surface. Each history is capped at 256 entries; navigating after Back
removes the forward suffix.

Worker-initiated navigation remains authenticated by package ID and generation,
then passes through the same route registry before updating that tab's history.
Web redirects remain subject to the existing request interceptor and never
replace the logical registered route with an arbitrary address.

Web titles arrive through `QWebEnginePage::titleChanged`. App pages may publish
a new bounded `PageMetadata` protocol message containing only a plain-text
title and optional safe status. The Host validates message version, request
context, generation, character class, and maximum length before updating the
tab model.

## 7. Session persistence

Production package mode gains a required, protected browser-state directory. It
is a canonical, non-reparse Host-owned directory disjoint from the deployment,
package store, package sources, sandbox temp, capability storage, telemetry,
signing/trust material, and immutable runtime roots. It is external to the
published deployment.

`BrowserSessionStore` writes a versioned JSON document with `QSaveFile`. It
contains only:

- window geometry;
- active tab ID and tab order;
- tab ID, type, title, logical address, history, and history index.

It excludes credentials, user input, page/form state, request or response
bodies, selected file information, clipboard content, cookies, permissions,
cache, PIDs, nonces, generations, and grants. The store has strict per-string,
per-history, tab-count, and whole-file bounds.

Tab creation, close, reorder, navigation, and activation schedule a debounced
atomic save. Normal shutdown forces a final bounded flush.

Restore validates the schema and every address against current Host policy and
the current route registry. A missing, unsigned, removed, or newly denied route
becomes a trusted error descriptor. A corrupt document is never partially
trusted: the Host preserves at most one bounded `.corrupt` diagnostic copy and
starts a clean New Tab.

Restored App tabs reverify their package and negotiate a fresh Worker identity
when first activated. The Web profile is recreated empty on every Host start.

## 8. Multi-tab capability and gesture security

Each App tab retains a separate generation-bound `HostCapabilityRuntime` with
one in-flight request. A long operation in one tab cannot block another tab's
lane or its heartbeats.

Only input delivered while all of the following are true can create a clipboard
read grant:

- the Q-Browser window is foreground;
- the tab is active;
- the bound Worker HWND and PID match the tab;
- the generation is current.

Window deactivation, tab switching, close, replacement, or Worker retirement
revokes any unconsumed grant. Background tabs cannot consume another tab's
gesture.

A native file dialog is owned by its requesting tab. The request keeps only
that tab busy; other tabs remain usable. Closing the owner cancels its dialog
and request. The result is correlated to the original tab, request ID, and
generation even if the user switches tabs while the dialog is visible.

External navigation, downloads, popups, file selection, protocol handlers,
permissions, full-screen requests, screen capture, and certificate exceptions
remain denied for Web tabs. A non-modal Host notification explains the denial
without granting a bypass.

## 9. Crash, update, and rollback behavior

A Worker crash affects only its tab. The tab displays a recovering state,
allows one supervised restart under the existing health policy, and then shows
a trusted crash page after a repeated failure. Other tabs and the Host remain
alive.

A Web renderer failure replaces only its Web tab with a trusted error state;
Reload creates a healthy renderer path without changing other tabs.

`AppRuntimeCoordinator` prevents a multi-tab package version split:

- an already running tab is pinned to the verified version used at launch;
- newly created or reloaded tabs use the newly activated version;
- a candidate crash-loop atomically restores LKG;
- every tab using that failed candidate has its generation revoked and is
  relaunched from LKG;
- tabs for other applications are unchanged.

No tab can continue executing a candidate after its rollback authority has been
retired.

## 10. Testing strategy

Every production behavior begins with a focused failing test and follows the
existing TDD and systematic-debugging rules.

### 10.1 Unit tests

- `BrowserAddress` scheme/authority/path validation.
- Tab creation, activation, reorder, close, 16-tab bound, independent 256-entry
  histories, and recently closed behavior.
- Session schema, canonical serialization, atomic write, corruption, bounds,
  route revalidation, and sensitive-field exclusion.
- Metadata sanitization and browser action/shortcut mapping.

### 10.2 Host integration tests

- Host, App, and Web tab construction plus dormant lazy activation.
- Distinct App-tab Worker PIDs, generations, Jobs, capability lanes, and
  Supervisor state.
- Distinct Web pages/views with one ephemeral profile.
- State and history preservation through tab switching.
- Per-tab close/crash isolation.
- Active-tab gesture and file-result binding.
- Candidate rollback across affected same-app tabs without affecting other
  applications.

### 10.3 End-to-end and deployment tests

Real keyboard and mouse automation must:

1. create App, Web, and New Tab tabs through browser commands;
2. build different histories and prove Back/Forward isolation;
3. preserve two live App page states while switching;
4. terminate one Worker while the Host and another tab remain usable;
5. close and reopen a tab and observe a new PID/generation;
6. restart the Host and restore order, addresses, and histories while proving
   that Worker identity and clipboard grants were not restored;
7. reject a corrupt session into a clean trusted New Tab;
8. reject arbitrary HTTPS, downloads, popups, permissions, and external
   protocols;
9. repeat the critical path using only the published Release under minimal and
   polluted `PATH`.

## 11. Acceptance gates

The browser-shell increment is complete only when:

1. Existing MVP tests and security assertions remain green.
2. All new unit, integration, Quick/UI, security, and E2E tests pass with zero
   required skips.
3. Essential browser operations are fully keyboard operable and expose stable
   accessible names and roles.
4. Switching a healthy tab never recreates its live surface.
5. Startup instantiates only the restored active tab; inactive tabs remain
   dormant until selected.
6. Closing, crashing, or restoring one tab cannot reuse authority or mutate an
   unrelated tab.
7. Session persistence restores only approved display/navigation state and no
   credential, Web profile, or capability state.
8. Arbitrary Internet and file navigation remain denied.
9. The final clean Release retains `BUILD_TESTING=OFF`, contains no test hook or
   private key, and passes deployment E2E under both PATH environments.
10. The canonical deployment inventory has no reparse point and verifies
    without mutation on a no-clean run.
