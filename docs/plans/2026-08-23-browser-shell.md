# Trusted Browser Shell Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the accepted single-surface MVP Host with a familiar, accessible, independently isolated tabbed browser shell while continuing to reject arbitrary Internet/file navigation and preserving the signed-package, LPAC Worker, capability-broker, rollback, and restricted-WebEngine boundaries.

**Architecture:** Keep Qt Widgets as the trusted browser chrome. Separate pure tab/session state from resource-owning per-tab controllers; give every App tab a fresh Worker/session/capability authority, give every Web tab its own page/view over one shared off-the-record profile, and coordinate package version authority per application. Persist only bounded logical navigation state in a protected Host-owned directory outside the deployment.

**Tech Stack:** C++20, Qt 6.11 Core/Widgets/Network/WebEngine/Test, Windows LPAC/AppContainer and COM, CMake/CTest, Windows UI Automation, PowerShell release verification, Node/Vitest.

---

## Working rules

- Work from L:\project\Q-Browser\.worktrees\q-browser-mvp-impl on branch codex/q-browser-mvp-impl.
- Preserve the existing untracked .task12-npm-cache directory and unrelated user changes.
- Apply superpowers:test-driven-development for every behavior change: add the smallest focused RED, verify that it fails for the intended reason, implement only enough production code, then run the focused and related regression tests.
- Apply superpowers:systematic-debugging to any unexpected build/test/runtime failure. Do not increase watchdogs or retry until pass.
- Keep every production object free of test-only public behavior. Prefer real process, file, input, and corruption evidence in tests.
- Run LPAC/process E2E tests serially. Build with /m:1 for focused iterations unless a known-safe target is explicitly parallelized.
- Commit after each GREEN task. Do not stage .task12-npm-cache.
- Retain legacy active-tab accessors until the existing navigation/update suites have migrated; compatibility must report only the active tab and must not reintroduce global authority.

## Fixed bounds and invariants

- Maximum open tabs: 16.
- Maximum recently closed descriptors: 16.
- Maximum history entries per tab: 256.
- Maximum canonical address: 2048 UTF-8 bytes.
- Maximum title: 256 UTF-16 code units after control/bidi removal, without
  splitting a surrogate pair; lone surrogates are invalid.
- Maximum session file: 16 MiB, read as maximum + 1.
- Authority-drain soft deadline: 2,000 ms for the whole affected-token batch;
  timeout permanently fails that batch closed while cleanup drain continues.
- Maximum queued outbound IPC per session: 64 frames and 4 MiB including frame
  prefixes; exceeding either bound fails with stable ipc.send_queue_full.
- Only qbrowser://newtab and currently registered/authorized app://pilot/... routes are valid logical addresses.
- http, https, file, data, javascript, unknown schemes, userinfo, ports, fragments, and noncanonical encodings remain denied.
- A persisted tab ID is presentation state only. Runtime authority always includes a fresh, nonreused runtime incarnation plus tab ID, PID, HWND, local generation, and an admitted lease-authority epoch.
- Closing the final tab replaces it with a fresh trusted New Tab. Only explicit window close exits the application.

### Task 1: Re-establish the accepted baseline

**Files:** No source changes.

1. Inspect the exact baseline and dirty state.

       git rev-parse --short=12 HEAD
       git status --short

   Expected: 4e36487 is an ancestor of HEAD, the implementation diff base is
   4e36487, the approved design and this plan are tracked, and the only known
   untracked entry is .task12-npm-cache/.

       git merge-base --is-ancestor 4e36487 HEAD
       git diff --name-only 4e36487..HEAD

2. Configure the existing development tree.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022

3. Build and run the most coupled existing suites before changing ownership.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug --target tst_host_runtime_config tst_host_capability_runtime tst_unified_navigation tst_production_update_runtime tst_e2e_web_fallback tst_e2e_host_survives_worker_crash -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(host_runtime_config|host_capability_runtime|unified_navigation|production_update_runtime|e2e_web_fallback|e2e_host_survives_worker_crash)$' -j1

   Expected: all selected tests pass. Record any pre-existing failure before implementation; do not normalize it by loosening assertions.

### Task 2: Add strict browser-address parsing

**Files:**

- Create: runtime/router/BrowserAddress.h
- Create: runtime/router/BrowserAddress.cpp
- Create: tests/unit/router/tst_browser_address.cpp
- Modify: runtime/router/CMakeLists.txt
- Modify: tests/unit/router/CMakeLists.txt

1. Add a test target named browser_address and RED cases for:

   - exact qbrowser://newtab acceptance;
   - valid AppUrl reuse and canonical output;
   - qbrowser trailing slash, query, fragment, userinfo, port, encoded authority/path, and unknown Host page rejection;
   - http/https/file/data/javascript/unknown scheme rejection;
   - case and percent-encoding ambiguity rejection;
   - empty query, raw Unicode/control/space/quote/backslash, lowercase escape,
     encoded unreserved byte, invalid/overlong UTF-8, and bidi query rejection;
   - the 2048-byte bound;
   - syntax-valid but unregistered App paths remaining syntactically valid so RouteRegistry/current package authority can reject them later.

2. Build the test before adding production code.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_address -- /m:1 /nr:false

   Expected RED: missing BrowserAddress production contract, not a CMake discovery error.

3. Implement this value contract and reuse AppUrl/NormalizedPath for the
   application path, but add the browser-session canonical query rule:

   - source prefix is byte-for-byte lowercase app://pilot or
     qbrowser://newtab;
   - query is absent or nonempty ASCII using RFC 3986 query-allowed characters;
   - percent escapes use uppercase hex;
   - an unreserved ASCII octet must appear literally, never percent-encoded;
   - percent-encoded non-ASCII must form shortest-form valid UTF-8 and decode to
     non-control/non-bidi Unicode;
   - reserved encoded bytes remain encoded because they may differ semantically;
   - raw Unicode, whitespace, quote, backslash, empty trailing ?, and fragments
     are rejected;
   - canonical() is exactly lowercase scheme/authority + NormalizedPath encoded
     path + the validated query, and the accepted input must equal it
     byte-for-byte.

   Do not claim that AppUrl currently canonicalizes query text; it only supplies
   strict URL/path parsing and raw query extraction.

4. Implement this value contract:

       enum class BrowserAddressKind { Invalid, NewTab, App };
       enum class BrowserAddressError {
           None, InvalidUrl, WrongScheme, WrongAuthority,
           UnknownHostPage, AppUrlInvalid, TooLong
       };

       class BrowserAddress final {
       public:
           static BrowserAddress parse(QStringView input,
                                       QStringView appAuthority = u"pilot");
           bool isValid() const noexcept;
           BrowserAddressKind kind() const noexcept;
           BrowserAddressError error() const noexcept;
           QString canonical() const;
           QString appPath() const;
           QString appQuery() const;
       };

5. Keep parsing side-effect free. It must never start a Worker, resolve a package, or fall back to a search engine.

6. Relink and run focused and existing router tests.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_address tst_app_url tst_normalized_path tst_route_registry -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_address|app_url|normalized_path|route_registry)$'

   Expected GREEN: all selected tests pass.

7. Commit:

       git add runtime/router/BrowserAddress.h runtime/router/BrowserAddress.cpp runtime/router/CMakeLists.txt tests/unit/router/tst_browser_address.cpp tests/unit/router/CMakeLists.txt
       git commit -m "feat: add strict browser addresses"

### Task 3: Introduce the resource-free tab model

**Files:**

- Create: apps/host/BrowserTabModel.h
- Create: apps/host/BrowserTabModel.cpp
- Create: tests/unit/host/tst_browser_tab_model.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: tests/unit/host/CMakeLists.txt

1. Write RED tests for:

   - create, activate, move, close, and active-index selection;
   - the 17th tab failing without changing the model;
   - stable ID lookup after QTabBar-style reordering;
   - independent histories for two tabs;
   - duplicate-current suppression, Back/Forward, forward-suffix removal, and the 256-entry bound;
   - the 16-entry LIFO recently closed stack;
   - reopen creating a new tab ID and retaining only the descriptor/history;
   - title control/bidi removal, plain-text behavior, the 256 UTF-16-unit
     bound, surrogate-pair boundary, and lone-surrogate rejection;
   - nonpersistent loading/progress, Host/App/Web identity,
     recovering/crashed/error state, and accessible-tab presentation;
   - snapshots containing no PID, HWND, nonce, generation, request, capability, cookie, or grant fields.

2. Run the new target and confirm RED.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_tab_model -- /m:1 /nr:false

3. Implement a QObject plus QVector model, not QAbstractListModel. QTabBar is index-based UI; tabData will carry the stable ID.

       enum class BrowserTabKind { Host, App, Web, TrustedError };
       enum class BrowserTabLifecycle {
           Dormant, Starting, Loading, Active, Background,
           TrustedError, Closing, Retired
       };

       struct BrowserTabSnapshot {
           QString id;
           BrowserTabKind kind;
           QString title;
           QString address;
           QStringList history;
           int historyIndex = -1;
       };

       enum class BrowserContentIdentity {
           QBrowser, SignedApplication, RestrictedWeb
       };
       enum class BrowserVisualState {
           Normal, Recovering, Crashed, TrustedError
       };
       struct BrowserTabPresentation {
           bool loading = false;
           int progress = 0;
           BrowserContentIdentity contentIdentity;
           BrowserVisualState visualState;
       };

   Identity is derived only from Host-validated BrowserTabKind/route engine.
   Worker/Web metadata, titles, status strings, and error text can never set
   either enum. BrowserChrome maps enums to fixed Host text/icons.

4. Generate IDs as 32 lowercase hexadecimal UUID characters. Treat them as model identity only. Reopen must generate a new ID.

5. Expose exact-change signals:

       void tabInserted(int index, const QString &id);
       void tabRemoved(int index, const QString &id);
       void tabMoved(int from, int to);
       void tabChanged(int index);
       void activeTabChanged(int oldIndex, int newIndex);
       void persistenceNeeded();

6. Let the model reach zero tabs internally. MainWindow atomically replaces a
   user-closed final tab with a new ID/qbrowser://newtab descriptor; an explicit
   window close bypasses that replacement and shuts the Host down.

7. Define replaceFromValidatedSnapshot() so every restored tab begins Dormant.
   Even the active descriptor becomes active only when TabController starts it;
   lifecycle and presentation fields never come from JSON.

8. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_tab_model tst_unified_navigation -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_tab_model|unified_navigation)$'

9. Commit:

       git add apps/host/BrowserTabModel.h apps/host/BrowserTabModel.cpp apps/host/CMakeLists.txt tests/unit/host/tst_browser_tab_model.cpp tests/unit/host/CMakeLists.txt
       git commit -m "feat: add bounded browser tab model"

### Task 4: Require a protected external browser-state root

**Files:**

- Create: apps/host/HostOwnedStateDirectory.h
- Create: apps/host/HostOwnedStateDirectory.cpp
- Create: apps/host/HostOwnedFileAuthority.h
- Create: apps/host/HostOwnedFileAuthority.cpp
- Modify: apps/host/HostRuntimeConfig.h
- Modify: apps/host/HostRuntimeConfig.cpp
- Modify: apps/host/main.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: runtime/package/WindowsStableIo.h
- Modify: runtime/package/WindowsStableIo.cpp
- Modify: tests/unit/host/tst_host_runtime_config.cpp
- Modify: tests/e2e/TestEnvironment.h
- Modify: tests/e2e/TestEnvironment.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/integration/update/tst_production_update_runtime.cpp
- Modify: scripts/build-release.ps1
- Modify: docs/development/getting-started.md

1. Add RED config tests for required package-mode arguments:

       --deployment-root=<absolute protected published root>
       --browser-state-directory=<absolute protected external root>

   Require stable errors host.config.missing_deployment_root and host.config.missing_browser_state_directory.

2. Treat deployment root as a stable containment authority, not another
   writable peer in a generic pairwise-disjoint list. Add RED cases proving:

   - the actual QCoreApplication executable, configured Worker executable,
     every immutable runtime root, and deployed trusted public key are inside
     the held deployment root;
   - an install package is either under deployment/packages or represented by a
     held, restricted-ACL, stable external package-source file authority whose
     parent is disjoint from browser-state and every mutable runtime root;
   - a caller-supplied fake root is rejected;
   - package store, sandbox temp, telemetry, capability storage, and
     browser-state are outside deployment and pairwise disjoint;
   - browser-state equals, contains, or is contained by deployment is rejected;
   - the allowed containment of runtime/trust/package-source by deployment is
     not rejected as ordinary overlap.

3. Add RED cases rejecting a state root that equals, contains, or is contained by:

   - deployment root;
   - package store;
   - sandbox temp;
   - telemetry;
   - capability storage;
   - immutable runtime roots;
   - trusted-key parent;
   - install-package parent.

4. Add Windows RED cases for any reparse ancestor, fake deployment root,
   changed deployment/state identity, permissive owner/DACL, and a
   rename/replacement race. Assert parsing never changes ACLs.

5. Run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_host_runtime_config -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^host_runtime_config$'

   Expected RED: missing arguments and state-root trust are not implemented.

6. Extend WindowsStableDirectoryTree only with the minimum reusable directory-root identity/ACL verification needed by HostOwnedStateDirectory. Hold stable authorities for both the real deployment root and browser-state root. Do not expose archive-specific mutation APIs through the Host wrapper.

7. Implement HostOwnedStateDirectory as a move-only/shared immutable authority that:

   - holds the directory handle/tree for the lifetime of HostRuntimeConfig/BrowserSessionStore;
   - exposes canonicalPath() and revalidate();
   - proves final path, file identity, non-reparse ancestry, owner/DACL, and non-overlap;
   - never grants the LPAC SID access;
   - fails closed if identity or security changes.

   Implement HostOwnedFileAuthority with the same stable parent/file identity,
   one-link/non-reparse/restricted-ACL checks for an external install package.
   HostRuntimeConfig retains it until lifecycle installation has consumed and
   reverified the exact bytes.

8. Store shared stable deployment/state authorities in HostRuntimeConfig, not
   only QStrings. Bind deployment to the current Host executable and configured
   immutable contents. Keep trusted-shell mode free of package/state authority.

   Add a non-CLI HostRuntimeParseContext carrying a stable
   currentHostExecutable evidence object. main.cpp constructs it from the
   running image handle/path and passes it to fromArguments(); no command-line
   value can override it. Unit tests pass a stable fixture Host copied beneath
   their deployment root, while QProcess E2E proves the real qbrowser-host.exe
   binding. This seam is path/identity evidence, not a test bypass, and tests
   never need to rewrite the entire build-tree ACL.

9. Update every package-mode fixture. In tst_production_update_runtime.cpp,
   update both the central parseHostArguments helper and the real
   installArguments/offlineArguments process-launch path. Give TestEnvironment
   and ValidArguments separate deployment/state roots and apply the same
   protected owner/DACL setup used by production; a plain inherited
   QTemporaryDir is insufficient.

10. Immediately plumb the required arguments through build-release.ps1:

   - manual state creates browser-state;
   - Start-DeployedHost passes the real deployment root and external state root;
   - all Host restarts reuse that state root.

   Task 19 will add the full browser acceptance markers and attestation, but no
   intermediate commit may leave the real launch script unusable.

11. Run the config, Host navigation, and production update regressions.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_host_runtime_config tst_unified_navigation tst_production_update_runtime -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(host_runtime_config|unified_navigation|production_update_runtime)$' -j1

12. Commit:

       git add apps/host/HostOwnedStateDirectory.h apps/host/HostOwnedStateDirectory.cpp apps/host/HostOwnedFileAuthority.h apps/host/HostOwnedFileAuthority.cpp apps/host/HostRuntimeConfig.h apps/host/HostRuntimeConfig.cpp apps/host/main.cpp apps/host/CMakeLists.txt runtime/package/WindowsStableIo.h runtime/package/WindowsStableIo.cpp tests/unit/host/tst_host_runtime_config.cpp tests/e2e/TestEnvironment.h tests/e2e/TestEnvironment.cpp tests/integration/host/tst_unified_navigation.cpp tests/integration/update/tst_production_update_runtime.cpp scripts/build-release.ps1 docs/development/getting-started.md
       git commit -m "feat: require protected browser state root"

### Task 5: Add bounded atomic session persistence

**Files:**

- Create: apps/host/BrowserSessionStore.h
- Create: apps/host/BrowserSessionStore.cpp
- Create: tests/unit/host/tst_browser_session_store.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: tests/unit/host/CMakeLists.txt
- Modify: runtime/package/WindowsStableIo.h
- Modify: runtime/package/WindowsStableIo.cpp

1. Write RED tests for:

   - missing file;
   - canonical round trip of geometry, tab order, active ID, histories, and index;
   - exact schema/version/key matching;
   - atomic replacement failure preserving the previous good file;
   - browser-session.json and .corrupt reparse leaf, hardlink,
     non-regular leaf, identity replacement, and parent-replacement races;
   - either leaf having a permissive owner/DACL, an LPAC or untrusted allow ACE,
     or a DACL replacement race before read/after replace;
   - invalid JSON, oversized file, nonintegral numbers, invalid IDs/indices, 17 tabs, 257 history items, and overlong strings;
   - a runtime title being sanitized before save, but a loaded title that is
     not already in canonical sanitized form making the whole file Corrupt;
   - surrogate-pair boundary and lone-surrogate rejection;
   - any unknown/sensitive field rejecting the whole document;
   - one malformed tab rejecting the whole session instead of partial recovery;
   - one bounded .corrupt copy at most, never logged or parsed;
   - resolver reclassifying App/Web from current authority and turning removed, unsigned, or denied routes into TrustedError.

2. Confirm RED:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_session_store -- /m:1 /nr:false

3. Implement this contract:

       struct BrowserWindowSnapshot {
           QRect geometry;
           QString activeTabId;
           QVector<BrowserTabSnapshot> tabs;
       };

       enum class BrowserSessionLoadStatus {
           Missing, Loaded, Corrupt, IoFailure
       };

       using RestoredAddressResolver =
           std::function<std::optional<BrowserTabKind>(const BrowserAddress &)>;

       BrowserSessionLoadResult load();
       BrowserSessionResolveResult validateAndResolve(
           const BrowserWindowSnapshot &raw,
           const RestoredAddressResolver &resolver);

   load() performs bounded I/O, exact schema/canonical-text validation, and
   BrowserAddress syntax validation only. validateAndResolve() is the second
   phase used after lifecycle-thread package verification returns a current
   signed-package descriptor; BrowserSessionStore never calls package authority
   across threads.

4. Use this exact schema and fixed key set. Serialize compact JSON with keys in
   the shown order and one final newline:

       {
         "version": 1,
         "window": {"x": 0, "y": 0, "width": 1280, "height": 800},
         "activeTabId": "32-lowercase-hex",
         "tabs": [{
           "id": "32-lowercase-hex",
           "kind": "host|app|web|trusted-error",
           "title": "bounded canonical plain text",
           "address": "canonical logical address",
           "history": ["canonical logical address"],
           "historyIndex": 0
         }]
       }

   Require 1–16 tabs; activeTabId must identify one tab; IDs are unique;
   history has 1–256 entries; historyIndex is in range and its item equals
   address. x/y are integral in [-1000000, 1000000]; width/height are integral
   in [320, 32768]. kind is advisory and is recomputed by the resolver.

5. BrowserSessionStore must retain std::shared_ptr<const
   HostOwnedStateDirectory>. Use only fixed browser-session.json and
   browser-session.json.corrupt leaves; reject any caller-controlled leaf.

6. Before reading either leaf, require absent-or-regular, non-reparse, one-link
   identity plus the protected Host owner/DACL: no LPAC/untrusted allow ACE and
   no writable inherited permission outside the approved principals. On Windows,
   open through a stable file handle, verify final path/volume/identity/security
   remains inside the held root, and read maximum + 1 from that handle. Repeat
   the leaf security/identity check after open so a DACL replacement race fails
   closed. Never let QFile follow an unverified leaf.

7. Before QSaveFile, reject a dangerous existing target, revalidate the held
   root, and create the temporary leaf with an explicit restricted security
   descriptor rather than inherited defaults. After commit, stable-open the
   final leaf, verify identity/path/link count/owner/DACL, read back the exact
   bytes, and revalidate both leaf security and root. If QSaveFile cannot prove
   that permission chain through replacement, use the minimum WindowsStableIo
   restricted-create + atomic-replace primitive instead of weakening the RED.
   Create and revalidate the bounded .corrupt leaf under the same explicit
   security descriptor.

8. Read at most 16 MiB + 1. Validate QJson numeric values as exact bounded integers. Persisted kind is advisory; the resolver recomputes the current kind from BrowserAddress, RouteRegistry, and current signed package authority.

9. Keep debounce out of the store. save() and load() remain synchronous and
   deterministic; MainWindow will own a single-shot timer later. “Bounded final
   flush” means one bounded-size attempt with no retry loop; it does not claim a
   wall-clock bound for the operating-system flush primitive.

10. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_session_store tst_browser_tab_model tst_host_runtime_config -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_session_store|browser_tab_model|host_runtime_config)$'

11. Commit:

       git add apps/host/BrowserSessionStore.h apps/host/BrowserSessionStore.cpp apps/host/CMakeLists.txt tests/unit/host/tst_browser_session_store.cpp tests/unit/host/CMakeLists.txt runtime/package/WindowsStableIo.h runtime/package/WindowsStableIo.cpp
       git commit -m "feat: persist bounded browser sessions"

### Task 6: Build the trusted New Tab page

**Files:**

- Create: apps/host/NewTabPage.h
- Create: apps/host/NewTabPage.cpp
- Create: tests/unit/host/tst_new_tab_page.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: tests/unit/host/CMakeLists.txt

1. Add RED tests for fixed Pilot entries, validated recent-route order/deduplication/bounds, keyboard activation, stable object/accessibility names, and external/invalid-address exclusion.

2. Assert constructing NewTabPage creates neither QWebEnginePage nor Worker process/surface state.

3. Reconfigure/build and confirm RED because NewTabPage is missing:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_new_tab_page -- /m:1 /nr:false

4. Implement a Host-owned QWidget:

       struct NewTabEntry {
           QString title;
           QString address;
       };

       class NewTabPage final : public QWidget {
           Q_OBJECT
       public:
           explicit NewTabPage(QWidget *parent = nullptr);
           void setRecentRoutes(const QVector<NewTabEntry> &validatedRoutes);
       signals:
           void addressActivated(const QString &canonicalAddress);
       };

5. Use explicit fixed entry addresses; do not add a RouteRegistry enumeration API. Revalidate every click through BrowserAddress and the window resolver.

6. Force QLabel text to Qt::PlainText; QAbstractButton/QTabBar text is already
   rendered as ordinary text and must never be converted to rich text. Bound
   titles and provide deterministic focus order and accessible names.

7. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_new_tab_page -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^new_tab_page$'

8. Commit:

       git add apps/host/NewTabPage.h apps/host/NewTabPage.cpp apps/host/CMakeLists.txt tests/unit/host/tst_new_tab_page.cpp tests/unit/host/CMakeLists.txt
       git commit -m "feat: add trusted new tab page"

### Task 7: Add browser chrome and action mapping

**Files:**

- Create: apps/host/BrowserChrome.h
- Create: apps/host/BrowserChrome.cpp
- Create: apps/host/BrowserCommand.h
- Create: apps/host/BrowserCommand.cpp
- Create: tests/unit/host/tst_browser_chrome.cpp
- Modify: apps/host/NavigationBar.h
- Modify: apps/host/NavigationBar.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: tests/unit/host/CMakeLists.txt

1. Write RED tests for:

   - stable tab IDs in QTabBar::tabData through insert/move/close;
   - signal blocking during model-to-view synchronization;
   - Back/Forward enable state;
   - the one Reload/Stop button changing action with loading state;
   - Home, identity label, address submit, Ctrl+L focus/select-all;
   - Ctrl+T, Ctrl+W, Ctrl+Shift+T, Ctrl+Tab, Ctrl+Shift+Tab;
   - Ctrl+1 through Ctrl+8 and Ctrl+9;
   - Alt+Left/Right, Ctrl+R/F5, Esc;
   - stable object names, accessible names/descriptions, and plain-text tab/title display;
   - actual QAccessible roles PageTabList, PageTab, Button, EditableText, and
     StaticText, including per-tab loading/crash accessible names.

2. Run the test and verify RED because BrowserChrome does not exist.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_chrome tst_new_tab_page -- /m:1 /nr:false

3. Implement BrowserChrome as a presentation/event component only. It must not parse addresses, resolve routes, launch processes, or own security authority.

4. Use movable/closable QTabBar plus a New Tab button. Use QSignalBlocker while applying model state. Never infer identity from a mutable tab index.

5. Expand NavigationBar to Back, Forward, Reload/Stop, Home, content identity,
   and address. Remove the nonstandard Go button once Enter and the action
   contract are tested. Synchronize active loading/progress, identity,
   recover/crash state, Back/Forward, address, tab accessible name, and window
   title from BrowserTabPresentation.

6. Define one BrowserCommand enum and one exact chord mapping shared by QAction
   and the later foreign-HWND route. Register every QAction with
   BrowserChrome::addAction() or MainWindow::addAction(), set WindowShortcut,
   and ensure a chord has one command source.

7. Use QTabBar::setAccessibleTabName() and
   QAccessible::queryAccessibleInterface() in tests; checking only stored
   accessible-name strings is insufficient.

8. Run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_chrome tst_new_tab_page -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_chrome|new_tab_page)$'

9. Commit:

       git add apps/host/BrowserChrome.h apps/host/BrowserChrome.cpp apps/host/BrowserCommand.h apps/host/BrowserCommand.cpp apps/host/NavigationBar.h apps/host/NavigationBar.cpp apps/host/CMakeLists.txt tests/unit/host/tst_browser_chrome.cpp tests/unit/host/CMakeLists.txt
       git commit -m "feat: add accessible browser chrome"

### Task 8: Share one ephemeral Web profile across independent pages

**Files:**

- Create: runtime/webengine/WebSessionProfile.h
- Create: runtime/webengine/WebSessionProfile.cpp
- Create: tests/unit/webengine/tst_web_session_profile.cpp
- Modify: runtime/webengine/WebSurface.h
- Modify: runtime/webengine/WebSurface.cpp
- Modify: runtime/webengine/CMakeLists.txt
- Modify: tests/unit/webengine/CMakeLists.txt
- Modify: tests/unit/webengine/tst_request_interceptor.cpp
- Modify: tests/unit/webengine/tst_webengine_file_selection.cpp
- Modify: apps/host/MainWindow.h
- Modify: apps/host/MainWindow.cpp
- Modify: apps/host/HostApplication.cpp

1. Add RED tests proving:

   - two WebSurface instances have distinct page/view objects and the same profile;
   - same-origin localStorage is shared inside one Host session;
   - two same-origin pages keep independent DOM and sessionStorage state while
     switching away/back preserves each page's state;
   - a fresh WebSessionProfile starts empty;
   - NoCache, NoPersistentCookies, AskEveryTime, exact-origin interception, download cancellation, popup denial, permission denial, external protocol denial, and file selection denial remain active;
   - each page restricts its main frame to the physical entry for its registered logical route;
   - title/loading/progress/renderer-failure signals are per surface;
   - a missing, blank, or URL-shaped physical page title falls back to a fixed
     trusted “Restricted web” title and never leaks loopback URL text;
   - user Stop does not load a trusted error page;
   - a loading page hidden in background freezes later when
     recommendedStateChanged permits it;
   - background/active transitions preserve page state.

2. Run RED:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_web_session_profile tst_request_interceptor tst_webengine_file_selection -- /m:1 /nr:false

3. Make WebSessionProfile the sole owner of the unnamed off-the-record QWebEngineProfile and PilotRequestInterceptor. Centralize download cancellation and associate it with request->page().

4. Change WebSurface to accept the shared profile and its exact registered
   physical main-frame entry:

       WebSurface(WebSessionProfile &session,
                  QUrl registeredMainFrameEntry,
                  QWidget *parent = nullptr);

   It owns only its restricted QWebEnginePage/QWebEngineView and exposes
   reload(), stop(), setTabActive(bool), title, progress, and renderer-failure
   signals. acceptNavigationRequest permits only that main-frame entry and the
   built-in qrc:/web/error.html; subframes/resources still pass the exact-origin
   profile interceptor.

5. Preserve only logical app:// addresses in the tab model. Never surface the physical loopback URL in chrome, history, telemetry, or session JSON.

6. Drive load state from QWebEnginePage::loadingChanged and
   QWebEngineLoadingInfo::LoadStartedStatus, LoadStoppedStatus,
   LoadSucceededStatus, and LoadFailedStatus. Do not infer Stop from
   loadFinished(false), and guard old-load callbacks with a navigation
   incarnation.

7. On background transition, hide first. If Qt still recommends Active, listen
   to recommendedStateChanged and freeze when safe. On activation ignore/disconnect
   the background freeze path, set Active, then visible. Never use Discarded for
   normal switching.

8. In this task, migrate the existing single MainWindow WebSurface to a
   MainWindow-owned WebSessionProfile so the changed constructor compiles.
   Task 10 expands it to multiple surfaces. HostApplication startup validation
   inspects the window's session profile rather than window->webSurface().

9. Define explicit destruction order in MainWindow::~MainWindow()/shutdown():

   1. stop and detach every view;
   2. synchronously destroy every WebSurface/page;
   3. require WebSessionProfile's registered page count to be zero and make a
      nonzero count fail shutdown() in Release (not only Q_ASSERT);
   4. detach interceptor and destroy the profile.

   Qt has no public profile shutdown/wait API. Prove renderer cleanup only from
   the external post-Host E2E/Release process inventory, never by waiting for a
   per-surface renderer PID.

10. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_web_session_profile tst_request_interceptor tst_webengine_file_selection tst_unified_navigation tst_host_runtime_config tst_production_update_runtime tst_e2e_host_routes tst_e2e_web_fallback -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(web_session_profile|request_interceptor|webengine_file_selection|unified_navigation|host_runtime_config|production_update_runtime|e2e_host_routes|e2e_web_fallback)$' -j1

11. Commit:

       git add runtime/webengine/WebSessionProfile.h runtime/webengine/WebSessionProfile.cpp runtime/webengine/WebSurface.h runtime/webengine/WebSurface.cpp runtime/webengine/CMakeLists.txt tests/unit/webengine/tst_web_session_profile.cpp tests/unit/webengine/CMakeLists.txt tests/unit/webengine/tst_request_interceptor.cpp tests/unit/webengine/tst_webengine_file_selection.cpp apps/host/MainWindow.h apps/host/MainWindow.cpp apps/host/HostApplication.cpp
       git commit -m "refactor: share ephemeral web session profile"

### Task 9: Add bounded App page metadata to authenticated IPC

**Files:**

- Modify: runtime/ipc/ProtocolMessage.h
- Modify: runtime/ipc/ProtocolMessage.cpp
- Modify: runtime/ipc/IpcSession.h
- Modify: runtime/ipc/IpcSession.cpp
- Modify: apps/worker/RuntimeFacade.h
- Modify: apps/worker/RuntimeFacade.cpp
- Modify: apps/worker/WorkerApplication.h
- Modify: apps/worker/WorkerApplication.cpp
- Modify: apps/host/HostWorkerSessionController.h
- Modify: apps/host/HostWorkerSessionController.cpp
- Modify: packages/pilot/qml/PilotRouter.qml
- Modify: tests/unit/ipc/tst_protocol_message.cpp
- Modify: tests/integration/ipc/tst_ipc_session.cpp
- Modify: tests/integration/worker/tst_worker_handshake.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/quick/pilot/tst_OrdersPage.qml
- Modify: tests/security/tst_worker_api_surface.cpp

1. Add RED protocol cases for a new one-way PageMetadata type with exact keys
   title and optional status. Title follows the 256 UTF-16-unit canonical rule;
   status is an allowlisted token of at most 32 ASCII bytes. Reject request
   IDs, unknown keys, controls/bidi overrides, invalid JSON/surrogates, and
   overlong values.

2. Add RED role/state cases: only an authenticated Worker may send metadata;
   the Host may receive it only after Ready; metadata from a closed or
   superseded session is ignored. Assert no PageMetadata appears on the wire
   before Ready even if QML requests it during startup.

3. Add RuntimeFacade RED for:

       Q_INVOKABLE bool setPageMetadata(const QString &title,
                                        const QString &status = {});

   It must emit a bounded sanitized value, not accept HTML or transmit arbitrary objects.

   Extend worker_api_surface to approve only this exact new invokable and prove
   it exposes no dynamic object, generic message bridge, writable security
   property, or additional method.

4. Reconfigure and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_protocol_message tst_ipc_session tst_worker_handshake tst_unified_navigation qbrowser_quick_tests tst_worker_api_surface -- /m:1 /nr:false

5. Add ProtocolType::PageMetadata and ProtocolMessage::pageMetadata(). Keep protocol version 1 only if old peers already fail closed on unknown types and the compatibility tests prove the additive message is safe; otherwise bump the version and update both ends atomically.

6. RuntimeFacade may cache only the last bounded metadata value until
   WorkerApplication has successfully sent Ready. WorkerApplication then sends
   it after Ready in order. It must never let eager QML metadata break Launcher
   handshake state.

7. Make HostWorkerSessionController emit metadata only with its current session
   generation. Sanitization is repeated at the Host model boundary; Worker-side
   cleanup is not an authority check.

8. Have PilotRouter publish a fixed plain-text title when currentPageName
   changes, guarded for Quick-test runtime doubles. Do not expose
   customer/order data or form text. Update the PilotRouter Quick mock/test.

9. Relink all selected consumers and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_protocol_message tst_ipc_session tst_worker_handshake tst_unified_navigation qbrowser_quick_tests tst_worker_api_surface -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(protocol_message|ipc_session|worker_handshake|unified_navigation|quick_design|worker_api_surface)$' -j1

10. Commit:

       git add runtime/ipc/ProtocolMessage.h runtime/ipc/ProtocolMessage.cpp runtime/ipc/IpcSession.h runtime/ipc/IpcSession.cpp apps/worker/RuntimeFacade.h apps/worker/RuntimeFacade.cpp apps/worker/WorkerApplication.h apps/worker/WorkerApplication.cpp apps/host/HostWorkerSessionController.h apps/host/HostWorkerSessionController.cpp packages/pilot/qml/PilotRouter.qml tests/unit/ipc/tst_protocol_message.cpp tests/integration/ipc/tst_ipc_session.cpp tests/integration/worker/tst_worker_handshake.cpp tests/integration/host/tst_unified_navigation.cpp tests/quick/pilot/tst_OrdersPage.qml tests/security/tst_worker_api_surface.cpp
       git commit -m "feat: publish bounded app page metadata"

### Task 10: Migrate MainWindow to per-tab Host and Web controllers

**Files:**

- Create: apps/host/TabController.h
- Create: apps/host/TabController.cpp
- Create: tests/integration/host/tst_browser_shell.cpp
- Modify: apps/host/MainWindow.h
- Modify: apps/host/MainWindow.cpp
- Modify: apps/host/HostApplication.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: tests/integration/host/CMakeLists.txt
- Modify: tests/integration/host/tst_unified_navigation.cpp

1. Add browser_shell RED cases for:

   - startup with exactly one qbrowser://newtab Host tab and no Worker/Web page;
   - New Tab, activate, move, close, reopen, and last-tab replacement;
   - two Web tabs with distinct page/view and the shared profile;
   - switching preserving Web page instance, title, loading, and state;
   - independent histories and Back/Forward;
   - invalid/external input leaving logical address/history unchanged while showing a trusted plain-text error;
   - inactive restored descriptors remaining resource-free;
   - close order reaching Retired before model removal;
   - Home/Reload/Stop/Back/Forward/address submission executing against only
     the active stable tab ID;
   - abnormal renderer termination destroying only its old page/view,
     entering a Host TrustedError, and Reload creating a fresh page without
     changing a sibling Web tab.

2. Confirm RED:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_shell tst_unified_navigation tst_production_update_runtime tst_e2e_host_routes tst_e2e_web_fallback -- /m:1 /nr:false

3. Implement one TabController per BrowserTab ID. It alone may transition Dormant/Starting/Loading/Active/Background/TrustedError/Closing/Retired and own the tab's Host or Web resource.

4. MainWindow owns BrowserChrome, BrowserTabModel, one visible QStackedWidget, a WebSessionProfile that outlives every WebSurface, and a QHash keyed by stable tab ID. QStackedWidget indices are never security identities.

5. Route each chrome action to the active stable ID. The reentrancy-safe
   navigation transaction is:

   1. BrowserAddress parse;
   2. current RouteRegistry and package-authority resolution;
   3. compute and atomically commit logical address/history plus a fresh
      navigation incarnation;
   4. update model/chrome so observers see only committed state;
   5. start the controller resource transition;
   6. if start fails, keep the validated logical address but transition that
      incarnation to TrustedError—never restore a half-old state;
   7. emit persistenceNeeded.

   Invalid/external input fails before step 3 and changes no address/history.

6. Keep currentAppUrl(), historyCount(), historyIndex(), activeSurface(), webSurface(), and workerSurface() temporarily as active-tab views so existing tests can migrate incrementally.

7. Support Host and Web resources now. Keep the existing package Host runnable
   through an explicit single-tab legacy App adapter that attaches the old
   singleton Worker to one owning tab and safely rejects a second App resource.
   Run production_update_runtime at every intermediate task. Task 15 removes
   this adapter; do not pretend it provides multi-App isolation.

8. Ensure Web/title/error signals capture stable tab ID and a controller
   incarnation. Abnormal renderProcessTerminated retires/destroys the old
   WebSurface and shows a Host-owned error; Reload constructs a new surface.
   A late signal from a retired surface is discarded.

9. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_shell tst_browser_chrome tst_browser_tab_model tst_web_session_profile tst_unified_navigation tst_e2e_host_routes tst_e2e_web_fallback tst_production_update_runtime -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_shell|browser_chrome|browser_tab_model|web_session_profile|unified_navigation|e2e_host_routes|e2e_web_fallback)$' -j1
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^production_update_runtime$' -j1

10. Commit:

       git add apps/host/TabController.h apps/host/TabController.cpp apps/host/MainWindow.h apps/host/MainWindow.cpp apps/host/HostApplication.cpp apps/host/CMakeLists.txt tests/integration/host/tst_browser_shell.cpp tests/integration/host/CMakeLists.txt tests/integration/host/tst_unified_navigation.cpp
       git commit -m "feat: add host and web tab controllers"

### Task 11: Remove global routing from Worker sessions

**Files:**

- Modify: apps/host/HostWorkerSessionController.h
- Modify: apps/host/HostWorkerSessionController.cpp
- Modify: apps/host/HostWorkerSessionIo.h
- Modify: apps/host/HostWorkerSessionIo.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/integration/ipc/tst_ipc_session.cpp

1. Add RED integration cases:

   - sameAppSessionsRouteOnlyTheirOwnTab;
   - backgroundWorkerNavigationUpdatesOnlyOwningHistory;
   - retiredSessionRejectsLateTabNavigation;
   - repeated request IDs in two sessions do not correlate across sessions;
   - PageMetadata from one session updates only its owning tab.

2. Confirm that the same-app test currently fails because every controller observes MainWindow::workerRouteRequested.

3. Replace the MainWindow global signal subscription with an injected callback:

       using NavigationCallback =
           std::function<bool(const QString &appId, const QString &route)>;

       explicit HostWorkerSessionController(NavigationCallback navigate,
                                            QObject *parent = nullptr);
       bool requestRouteLoad(const QString &route);
       quint64 generation() const noexcept;

4. Make all outbound/inbound callbacks carry the controller generation. AppTabRuntimeController will later bind the callback to one tab ID.

5. Preserve heartbeat polling while route/capability work is pending. Retiring one controller must not close another controller's IpcSession.

6. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_unified_navigation tst_ipc_session tst_worker_handshake tst_production_update_runtime -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(unified_navigation|ipc_session|worker_handshake)$' -j1
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^production_update_runtime$' -j1

7. Commit:

       git add apps/host/HostWorkerSessionController.h apps/host/HostWorkerSessionController.cpp apps/host/HostWorkerSessionIo.h apps/host/HostWorkerSessionIo.cpp tests/integration/host/tst_unified_navigation.cpp tests/integration/ipc/tst_ipc_session.cpp
       git commit -m "refactor: bind worker navigation to one tab"

### Task 12: Establish strong per-tab capability and gesture authority

**Files:**

- Create: apps/host/TabCapabilityAuthority.h
- Create: apps/host/HostGestureRouter.h
- Create: apps/host/HostGestureRouter.cpp
- Create: runtime/package/AuthorityAdmissionToken.h
- Create: runtime/package/AuthorityAdmissionToken.cpp
- Modify: apps/host/HostCapabilityRuntime.h
- Modify: apps/host/HostCapabilityRuntime.cpp
- Modify: apps/host/HostApplication.h
- Modify: apps/host/HostApplication.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: runtime/broker/UserGestureGrantStore.h
- Modify: runtime/broker/UserGestureGrantStore.cpp
- Modify: runtime/package/CMakeLists.txt
- Modify: tests/unit/host/tst_host_capability_runtime.cpp
- Modify: tests/unit/broker/tst_capability_broker.cpp

1. Add RED tests proving:

   - two clipboard-enabled capability runtimes can coexist;
   - only the active binding can create and consume a grant;
   - A input never authorizes B, even with equal request IDs;
   - tab switch, Host deactivation, close, PID/HWND/generation change, replacement, and retirement revoke unconsumed evidence;
   - background focus stealing cannot authorize;
   - dispatch revalidates authority instead of trusting only hook-time state;
   - one valid consumption succeeds and replay fails;
   - a browser Ctrl+Tab shortcut is not recorded as a Worker gesture;
   - an old UseGuard cannot publish after beginRevoke, while its
     RevocationTicket remains pending until the guard is released;
   - retiring one runtime leaves a sibling's gesture session, storage lane,
     and completion delivery active.

2. Confirm the existing second-runtime RED fails at TrustedWorkerInputObserver::active_.

3. Introduce a strong immutable authority:

       struct TabCapabilityAuthority final {
           QString tabId;
           quint64 runtimeIncarnation;
           QString appIdentity;
           quint32 workerProcessId;
           quintptr workerWindowId;
           quint64 sessionGeneration;
           quint64 leaseAuthorityEpoch;
       };

       class AuthorityAdmissionToken final {
       public:
           class UseGuard {
           public:
               bool publishIfStillAdmitted(
                   const std::function<bool()> &nonBlockingCommit);
           };
           class RevocationTicket {
           public:
               bool waitUntil(qint64 monotonicDeadlineMs) const;
               void waitUntilDrained() const;
           };
           std::optional<UseGuard> tryAcquireUse();
           RevocationTicket beginRevoke();
       private:
           friend class AppRuntimeCoordinator;
           // publication mutex + closed flag + active-use count + condition variable
       };

   Define AuthorityAdmissionToken in runtime/package, below both Host and
   AppRuntimeCoordinator in the dependency graph. TabCapabilityAuthority only
   refers to it; runtime/package never includes an apps/host header.

4. Move the single low-level keyboard/mouse observer out of each HostCapabilityRuntime into one HostGestureRouter. Register all Worker bindings but maintain exactly one active binding.

5. HostCapabilityRuntime and HostGestureRouter hold a nonreusable shared
   AuthorityAdmissionToken issued with the exact immutable authority tuple.
   tryAcquireUse() linearizes “still admitted” with incrementing an active-use
   count; UseGuard decrements it. A UseGuard exposes
   publishIfStillAdmitted(nonBlockingCommit): under the token's publication
   mutex it rechecks the closed bit and performs only the final in-memory attach
   or owned-buffer IPC submission. The closure may not block, yield, invoke user
   code, or call lifecycle. beginRevoke() closes both new use and publication,
   then returns a RevocationTicket carrying the active-use drain state; it never
   waits for an arbitrary UseGuard body. An atomic bool check alone is forbidden
   because it leaves a check/enqueue TOCTOU. Tests use a real token, not a
   callback into a fake coordinator.

   RevocationTicket waiting is forbidden on GUI and lifecycle threads. A guard
   acquired before beginRevoke but paused before publishIfStillAdmitted must fail
   publication after revocation, including after a drain deadline expires.

6. On a switch, clear old evidence before binding the new tab. At both
   observation and dispatch, verify foreground Host root, active tab, PID,
   HWND, runtime incarnation, session generation, and an admitted
   lease-authority UseGuard and publishIfStillAdmitted gate. GUI code never reads the
   lifecycle coordinator's containers or makes a blocking cross-thread call.

7. Change HostCapabilityRuntime to hold its Host-issued authority. Incoming requests may not supply or override any authority field. Completion returns the same authority plus request ID to its owning controller.

8. Let HostGestureRouter use the Task 7 BrowserCommand mapping when the active
   foreign Worker HWND owns focus. It recognizes the browser chord before
   gesture recording, suppresses matching keydown and keyup from the Worker,
   and queues exactly one command to the GUI. Deduplicate against QAction so
   one physical chord cannot execute twice. Normal QWidget/QWebEngine focus
   continues through QAction.

9. Run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_host_capability_runtime tst_capability_broker tst_capability_escape tst_production_update_runtime -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(host_capability_runtime|capability_broker|capability_escape)$' -j1
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^production_update_runtime$' -j1

10. Commit:

       git add apps/host/TabCapabilityAuthority.h apps/host/HostGestureRouter.h apps/host/HostGestureRouter.cpp runtime/package/AuthorityAdmissionToken.h runtime/package/AuthorityAdmissionToken.cpp runtime/package/CMakeLists.txt apps/host/HostCapabilityRuntime.h apps/host/HostCapabilityRuntime.cpp apps/host/HostApplication.h apps/host/HostApplication.cpp apps/host/CMakeLists.txt runtime/broker/UserGestureGrantStore.h runtime/broker/UserGestureGrantStore.cpp tests/unit/host/tst_host_capability_runtime.cpp tests/unit/broker/tst_capability_broker.cpp
       git commit -m "refactor: bind capabilities to active tab authority"

### Task 13: Add immutable verified package leases

**Files:**

- Create: runtime/package/WorkerLaunchRequest.h
- Modify: runtime/package/PackageInstaller.h
- Modify: runtime/package/PackageInstaller.cpp
- Modify: runtime/package/UpdateLifecycleCoordinator.h
- Modify: runtime/package/UpdateLifecycleCoordinator.cpp
- Modify: apps/host/RuntimePackageAuthority.h
- Modify: apps/host/RuntimePackageAuthority.cpp
- Modify: apps/host/InstalledPackageWorkerLauncher.h
- Modify: apps/host/InstalledPackageWorkerLauncher.cpp
- Modify: apps/host/InstalledPackageWorkerLauncherTestHooks.h
- Modify: apps/host/InstalledPackageWorkerLauncherTestHooks.cpp
- Modify: apps/host/HostApplication.h
- Modify: apps/host/HostApplication.cpp
- Modify: tests/integration/package/tst_package_installer.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/integration/update/tst_production_update_runtime.cpp

1. Add RED cases:

   - reverifyPinnedSignedVersionAfterCurrentChanges;
   - rejectTamperedPinnedVersion;
   - rejectPinnedPathOrDigestMismatch;
   - launcher revalidates the exact lease before launch and again after handshake.

2. Define an immutable verified lease:

       struct VerifiedPackageLease final {
           QString appId;
           QString version;
           QString versionDirectory;
           QString packageDirectory;
           QString entryPoint;
           ManifestPermissions permissions;
           QByteArray digestHex;
           qint64 activationGenerationAtIssue;
           quint64 leaseAuthorityEpoch;
       };

3. Define WorkerLaunchRequest in a package-level header so both legacy and new
   coordinators can call the existing Launcher:

       enum class PackageRevalidationMode {
           CurrentActivation, PinnedLease
       };

       struct WorkerLaunchRequest final {
           QString tabId;
           quint64 runtimeIncarnation;
           QString route;
           VerifiedPackageLease lease;
           std::shared_ptr<AuthorityAdmissionToken> admission;
           WorkerAttemptKey attempt;
           PackageRevalidationMode revalidationMode;
           bool recovery = false;
       };

   Replace UpdateLaunchRequest at the Launcher boundary. Update
   BindingValidator, matching/admission helpers, and test hooks to receive the
   complete request/lease rather than only appId + ActivationBinding.

4. Add two explicit revalidation modes:

   - CurrentActivation: retain current double activation-binding verification;
   - PinnedLease: reverify signature, digest, manifest, entry point, canonical paths, and immutable files for the exact leased version without requiring it to remain activation.json current.

5. PinnedLease authorization is not permanent. The lifecycle coordinator must
   separately admit tabId + runtime incarnation + leaseAuthorityEpoch before
   attach. After handshake revalidation, InstalledPackageWorkerLauncher must
   acquire AuthorityAdmissionToken::UseGuard, recompare the complete launch
   authority, and call publishIfStillAdmitted for the final nonblocking
   surface/session attach commit.
   Revocation behavior is tested in Task 14, after the coordinator exists.

6. Keep one InstalledPackageWorkerLauncher instance per App tab. Do not turn its currentProcess_/pendingRequest_ fields into an unbounded process pool.

7. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_package_installer tst_unified_navigation tst_production_update_runtime tst_malicious_package -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(package_installer|unified_navigation|production_update_runtime|malicious_package)$' -j1

8. Commit:

       git add runtime/package/WorkerLaunchRequest.h runtime/package/PackageInstaller.h runtime/package/PackageInstaller.cpp runtime/package/UpdateLifecycleCoordinator.h runtime/package/UpdateLifecycleCoordinator.cpp apps/host/RuntimePackageAuthority.h apps/host/RuntimePackageAuthority.cpp apps/host/InstalledPackageWorkerLauncher.h apps/host/InstalledPackageWorkerLauncher.cpp apps/host/InstalledPackageWorkerLauncherTestHooks.h apps/host/InstalledPackageWorkerLauncherTestHooks.cpp apps/host/HostApplication.h apps/host/HostApplication.cpp tests/integration/package/tst_package_installer.cpp tests/integration/host/tst_unified_navigation.cpp tests/integration/update/tst_production_update_runtime.cpp
       git commit -m "feat: add verified package leases"

### Task 14: Separate per-application version authority from per-tab supervision

**Files:**

- Create: runtime/package/AppRuntimeCoordinator.h
- Create: runtime/package/AppRuntimeCoordinator.cpp
- Create: tests/integration/update/tst_app_runtime_coordinator.cpp
- Modify: runtime/package/CMakeLists.txt
- Modify: runtime/package/PackageStore.h
- Modify: tests/integration/update/CMakeLists.txt

1. Write pure-state-machine RED cases:

   - newTabsUseCandidateWhileExistingTabsRemainPinned;
   - pinnedOldTabRestartsItsPinnedVersion;
   - candidateTabsHaveIndependentAttempts;
   - firstHealthyCandidateMarksLkgExactlyOnce;
   - lkgGenerationChangeDoesNotPoisonSiblingAdmission;
   - revokedLeaseCannotPassAdmission;
   - candidateCrashLoopRollsBackOnce;
   - rollbackRevokesEveryFailedCandidateLease;
   - multiTabRollbackReturnsEveryActionInStableRevokeStopRecoverOrder;
   - candidateRollbackDoesNotRevokePinnedOldVersionTab;
   - pinned old-version heartbeat/capability authority remains admitted after rollback;
   - failed-candidate late capability/result is rejected;
   - rollback racing an async capability completion closes every affected token
     before scheduling any drain, so the completion is always discarded;
   - all affected tokens close before any wait begins, and one shared monotonic
     deadline bounds the batch rather than multiplying a timeout by 16 tabs;
   - a UseGuard paused before final publication, then resumed after the drain
     deadline, still cannot attach/enqueue because publishIfStillAdmitted fails;
   - lifecycle continues processing a pinned sibling's heartbeat/admission while
     the failed-candidate drain waits on the authority-drain executor;
   - drain timeout emits ordered FailedClosed/Stop/Isolate actions, never
     recovery/launch; the background drain remains pending until every guard is
     released, and its eventual completion performs cleanup only;
   - after first healthy promotion, a sibling late failure follows normal
     per-tab restart/crash behavior and does not roll back the promoted LKG;
   - lateCandidateEventsAreIgnored;
   - rollbackDoesNotAffectAnotherApp;
   - activeNewTabWithInstallPackageStartsZeroWorkers;
   - firstAppActivationUsesTheInstalledCurrentLease;
   - userReloadUsesCurrentCandidateWhileCrashRestartUsesPinnedLease;
   - shutdown retires every admitted tab and rejects new events;
   - clean exit, startup/admission failure, crash exit, and fatal cleanup
     failure remain distinct and preserve existing failed-closed semantics;
   - healthy/rollback telemetry is recorded once per app transition and contains
     no tab ID, PID, route parameters, or user data.

2. Confirm RED:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_app_runtime_coordinator -- /m:1 /nr:false

3. Implement one AppRuntimeCoordinator per app ID on the lifecycle thread. It
   owns current/LKG/candidate descriptors, one WorkerSupervisor per tab runtime,
   a monotonic lease-epoch issuer, an explicit active/revoked lease set with one
   shared AuthorityAdmissionToken per exact tuple, and one-shot rollback state.
   Revocation is a two-phase batch:

   1. on lifecycle, collect the complete affected set and call beginRevoke() on
      every token before scheduling a wait or emitting recovery/launch;
   2. emit one AwaitAuthorityDrain action containing all RevocationTickets and
      one shared monotonic deadline; a dedicated executor waits off lifecycle and
      posts timeout/completion events back to the lifecycle queue.

   Lifecycle never waits on an active-use condition variable, so unrelated tab
   heartbeat/admission/exit events continue. A before-deadline completion emits
   deterministic ordered Revoke/Stop/RecoverFromLkg actions. Timeout emits
   FailedClosed/Stop/IsolateSession, leaves the drain running, and permanently
   forbids recovery/launch for that batch; eventual drain completion only permits
   cleanup. No timeout path reopens a token.

   GUI/input/launcher/IO-send/async-completion code reads no coordinator
   container. It acquires a short-lived UseGuard directly from its token, then
   uses publishIfStillAdmitted for only the final full-authority comparison plus
   attach commit or nonblocking owned-buffer IPC submission. Never hold a UseGuard
   across a blocking lifecycle query.

   Extend PackageStore's narrow friend list for AppRuntimeCoordinator rather
   than making activation/rollback internals public.

4. Define this minimum public contract and keep every coordinator method on the
   lifecycle thread. RevocationTicket waiting belongs to the injected Host drain
   executor, never these methods:

       struct TabLaunchAuthority final {
           QString tabId;
           quint64 runtimeIncarnation;
       };

       enum class TabLaunchIntent {
           ActivateCurrent, ReloadCurrent, RestartPinned
       };

       struct FullAttemptKey final {
           TabLaunchAuthority tab;
           WorkerAttemptKey attempt;
           quint64 leaseAuthorityEpoch;
       };

       struct AuthorityDrainBatch final {
           quint64 id;
           QVector<AuthorityAdmissionToken::RevocationTicket> tickets;
           qint64 monotonicDeadlineMs;
       };

       enum class AppRuntimeActionKind {
           None, Launch, Stop, Revoke, AwaitAuthorityDrain, IsolateSession,
           RecoverFromLkg, TrustedCrash, FailedClosed
       };

       struct AppRuntimeAction {
           AppRuntimeActionKind kind;
           QString tabId;
           quint64 runtimeIncarnation;
           std::optional<WorkerLaunchRequest> launch;
           std::optional<AuthorityDrainBatch> drain;
       };

       enum class AppRuntimeResultCode {
           Applied, IgnoredStale, Rejected, FailedClosed
       };

       struct AppRuntimeResult final {
           AppRuntimeResultCode code;
           QString stableError;
           QVector<AppRuntimeAction> actions;
       };

       AppRuntimeResult installAndActivate(const QString &packagePath,
                                           qint64 nowMs);
       AppRuntimeResult startOffline(qint64 nowMs);
       AppRuntimeResult requestTabLaunch(const TabLaunchAuthority &tab,
                                         const QString &route,
                                         TabLaunchIntent intent,
                                         qint64 nowMs);
       AppRuntimeResult admitAuthenticatedWorker(const FullAttemptKey &key,
                                                  const VerifiedPackageLease &lease);
       AppRuntimeResult heartbeat(const FullAttemptKey &key,
                                  qint64 receivedMonotonicMs);
       AppRuntimeResult checkHealth(qint64 nowMs);
       AppRuntimeResult workerExited(const FullAttemptKey &key,
                                     WorkerExitReason reason,
                                     qint64 nowMs);
       AppRuntimeResult workerAdmissionFailed(const FullAttemptKey &key,
                                              const QString &stableError,
                                              qint64 nowMs);
       AppRuntimeResult workerCleanupFailed(const FullAttemptKey &key,
                                             const QString &stableError,
                                             qint64 nowMs);
       AppRuntimeResult authorityDrainTimedOut(quint64 batchId,
                                               qint64 nowMs);
       AppRuntimeResult authorityDrainCompleted(quint64 batchId,
                                                qint64 nowMs);
       AppRuntimeResult closeTab(const TabLaunchAuthority &tab);
       AppRuntimeResult beginShutdown();

   installAndActivate() stages/verifies/activates but never launches a Worker.
   startOffline() verifies/describes current/LKG but never launches. A user
   activation/reload requests the current candidate; a supervised crash restart
   uses the tab's pinned lease. AppRuntimeResult::actions is an ordered vector,
   because one app rollback can revoke/stop/recover several tab runtimes. Stable
   tab-ID ordering plus Revoke-before-Stop-before-Recover/Launch is part of the
   tested contract; consumers never infer an omitted sibling action.

5. Define every event key as tab ID + runtime incarnation + WorkerAttemptKey +
   leaseAuthorityEpoch. WorkerAttemptKey alone is not unique across supervisors.

6. First sustained healthy candidate may promote the version once. Promotion
   generation changes must not make same-version sibling admissions stale.

7. Candidate crash-loop calls beginRevoke on all and only active leases issued
   for that failed candidate, restores LKG once, and emits explicit affected-tab
   recovery actions only after the whole batch drains before its shared deadline.
   Other apps and pinned old-version lease epochs remain admitted. A timeout
   fails the batch closed and never relaunches it, even when its eventual
   cleanup-only drain completion arrives.

8. Keep the production UpdateLifecycleCoordinator adapter unchanged in this
   task. The new coordinator is exercised with a deterministic fake
   AwaitAuthorityDrain consumer that posts timeout/completion callbacks without
   a Host dependency. Task 15 moves the legacy adapter only when the real Host
   drain executor and async action dispatch land in the same GREEN commit; no
   intermediate task may synchronously wait for a drain on lifecycle.

9. Relink every selected update executable and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_app_runtime_coordinator tst_update_lifecycle tst_crash_rollback tst_offline_lkg tst_production_update_runtime -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(app_runtime_coordinator|update_lifecycle|crash_rollback|offline_lkg|production_update_runtime)$' -j1

10. Commit:

       git add runtime/package/AppRuntimeCoordinator.h runtime/package/AppRuntimeCoordinator.cpp runtime/package/CMakeLists.txt runtime/package/PackageStore.h tests/integration/update/tst_app_runtime_coordinator.cpp tests/integration/update/CMakeLists.txt
       git commit -m "feat: coordinate package authority across tabs"

### Task 15: Give every App tab an independent runtime

**Files:**

- Create: apps/host/AppTabRuntimeController.h
- Create: apps/host/AppTabRuntimeController.cpp
- Modify: apps/host/TabController.h
- Modify: apps/host/TabController.cpp
- Modify: apps/host/MainWindow.h
- Modify: apps/host/MainWindow.cpp
- Modify: apps/host/HostApplication.h
- Modify: apps/host/HostApplication.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: apps/host/InstalledPackageWorkerLauncher.h
- Modify: apps/host/InstalledPackageWorkerLauncher.cpp
- Modify: apps/host/InstalledPackageWorkerLauncherTestHooks.h
- Modify: apps/host/InstalledPackageWorkerLauncherTestHooks.cpp
- Modify: apps/host/WorkerRetirementManager.h
- Modify: apps/host/WorkerRetirementManager.cpp
- Modify: runtime/ipc/ProtocolMessage.h
- Modify: runtime/ipc/ProtocolMessage.cpp
- Modify: runtime/ipc/IpcSession.h
- Modify: runtime/ipc/IpcSession.cpp
- Modify: runtime/package/AppRuntimeCoordinator.h
- Modify: runtime/package/AppRuntimeCoordinator.cpp
- Modify: runtime/package/UpdateLifecycleCoordinator.h
- Modify: runtime/package/UpdateLifecycleCoordinator.cpp
- Modify: apps/worker/RuntimeFacade.h
- Modify: apps/worker/RuntimeFacade.cpp
- Modify: apps/worker/WorkerApplication.h
- Modify: apps/worker/WorkerApplication.cpp
- Modify: tests/unit/ipc/tst_protocol_message.cpp
- Modify: tests/integration/ipc/tst_ipc_session.cpp
- Modify: tests/integration/update/tst_app_runtime_coordinator.cpp
- Modify: tests/integration/update/tst_update_lifecycle.cpp
- Modify: tests/integration/update/tst_crash_rollback.cpp
- Modify: tests/integration/update/tst_offline_lkg.cpp
- Modify: tests/integration/worker/tst_worker_handshake.cpp
- Modify: tests/security/tst_worker_api_surface.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/integration/update/tst_production_update_runtime.cpp
- Modify: tests/e2e/TestEnvironment.h
- Modify: tests/e2e/TestEnvironment.cpp
- Modify: tests/e2e/tst_host_survives_worker_crash.cpp

1. Add RED Host integration cases:

   - twoAppTabsOwnDistinctPidHwndAuthoritySessionProcessAndSurface;
   - switchingTabsDoesNotRestartHealthyWorker;
   - closingOneAppTabLeavesSiblingRunning;
   - reopeningTabCreatesFreshAuthority;
   - backgroundAppNavigationUpdatesOnlyItsHistory;
   - one pending capability lane does not block sibling heartbeat/request;
   - close discards late attach, metadata, navigation, capability, and exit events;
   - Host-to-Worker visibility accepts only the current authenticated
     generation and stale/retired messages fail closed;
   - 16 Workers cannot saturate lifecycle admission/exit/rollback events;
   - App Reload retires the old runtime, preserves tab/history, revalidates the
     current candidate, and creates a fresh PID/incarnation/authority tuple;
   - App Stop during Starting/Loading cancels launch/attach, invalidates that
     incarnation, discards late attach, leaves the tab reloadable, and is a
     no-op for an already Active/nonloading tab;
   - InstalledPackageWorkerLauncherTestHooks pauses after a UseGuard is acquired
     but before publishIfStillAdmitted; beginRevoke then makes the resumed attach
     fail without changing the owning controller/surface.

2. Add RED update cases:

   - sameAppCandidateCrashLoopRevokesAllCandidateTabs;
   - candidateRollbackRelaunchesAffectedTabsFromLkg;
   - oldVersionTabSurvivesCandidateActivation;
   - lateFailedCandidateAdmissionCannotAttach;
   - the legacy adapter returns AwaitAuthorityDrain without blocking, and maps
     completion to its historical healthy/restart/rollback outcome only after the
     injected executor posts the batch callback;
   - timeout maps to permanent FailedClosed and never to the old synchronous
     RolledBackAndLaunched result.

3. Build the changed tests and confirm the multi-App/visibility/queue cases are
   RED for the intended singleton/protocol reason:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_protocol_message tst_ipc_session tst_app_runtime_coordinator tst_update_lifecycle tst_crash_rollback tst_offline_lkg tst_worker_handshake tst_worker_api_surface tst_unified_navigation tst_production_update_runtime tst_e2e_host_survives_worker_crash -- /m:1 /nr:false

4. Before runtime integration, add authenticated Host-to-Worker
   VisibilityChanged(active: bool). Validate direction, role, state, exact
   payload, and generation; RuntimeFacade exposes a read-only active property so
   package presentation timers may pause. This message grants no authority and
   cannot stop heartbeat.

   Update worker_api_surface to permit only the read-only active property and
   notifier. There is no QML setter, dynamic object, generic state map, or
   additional bridge.

5. Implement AppTabRuntimeController as the strict owner of one:

   - InstalledPackageWorkerLauncher;
   - HostWorkerSessionController;
   - generation-bound HostCapabilityRuntime;
   - process lifetime/stop callback;
   - WorkerSurface;
   - current WorkerAttemptKey, VerifiedPackageLease, and runtime incarnation.

   The launcher performs its final complete-authority comparison and
   uses UseGuard::publishIfStillAdmitted for the nonblocking surface/session
   attach commit. If use acquisition or publication fails, it tears down the
   unaffiliated process/session and reports stale admission; a plain check before
   attach is forbidden. The deterministic hook pauses only before the publication
   gate, never inside its nonblocking critical section.

6. Give each controller a fresh SandboxTrustBoundary::create() over the same
   immutable approved roots; its shared internal state may be immutable, but a
   move-only boundary instance is never copied between controllers.

7. Use the nonblocking close order:

   1. stop admitting new requests;
   2. revoke gesture and capability authority;
   3. invalidate session generation;
   4. request bounded IPC shutdown;
   5. stop/retire the process and surface;
   6. detach the surface and submit remaining lifetime to the global
      WorkerRetirementManager asynchronously;
   7. remain Closing without blocking the GUI;
   8. on retirement completion, report Retired so the model entry can be removed.

   Only full Host shutdown performs a bounded global retirement drain.

8. Let TabController own AppTabRuntimeController through a HostApplication-supplied
   factory. HostApplication owns shared RuntimePackageAuthority, per-app
   coordinators, HostGestureRouter, the global retirement drain, and a dedicated
   authority-drain executor—not per-tab process fields. AwaitAuthorityDrain waits
   on that executor and posts only batch ID + timed-out/completed status back to
   lifecycle; it never occupies GUI or lifecycle.

   In this same task, reimplement UpdateLifecycleCoordinator as a reserved
   legacy-tab adapter over AppRuntimeCoordinator. Translate installAndLaunch /
   startOffline into install/resolve plus one explicit legacy requestTabLaunch,
   and consume AwaitAuthorityDrain through the same injected executor contract.
   Do not keep a second security state machine or synchronously manufacture the
   old RolledBackAndLaunched result before drain completion. Preserve
   LifecycleClock, EventRecorder, SafeEvent healthy/restarted/rollback behavior,
   and recordRouteLoadAcknowledged compatibility.

9. Replace HostApplication's singleton session/capability/process/key fields
   with tab-keyed ownership/weak observation. Local session generation numbers
   may both be 1; uniqueness is the full tab ID + runtime incarnation + local
   generation tuple. Do not expose a raw Job HANDLE solely for tests: prove
   isolation through two simultaneous Workers, per-launch activeProcessLimit=1,
   independent cleanup, and killing A without affecting B.

10. Batch only heartbeat/health observations before sending them to the
   lifecycle thread. Capture monotonic arrival time at receipt, key by full
   authority/attempt, keep the newest arrival per tab, and never merge/drop
   admission, exit, revoke, or rollback. Health deadlines use captured arrival
   time, not queue-consumption time. Test a 16-Worker saturation burst.

11. Preserve legacy test signals/accessors as active-tab compatibility views.
    Add tab-ID-bearing ready/exited signals and TestEnvironment maps for
    PID/HWND/local generation/runtime incarnation/lease epoch.

12. App backgrounding hides the surface and sends authenticated inactive
    visibility while heartbeat remains active. Do not OS-suspend Workers in this
    increment.

    Wire BrowserCommand Reload/Stop through MainWindow -> owning TabController
    -> AppTabRuntimeController. Reload keeps the stable tab/history but retires
    the old runtime and requests the current lease; Stop affects only
    Starting/Loading and advances the runtime incarnation before cancellation
    so late attach cannot succeed.

13. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_protocol_message tst_ipc_session tst_app_runtime_coordinator tst_update_lifecycle tst_crash_rollback tst_offline_lkg tst_worker_handshake tst_worker_api_surface tst_unified_navigation tst_production_update_runtime tst_e2e_host_survives_worker_crash tst_worker_crash -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(protocol_message|ipc_session|unified_navigation|app_runtime_coordinator|update_lifecycle|crash_rollback|offline_lkg|production_update_runtime|e2e_host_survives_worker_crash|worker_crash|worker_handshake|worker_api_surface)$' -j1

14. Commit:

       git add apps/host/AppTabRuntimeController.h apps/host/AppTabRuntimeController.cpp apps/host/TabController.h apps/host/TabController.cpp apps/host/MainWindow.h apps/host/MainWindow.cpp apps/host/HostApplication.h apps/host/HostApplication.cpp apps/host/CMakeLists.txt apps/host/InstalledPackageWorkerLauncher.h apps/host/InstalledPackageWorkerLauncher.cpp apps/host/InstalledPackageWorkerLauncherTestHooks.h apps/host/InstalledPackageWorkerLauncherTestHooks.cpp apps/host/WorkerRetirementManager.h apps/host/WorkerRetirementManager.cpp runtime/ipc/ProtocolMessage.h runtime/ipc/ProtocolMessage.cpp runtime/ipc/IpcSession.h runtime/ipc/IpcSession.cpp runtime/package/AppRuntimeCoordinator.h runtime/package/AppRuntimeCoordinator.cpp runtime/package/UpdateLifecycleCoordinator.h runtime/package/UpdateLifecycleCoordinator.cpp apps/worker/RuntimeFacade.h apps/worker/RuntimeFacade.cpp apps/worker/WorkerApplication.h apps/worker/WorkerApplication.cpp tests/unit/ipc/tst_protocol_message.cpp tests/integration/ipc/tst_ipc_session.cpp tests/integration/update/tst_app_runtime_coordinator.cpp tests/integration/update/tst_update_lifecycle.cpp tests/integration/update/tst_crash_rollback.cpp tests/integration/update/tst_offline_lkg.cpp tests/integration/worker/tst_worker_handshake.cpp tests/security/tst_worker_api_surface.cpp tests/integration/host/tst_unified_navigation.cpp tests/integration/update/tst_production_update_runtime.cpp tests/e2e/TestEnvironment.h tests/e2e/TestEnvironment.cpp tests/e2e/tst_host_survives_worker_crash.cpp
       git commit -m "feat: isolate app runtime per tab"

### Task 16: Bind native file work and all capability results to their owner tab

**Files:**

- Create: runtime/broker/FileDialogCoordinator.h
- Create: runtime/broker/FileDialogCoordinator.cpp
- Create: tests/integration/host/tst_browser_capability_isolation.cpp
- Modify: runtime/broker/FileBroker.h
- Modify: runtime/broker/FileBroker.cpp
- Modify: runtime/broker/FileDialogTestHooks.h
- Modify: runtime/broker/FileDialogTestHooks.cpp
- Modify: runtime/broker/CapabilityBroker.h
- Modify: runtime/broker/CapabilityBroker.cpp
- Modify: runtime/broker/StorageBroker.h
- Modify: runtime/broker/StorageBroker.cpp
- Modify: runtime/broker/StorageTestHooks.h
- Modify: runtime/broker/StorageTestHooks.cpp
- Modify: runtime/broker/CMakeLists.txt
- Modify: runtime/ipc/IpcSession.h
- Modify: runtime/ipc/IpcSession.cpp
- Modify: runtime/ipc/WinPipeTransport.h
- Modify: runtime/ipc/WinPipeTransport.cpp
- Modify: apps/host/HostApplication.h
- Modify: apps/host/HostApplication.cpp
- Modify: apps/host/HostCapabilityRuntime.h
- Modify: apps/host/HostCapabilityRuntime.cpp
- Modify: apps/host/HostWorkerSessionController.h
- Modify: apps/host/HostWorkerSessionController.cpp
- Modify: apps/host/HostWorkerSessionIo.h
- Modify: apps/host/HostWorkerSessionIo.cpp
- Modify: apps/host/HostWorkerSessionTestHooks.h
- Modify: apps/host/HostWorkerSessionTestHooks.cpp
- Modify: apps/host/AppTabRuntimeController.h
- Modify: apps/host/AppTabRuntimeController.cpp
- Modify: tests/unit/broker/tst_capability_broker.cpp
- Modify: tests/unit/host/tst_host_capability_runtime.cpp
- Modify: tests/integration/broker/tst_storage_broker.cpp
- Modify: tests/integration/ipc/tst_ipc_session.cpp
- Modify: tests/integration/host/CMakeLists.txt

1. Add RED tests proving:

   - a file dialog marks only its owner tab lane busy;
   - another tab's network/storage/clipboard request and heartbeat continue;
   - a second simultaneous dialog returns stable file.busy if the platform coordinator permits only one;
   - a background tab's new file request is denied before creating a dialog;
   - tab switching does not change result ownership;
   - closing the owner calls exact dialog cancellation;
   - success/cancel/failure returns only to the original tab/request/runtime incarnation/session generation;
   - owner retirement discards late completion;
   - a reopened tab cannot receive the closed tab's result;
   - two same-app capability runtimes initialize one storage root concurrently,
     then concurrent storage updates keep existing locking/atomicity guarantees;
   - dialog Show/read/encode/completion/cancellation never blocks the GUI event loop;
   - the maximum allowed selection can finish while GUI and every Worker
     heartbeat remain responsive;
   - rollback/close racing network, storage, clipboard, and file completion all
     pass through the same final authority/token gate and discard revoked work;
   - an IPC response queued before revocation but reaching the final send gate
     afterward is dropped because publishIfStillAdmitted refuses publication;
   - HostWorkerSessionTestHooks pauses after UseGuard acquisition but before the
     final send publication gate; revocation plus deadline may complete/timeout,
     yet resuming the old work still cannot submit an IPC write;
   - a stalled old operation does not prevent a sibling heartbeat or admission
     event from traversing the lifecycle queue;
   - async IPC submission returns promptly when the peer is not reading, owns its
     frame bytes until completion, preserves FIFO order per session, and reports
     success/failure/cancellation exactly once;
   - the 64-frame and 4-MiB boundaries accept exactly-at-limit work, reject the
     next frame with ipc.send_queue_full, and do not disturb accepted order;
   - an accepted Host send retains its UseGuard through transport completion or
     cancellation, so a revocation drain cannot report zero while bytes remain
     queued/in flight;
   - closing one blocked writer cancels/joins it without blocking GUI/lifecycle
     or delaying a sibling session's send/heartbeat.

2. Verify RED with:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_capability_isolation tst_capability_broker tst_host_capability_runtime tst_ipc_session -- /m:1 /nr:false

3. Keep existing capability services synchronous except for file selection.
   Add a two-phase CapabilityBroker contract:

       std::optional<PreparedFileRequest>
       prepareFileRequest(const EffectivePolicy &policy,
                          const QJsonObject &payload);

       BrokerResult completeFileRequest(const PreparedFileRequest &request,
                                        const FileDialogSelection &selection);

   HostCapabilityRuntime intercepts only prepared file.open, calls the injected
   FileDialogCoordinator::openAsync(token, callback), and leaves that tab's one
   request pending. It never blocks waiting for the callback. All policy,
   payload, selected-path, size/content, and final-result validation remains in
   CapabilityBroker/FileBroker.

4. Refactor the file backend to accept an opaque Host-issued operation token
   plus cancellation handle. Do not make runtime/broker depend on
   BrowserTabModel or string-concatenate security identity. Migrate/remove
   synchronous FileDialogTestHooks so test code cannot bypass the async
   authority path.

   HostApplication owns exactly one FileDialogCoordinator for the process,
   injects it into every AppTabRuntimeController/HostCapabilityRuntime, cancels
   all operations during shutdown, and destroys it only after all tab callbacks
   are quiescent. This single owner enforces cross-tab file.busy.

5. Run IFileOpenDialog on a dedicated STA. Before Show(), create a message-only
   window on that STA and retain the exact IFileOpenDialog instance. Cancellation
   uses PostMessage to that window; its STA window procedure calls
   Close(ERROR_CANCELLED) while the COM modal loop is pumping. Alternatively,
   use a tested GIT-marshaled proxy. A plain Qt queued call on the blocked
   Show() stack is not an acceptable implementation.

   Keep the selected stable handle/QIODevice on that STA. Read at most the
   approved maximum + 1, perform final identity/size validation and base64
   encoding there (or on a dedicated capability worker after explicitly moving
   owned bytes, never the QIODevice), close it, and send an owned BrokerResult
   value to GUI. GUI only enqueues a result carrying the complete immutable
   authority plus admission token; it does not make the decisive authority
   decision or enqueue bytes to IPC.

6. Do not use the entire browser window as a modal owner if that prevents
   sibling-tab operation. Use a logical owner/proxy arrangement and explicitly
   guard reentrant close/attach during COM callbacks.

7. HostCapabilityRuntime maps the operation token to TabCapabilityAuthority and
   request ID. The async callback queues to GUI. At actual IO dequeue/send,
   HostWorkerSessionIo acquires AuthorityAdmissionToken::UseGuard, performs the
   final full-authority comparison, then calls publishIfStillAdmitted to submit a
   nonblocking owned-buffer IPC write. That publication gate is the one decisive
   send authorization. The accepted send context retains the UseGuard until its
   completion/cancellation callback; work queued before revocation is dropped if
   either use or publication fails. Neither controller queries lifecycle state
   across threads, and no UseGuard spans a blocking call back to lifecycle. Test
   hooks pause only before publishIfStillAdmitted, never inside its nonblocking
   critical section.

   Add IpcSession::submitSend with an owned serialized frame and exactly-once
   completion. Refactor WinPipeTransport from caller-blocking writeAll into a
   bounded per-transport FIFO with one persistent ordered writer (or true Windows
   OVERLAPPED writes), owned buffers, bounded queue bytes, exact cancellation,
   and close-time completion. Submission itself only enqueues and returns; no
   GUI/lifecycle call waits for the peer. Existing synchronous Worker-side APIs
   may wrap submit + bounded wait only on their dedicated session IO thread, not
   on GUI/lifecycle. A Host pending-send context captures the UseGuard without
   making runtime/ipc depend on runtime/package. Enforce the fixed 64-frame / 4
   MiB per-session limits before accepting ownership; queue-full is a stable
   failure and never drops/reorders an already accepted frame.

   Apply this same final gate to every async network/storage/clipboard/file
   delivery, not only file.

   Serialize StorageBroker initialization for the same canonical app root with
   its stable inter/intra-process lock and revalidation before any DACL
   transaction; retirement of one broker cannot undo a sibling's root. Extend
   StorageTestHooks only for deterministic race orchestration and include its
   canary in Release scanning.

8. Relink and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_capability_isolation tst_capability_broker tst_host_capability_runtime tst_ipc_session tst_network_broker tst_storage_broker tst_e2e_pilot_capabilities -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_capability_isolation|capability_broker|host_capability_runtime|ipc_session|network_broker|storage_broker|e2e_pilot_capabilities)$' -j1

9. Commit:

       git add runtime/broker/FileDialogCoordinator.h runtime/broker/FileDialogCoordinator.cpp runtime/broker/FileBroker.h runtime/broker/FileBroker.cpp runtime/broker/FileDialogTestHooks.h runtime/broker/FileDialogTestHooks.cpp runtime/broker/CapabilityBroker.h runtime/broker/CapabilityBroker.cpp runtime/broker/StorageBroker.h runtime/broker/StorageBroker.cpp runtime/broker/StorageTestHooks.h runtime/broker/StorageTestHooks.cpp runtime/broker/CMakeLists.txt runtime/ipc/IpcSession.h runtime/ipc/IpcSession.cpp runtime/ipc/WinPipeTransport.h runtime/ipc/WinPipeTransport.cpp apps/host/HostApplication.h apps/host/HostApplication.cpp apps/host/HostCapabilityRuntime.h apps/host/HostCapabilityRuntime.cpp apps/host/HostWorkerSessionController.h apps/host/HostWorkerSessionController.cpp apps/host/HostWorkerSessionIo.h apps/host/HostWorkerSessionIo.cpp apps/host/HostWorkerSessionTestHooks.h apps/host/HostWorkerSessionTestHooks.cpp apps/host/AppTabRuntimeController.h apps/host/AppTabRuntimeController.cpp tests/unit/broker/tst_capability_broker.cpp tests/unit/host/tst_host_capability_runtime.cpp tests/integration/broker/tst_storage_broker.cpp tests/integration/ipc/tst_ipc_session.cpp tests/integration/host/tst_browser_capability_isolation.cpp tests/integration/host/CMakeLists.txt
       git commit -m "feat: isolate capability work by tab"

### Task 17: Wire atomic restore, lazy activation, and final save

**Files:**

- Create: apps/host/BrowserWindowGeometry.h
- Create: apps/host/BrowserWindowGeometry.cpp
- Modify: apps/host/CMakeLists.txt
- Modify: apps/host/MainWindow.h
- Modify: apps/host/MainWindow.cpp
- Modify: apps/host/HostApplication.h
- Modify: apps/host/HostApplication.cpp
- Modify: apps/host/BrowserSessionStore.h
- Modify: apps/host/BrowserSessionStore.cpp
- Modify: apps/host/TabController.h
- Modify: apps/host/TabController.cpp
- Modify: tests/unit/host/tst_browser_session_store.cpp
- Modify: tests/integration/host/tst_browser_shell.cpp
- Modify: tests/integration/host/tst_unified_navigation.cpp
- Modify: tests/integration/update/tst_production_update_runtime.cpp
- Modify: tests/e2e/TestEnvironment.h
- Modify: tests/e2e/TestEnvironment.cpp
- Modify: tests/e2e/tst_package_update.cpp

1. Add RED cases:

   - model mutations schedule one debounced save;
   - normal shutdown performs one bounded final flush;
   - restore preserves order, active ID, geometry, logical addresses, histories, and indices;
   - only the active descriptor starts; all other controllers remain Dormant;
   - active New Tab starts zero Workers/Web pages;
   - first activation of a dormant App revalidates the current signed package and creates fresh authority;
   - restored Web profile has no cookie/cache/permission state;
   - corrupt session creates exactly one clean New Tab and at most one bounded .corrupt;
   - removed/denied/unsigned routes become trusted error descriptors without partial trust;
   - startup/shutdown I/O failure remains fail-closed and does not overwrite a known-good session;
   - active New Tab plus --install-package installs/activates but starts zero Workers;
   - the first App activation uses that verified current lease exactly once;
   - installing an update does not silently restart a pinned live tab;
     explicit Reload/new App tab then launches the candidate;
   - TestEnvironment::install reports activation only and never recreates the
     removed legacy auto-launch behavior;
   - TestEnvironment::startAtNewTab returns after trusted New Tab/zero-Worker
     readiness, while start remains a compatibility wrapper that then explicitly
     navigates the default App and waits for its keyed Worker ready/healthy events;
   - activateAppAndWaitReady returns the new stable tab ID so later waits cannot
     fall back to a global/singleton Worker observation;
   - startup-incarnation mismatch discards a late lifecycle verification reply;
   - restore initialization itself does not schedule a redundant save;
   - completely offscreen geometry, removed-monitor topology, and less than a
     64x64 visible intersection fall back to a centered default on the primary
     available screen.

2. Confirm RED:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_session_store tst_browser_shell tst_unified_navigation tst_host_runtime_config tst_production_update_runtime tst_e2e_package_update tst_e2e_host_routes tst_e2e_web_fallback tst_e2e_pilot_capabilities tst_e2e_host_survives_worker_crash -- /m:1 /nr:false

3. Construct BrowserSessionStore only in package mode from HostRuntimeConfig's
   held HostOwnedStateDirectory. Trusted-shell mode gets an in-memory New Tab
   and no persistence authority.

4. Split HostApplication startup into an explicit two-phase sequence:

   1. create routes/chrome/model/Web profile without showing the window;
   2. bounded-load the structurally valid raw session on GUI, suppressing all
      persistenceNeeded signals;
   3. on the lifecycle thread, construct RuntimePackageAuthority and
      AppRuntimeCoordinator, then install/activate pending --install-package or
      verify offline current/LKG without launching a Worker;
   4. return an immutable verified current-package descriptor tagged with the
      startup incarnation;
   5. on GUI, discard stale replies and resolve raw descriptors using
      BrowserAddress + RouteRegistry + the returned descriptor;
   6. apply the model, instantiate only the active descriptor, then show.

   MainWindow never directly calls RuntimePackageAuthority across threads.
   Remove the current automatic installAndLaunch/startOffline Worker path; all
   Worker launches begin at App tab activation/reload/recovery.

   Update TestEnvironment in the same task: add startAtNewTab() for the true
   browser-start/zero-Worker boundary; keep start() as a compatibility wrapper
   that calls startAtNewTab(), explicitly navigates the default signed App, and
   waits for that keyed Worker. install() reports only verified activation, and
   subsequent Worker waits require an explicit stable tab ID returned by
   activateAppAndWaitReady()/Reload/new App tab. No production auto-launch path
   is retained, and existing E2E callers do not silently change preconditions.

5. Never trust persisted kind/title/ID as executable authority. The
   structurally loaded kind remains advisory; the second-phase resolver
   reclassifies it from current route/engine/package authority.

6. Make load status control saving:

   - Missing: build New Tab and allow later saves;
   - Loaded: apply without scheduling a save;
   - Corrupt: enable clean-session saving only after the bounded .corrupt
     preservation/removal transaction succeeds; otherwise disable saving;
   - IoFailure: use an in-memory New Tab but disable autosave/final save for the
     run so an unknown good file cannot be overwritten.

7. MainWindow owns one single-shot debounce QTimer. Save only approved snapshot
   fields; recently closed state, live page state, and all security authority
   remain memory-only.

8. Implement BrowserWindowGeometry as a pure function over the persisted QRect
   and current QScreen::availableGeometry() list. Restore only when at least
   64x64 remains visible on one current screen; otherwise clamp the default
   1280x800 size to the primary available geometry and center it. Persisted
   coordinates never select or create a screen.

9. Shutdown order is: stop model mutations/new capability admission; revoke
   gestures and new requests; freeze one snapshot; make the one bounded-size
   final save when enabled; asynchronously submit Worker lifetimes to retirement;
   then synchronously stop/destroy every Web page/surface before its profile as
   required by Task 8. A save failure never skips security cleanup.

10. Reconfigure, relink, and run:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_browser_session_store tst_browser_shell tst_unified_navigation tst_host_runtime_config tst_production_update_runtime tst_e2e_package_update tst_e2e_host_routes tst_e2e_web_fallback tst_e2e_pilot_capabilities tst_e2e_host_survives_worker_crash -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(browser_session_store|browser_shell|unified_navigation|host_runtime_config|production_update_runtime|e2e_package_update|e2e_host_routes|e2e_web_fallback|e2e_pilot_capabilities|e2e_host_survives_worker_crash)$' -j1

11. Commit:

       git add apps/host/BrowserWindowGeometry.h apps/host/BrowserWindowGeometry.cpp apps/host/CMakeLists.txt apps/host/MainWindow.h apps/host/MainWindow.cpp apps/host/HostApplication.h apps/host/HostApplication.cpp apps/host/BrowserSessionStore.h apps/host/BrowserSessionStore.cpp apps/host/TabController.h apps/host/TabController.cpp tests/unit/host/tst_browser_session_store.cpp tests/integration/host/tst_browser_shell.cpp tests/integration/host/tst_unified_navigation.cpp tests/integration/update/tst_production_update_runtime.cpp tests/e2e/TestEnvironment.h tests/e2e/TestEnvironment.cpp tests/e2e/tst_package_update.cpp
       git commit -m "feat: restore browser tabs lazily"

### Task 18: Prove the real browser workflow end to end

**Files:**

- Create: tests/e2e/BrowserUiAutomation.h
- Create: tests/e2e/BrowserUiAutomation.cpp
- Create: tests/e2e/tst_browser_shell.cpp
- Modify: tests/e2e/CMakeLists.txt
- Modify: tests/e2e/TestEnvironment.h
- Modify: tests/e2e/TestEnvironment.cpp
- Modify: tests/e2e/tst_pilot_capabilities.cpp
- Modify: tests/e2e/tst_host_survives_worker_crash.cpp

1. Add e2e_browser_shell using real signed LPAC Workers. UI Automation may
   discover controls and verify state only. Every required browser command and
   click uses SendInput after proving foreground window, focus owner, and
   on-screen coordinates; InvokePattern, ValuePattern, direct Qt signals, and
   production test commands are not acceptance evidence.

2. First write focused RED for every approved shortcut while a real Worker HWND
   owns focus. Prove the Host executes it exactly once, the Worker receives
   neither the matching keydown nor keyup, and no clipboard gesture is minted.
   Finish the already scoped HostGestureRouter Worker-focus path; do not
   register global hotkeys or broaden foreground matching.

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_e2e_browser_shell -- /m:1 /nr:false
       .\build\dev\tests\e2e\Debug\tst_e2e_browser_shell.exe workerFocusBrowserShortcuts -o -,txt

   Expected RED: a foreign Worker-focused chord is not yet routed/isolated, not
   a UIA discovery, COM initialization, focus, or package-launch failure.

3. Cover this deterministic workflow:

   1. create New Tab, App, and restricted Web tabs using browser commands;
   2. create different histories and prove Alt+Left/Right isolation;
   3. prove two App tabs have distinct PID/HWND/full authority tuples and preserved in-memory page state;
   4. switch without recreating healthy surfaces;
   5. Ctrl+R/F5 on an active App preserves tab/history but creates a fresh
      runtime from the current version;
   6. Esc during a deliberately observable App load cancels only that
      incarnation, rejects late attach, and Reload recovers the same tab;
   7. terminate A Worker and prove B, Host, and B capability lane remain usable;
   8. close/reopen A and prove fresh PID/runtime incarnation/full authority tuple;
   9. prove delayed A gesture/capability cannot cross into B;
   10. keep B usable while A owns a file dialog, then close A and observe exact cancellation;
   11. restart Host and restore order/addresses/histories with fresh authority and lazy inactive tabs;
   12. corrupt the session and get one safe New Tab;
   13. reject HTTPS, file, data, javascript, unknown protocol, download, popup, permission, and external navigation.

4. BrowserUiAutomation must initialize/uninitialize COM per test thread, bind
   controls by exact accessible name/role, and bind Worker windows by
   TestEnvironment's tab ID -> PID/HWND/local generation/runtime
   incarnation/lease epoch map. Never use “first Worker” or global count
   assumptions.

5. Link Windows E2E UIA support explicitly with Ole32, OleAut32, and
   Uiautomationcore.

6. Reconfigure and run focused E2E:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_e2e_browser_shell tst_e2e_pilot_capabilities tst_e2e_host_survives_worker_crash -- /m:1 /nr:false
       & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(e2e_browser_shell|e2e_pilot_capabilities|e2e_host_survives_worker_crash)$' -j1

7. Repeat the critical browser E2E a fixed five times; require 5/5.

       1..5 | ForEach-Object {
           & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^e2e_browser_shell$' -j1
           if ($LASTEXITCODE -ne 0) { throw "e2e_browser_shell failed on fixed run $_" }
       }

8. Commit:

       git add tests/e2e/BrowserUiAutomation.h tests/e2e/BrowserUiAutomation.cpp tests/e2e/tst_browser_shell.cpp tests/e2e/CMakeLists.txt tests/e2e/TestEnvironment.h tests/e2e/TestEnvironment.cpp tests/e2e/tst_pilot_capabilities.cpp tests/e2e/tst_host_survives_worker_crash.cpp
       git commit -m "test: prove trusted browser shell end to end"

### Task 19: Harden deployment, attestation, operations, and acceptance inventory

**Files:**

- Modify: scripts/build-release.ps1
- Modify: scripts/run-acceptance.ps1
- Modify: cmake/Deploy.cmake
- Modify: docs/architecture/runtime.md
- Modify: docs/security/threat-model.md
- Modify: docs/security/windows-sandbox.md
- Modify: docs/development/getting-started.md
- Modify: docs/operations/diagnostics.md
- Modify: docs/operations/update-rollback.md
- Create: docs/operations/browser-session.md
- Modify: docs/verification/mvp-acceptance-report.md

1. Extend build-release.ps1 protected state from package-store/sandbox-temp/telemetry/storage to include external browser-state and the explicit deployment root argument.

2. Rework New-DeployedHostAutomation/Invoke-DeployedNavigation for the
   address-field Enter workflow because Task 7 removes Go. Rework Worker
   assertions from “global Count == 1” to exact visible tab -> Worker PID/HWND
   bindings. Deployment code does not expose internal incarnation/lease epoch.
   Freshness is jointly attested by external close/reopen plus late-result
   rejection and the in-process real-process E2E's full authority tuple.
   Keep explicit one-Worker assertions only inside single-tab update cases.

   Remove every unconditional Wait-DeployedWorker immediately after
   Start-DeployedHost. Initial install must first assert zero Workers, then use
   real SendInput to activate an App address, and only then wait for its exact
   binding. Update/clipboard/restart flows either restore a persisted active App
   descriptor or explicitly activate one before waiting; each path states which
   precondition it uses.

3. Run the same complete deployed browser-shell critical path under minimal and
   polluted PATH, each with an independent protected package store, sandbox,
   telemetry, storage, and browser-state root. browser-session.json is the fixed
   leaf inside browser-state; there is no second session-root authority. Within
   each environment, reuse browser-state across the Host restart. Require these
   stable markers:

       DEPLOYMENT_BROWSER_TABS=PASS
       DEPLOYMENT_BROWSER_HISTORY=PASS
       DEPLOYMENT_BROWSER_STATE=PASS
       DEPLOYMENT_BROWSER_CRASH_ISOLATION=PASS
       DEPLOYMENT_BROWSER_REOPEN=PASS
       DEPLOYMENT_BROWSER_SESSION_RESTORE=PASS authority=fresh lazy=1
       DEPLOYMENT_BROWSER_CORRUPT_SESSION=PASS
       DEPLOYMENT_BROWSER_GESTURE_ISOLATION=PASS
       DEPLOYMENT_BROWSER_FILE_BINDING=PASS
       DEPLOYMENT_BROWSER_DENIALS=PASS

4. Snapshot browser-state ACL and identity before/after E2E. Validate
   browser-session.json structurally against the Task 5 schema. Allowed fields
   are version, window geometry, active tab ID, tab ID/advisory kind,
   canonical plain-text title, canonical logical address/history, and history
   index. Address/history/title may derive from user navigation and are the only
   allowed user-origin display fields. Prove no form/page draft, page body,
   credential, request/response body, selected file content/path, clipboard,
   cookie, permission, cache, PID, HWND, nonce, generation, grant, or capability
   authority. Use unique sensitive-source canaries plus exact schema keys, not a
   claim that arbitrary user content is absent. Keep .corrupt Host-only,
   bounded, unlogged, and outside deployment/manifest.

5. Replace findstr /P hook checks with raw-byte scanning of every staging EXE
   and DLL for ASCII and UTF-16 canaries. Include
   qbrowser_host_testing, qbrowser_broker_testing,
   qbrowser_archive_testing, qbrowser_package_installer_testing,
   qbrowser_package_store_testing, qbrowser_signature_testing,
   qbrowser_sandbox_testing, qbrowser_browser_testing, all existing test-hook
   class names, and every newly introduced ForTesting symbol. Prefer no new
   production hooks.

6. Upgrade Deploy.cmake to require this exact compact attestation plus newline:

       {"schema":2,"deploymentOnlyE2E":true,"routeCount":10,"webEngine":"deployed","signedUpdate":"1.1.0","rollback":"1.0.0","browserShell":"tabs+history+restore","authority":"fresh","lazyRestore":true,"crashIsolation":true,"corruptSession":true,"gestureIsolation":true,"fileBinding":true,"denials":true,"paths":"minimal+polluted"}

   Map browserShell to TABS/HISTORY/STATE/REOPEN markers; authority/lazyRestore
   to SESSION_RESTORE; crashIsolation to CRASH_ISOLATION; corruptSession to
   CORRUPT_SESSION; gestureIsolation to GESTURE_ISOLATION; fileBinding to
   FILE_BINDING; denials to DENIALS; and paths only when the identical workflow
   passes both environments. Reject browser-state, browser-session.json, and
   .corrupt from deployment and SHA-256SUMS. Add browser-session.md to the
   documented deployment inventory.

7. Add browser_address, browser_tab_model, browser_session_store, new_tab_page,
   browser_chrome, web_session_profile, browser_shell,
   app_runtime_coordinator, browser_capability_isolation, e2e_browser_shell,
   host_runtime_config, and unified_navigation to run-acceptance.ps1 required
   tests/executables. Continue requiring JUnit failures=0, errors=0, skipped=0;
   do not hard-code a case total before the final run.

8. Update runtime/security/operations documentation to describe:

   - per-tab Worker authority and shared ephemeral Web profile;
   - persisted session threat model and external protected root;
   - gesture/file/late-result cross-tab threats;
   - candidate rollback fan-out;
   - safe session clearing/corruption diagnostics without logging contents.

9. Run real parser checks—there is no dry-run switch:

       [void][ScriptBlock]::Create((Get-Content -Raw scripts\build-release.ps1))
       [void][ScriptBlock]::Create((Get-Content -Raw scripts\run-acceptance.ps1))

   Then run a real Debug preflight in a new directory:

       $Preflight = Join-Path (Resolve-Path .) ("build\browser-shell-preflight-" + [Guid]::NewGuid().ToString('N'))
       powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 -Configuration Debug -BuildDirectory $Preflight

   Do not claim final Release success until Task 20.

10. Commit:

       git add scripts/build-release.ps1 scripts/run-acceptance.ps1 cmake/Deploy.cmake docs/architecture/runtime.md docs/security/threat-model.md docs/security/windows-sandbox.md docs/development/getting-started.md docs/operations/diagnostics.md docs/operations/update-rollback.md docs/operations/browser-session.md docs/verification/mvp-acceptance-report.md
       git commit -m "build: attest trusted browser shell"

### Task 20: Perform fresh acceptance, review, Release publication, and cleanup

**Files:**

- Modify only if evidence requires a deterministic fix: production/test files named by the failing reproducer.
- Finalize: docs/verification/mvp-acceptance-report.md

1. Apply superpowers:verification-before-completion. Inspect status and diff first:

       git status --short
       git diff --check
       git log --oneline 4e36487..HEAD

2. Relink every repeated executable, then run fixed fluctuation probes before
   full acceptance:

       & 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_pilot_routes tst_production_update_runtime tst_e2e_package_update tst_e2e_browser_shell -- /m:1 /nr:false

       1..10 | ForEach-Object {
           & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^pilot_routes$' -j1
           if ($LASTEXITCODE -ne 0) { throw "pilot_routes failed on fixed run $_" }
       }
       1..5 | ForEach-Object {
           & 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(production_update_runtime|e2e_package_update|e2e_browser_shell)$' -j1
           if ($LASTEXITCODE -ne 0) { throw "fixed runtime repeat failed on run $_" }
       }

   On any failure, capture the first phase/process/log evidence, add the smallest reproducer, and fix the root cause. Do not raise 30/60/120/900-second bounds.

3. Run a never-before-existing fresh Debug acceptance directory:

       $FreshDebug = Join-Path (Resolve-Path .) ("build\browser-shell-fresh-debug-" + [Guid]::NewGuid().ToString('N'))
       powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 -Configuration Debug -BuildDirectory $FreshDebug

   Required evidence: full CTest green, every required JUnit suite has zero failures/errors/skips, Node/Vitest green, signed packaging passes, and all browser E2E tests use real processes.

4. Run superpowers:requesting-code-review with reviewers split across:

   - tab/UI/Web lifetime;
   - package/multiworker rollback;
   - gesture/file/session security;
   - release/deployment evidence.

   Resolve every actionable finding with RED/GREEN evidence and rerun the affected focused suite. Inspect the full 4e36487..HEAD diff after review.

5. Run the guarded clean Release publication:

       powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Clean

   Required evidence: BUILD_TESTING=OFF, zero CTest inventory, no test hook/private key, no reparse point, browser-state outside deployment, complete deployed browser E2E under minimal and polluted PATH, and an attested immutable inventory.

6. Run the no-clean verifier/publication path:

       powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-release.ps1

   Expected: verification succeeds without changing deployment inventory, ACLs, or file identities.

7. Record in mvp-acceptance-report.md:

   - final commit;
   - exact test and JUnit counts from generated evidence;
   - fixed-repeat results;
   - Release deployment path;
   - manifest SHA-256;
   - all browser deployment markers;
   - dual-PATH results;
   - session allowlist/security evidence;
   - nohooks/zero-test proof;
   - review findings and resolutions.

8. Commit the final evidence:

       git add docs/verification/mvp-acceptance-report.md
       git commit -m "docs: record browser shell acceptance"

9. Perform exact safe cleanup only after every build/test/review process exits:

   - enumerate only this worktree's ignored browser-shell build/temp/log targets;
   - resolve each literal target and prove it is a strict descendant of the worktree/build root;
   - reject tracked files, .git, .task12-npm-cache, unknown .qbrowser-dev, other workspaces, and the protected LocalAppData deployment;
   - delete only verified targets with one PowerShell implementation;
   - record item/byte counts and re-enumerate zero residue.

10. Finish with:

       git status --short
       git diff --check 4e36487..HEAD
       git diff --check

   Expected: only the protected untracked .task12-npm-cache remains; the published deployment still verifies through the no-clean command.

## Completion gates

The implementation is not complete until all ten approved design gates in 2026-08-23-browser-shell-design.md are proven. In particular:

- switching a healthy tab never recreates it;
- inactive restored tabs allocate no Worker/Web resource until selected;
- every App tab has a distinct PID/HWND/tab ID/runtime-incarnation/local-generation/lease-epoch tuple and capability lane;
- rollback retires all and only failed-candidate authorities;
- clipboard gestures and file results cannot cross tabs or reopened incarnations;
- session JSON contains only bounded display/navigation state;
- arbitrary Internet and file navigation remain denied;
- clean Release and no-clean verification both pass under minimal and polluted PATH.
