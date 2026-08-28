# App Runtime Coordinator Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a lifecycle-thread `AppRuntimeCoordinator` that shares app package authority across tabs while preserving independent worker supervision and pinned verified leases.

**Architecture:** Keep `PackageStore` and `PackageInstaller` as the package authority and verification boundary. The coordinator owns per-tab supervisors, full event keys, lease epochs, and a two-phase authority-drain state machine; Task14 injects a deterministic drain consumer and leaves the production Host adapter unchanged.

**Tech Stack:** C++20, Qt 6 Core/Test, CMake, existing `PackageStore`, `PackageInstaller`, `WorkerSupervisor`, and `AuthorityAdmissionToken`.

---

### Task 1: Add the failing coordinator contract tests

**Files:**
- Create: `tests/integration/update/tst_app_runtime_coordinator.cpp`
- Modify: `tests/integration/update/CMakeLists.txt`

**Step 1: Write the failing tests**

Add a Qt test fixture with deterministic monotonic time and a fake drain consumer. Cover the public contract and the required state cases: candidate/current lease separation, pinned restart, independent attempts, one-shot LKG promotion, stale/revoked admission, candidate crash-loop rollback, stable revoke/stop/drain ordering, timeout failed-closed behavior, stale events, unrelated app isolation, zero-worker activation, reload-versus-crash lease choice, shutdown, and distinct clean/startup/crash/cleanup failures. Keep assertions on result code, action kind/order, tab/runtime identity, launch lease/mode, and absence of sensitive telemetry fields.

**Step 2: Register the target**

Use the existing `q_browser_add_update_test` helper and add a `tst_app_runtime_coordinator` target with test name `app_runtime_coordinator`.

**Step 3: Run the RED build**

Run:

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_app_runtime_coordinator -- /m:1 /nr:false
```

Expected: configuration succeeds, but compilation fails because `AppRuntimeCoordinator.h` and the implementation do not yet exist.

**Step 4: Commit the RED test**

```powershell
git add tests/integration/update/tst_app_runtime_coordinator.cpp tests/integration/update/CMakeLists.txt
git commit -m "test: define app runtime coordinator contract"
```

### Task 2: Add the public types and package-store friend boundary

**Files:**
- Create: `runtime/package/AppRuntimeCoordinator.h`
- Modify: `runtime/package/PackageStore.h`
- Modify: `runtime/package/CMakeLists.txt`

**Step 1: Define the contract**

Declare the exact `TabLaunchAuthority`, `TabLaunchIntent`, `FullAttemptKey`, `AuthorityDrainBatch`, `AppRuntimeActionKind`, `AppRuntimeAction`, `AppRuntimeResultCode`, and `AppRuntimeResult` types from the browser-shell plan. Add equality helpers where tests need deterministic comparisons. Declare the coordinator constructor with package-store/installer references, supervision policy, lifecycle clock, optional event recorder, and an injectable drain callback that receives one `AuthorityDrainBatch`.

**Step 2: Add the narrow friend**

Add only `friend class AppRuntimeCoordinator;` to `PackageStore`; do not make activation or rollback internals public.

**Step 3: Add library sources**

Add `AppRuntimeCoordinator.h/.cpp` to `q_browser_package` sources (the implementation is still a stub until Task 3).

**Step 4: Compile the contract**

Run the RED target again. Expected: the test now links against a stub but fails at runtime/assertions until state behavior is implemented.

### Task 3: Implement package descriptors and tab launch state

**Files:**
- Modify: `runtime/package/AppRuntimeCoordinator.h`
- Create/Modify: `runtime/package/AppRuntimeCoordinator.cpp`

**Step 1: Add internal state**

Store current/candidate/LKG descriptors, per-tab state keyed by tab ID plus runtime incarnation, a monotonic lease epoch, active/revoked token records, rollback/drain state, shutdown state, and one-shot promotion state. Keep all mutation on the lifecycle thread.

**Step 2: Implement install/offline**

`installAndActivate` calls `PackageInstaller::install`, reuses the returned verified metadata and activation binding, records candidate/current descriptors, and emits no Launch action. `startOffline` reads and verifies recorded current/LKG descriptors and emits no Launch action. Errors map to `Rejected` or `FailedClosed` with stable non-sensitive strings.

**Step 3: Implement launch requests**

`requestTabLaunch` validates the tab authority and shutdown state, chooses current candidate for `ActivateCurrent`/`ReloadCurrent`, or the tab’s pinned lease for `RestartPinned`, creates a fresh supervisor activation/attempt and authority token, and returns one `Launch` action containing a complete `WorkerLaunchRequest`. New-tab activation must not start a Worker until this method is called.

**Step 4: Run focused tests**

Run the new executable with `newTabsUseCandidateWhileExistingTabsRemainPinned`, `pinnedOldTabRestartsItsPinnedVersion`, `activeNewTabWithInstallPackageStartsZeroWorkers`, and `userReloadUsesCurrentCandidateWhileCrashRestartUsesPinnedLease`. Expected: these cases pass before failure/rollback cases are enabled.

### Task 4: Implement admission, health, and promotion

**Files:**
- Modify: `runtime/package/AppRuntimeCoordinator.cpp`

**Step 1: Validate complete event keys**

Require exact tab ID, runtime incarnation, attempt key, lease epoch, matching lease contents, and an active non-revoked token. Return `IgnoredStale` for superseded events and `Rejected` for invalid/revoked admissions without producing actions.

**Step 2: Implement admission and heartbeat**

Forward valid admissions/heartbeats to the owning `WorkerSupervisor`; never use `WorkerAttemptKey` alone. Preserve old-version pinned tabs when current generation changes.

**Step 3: Implement one-shot healthy promotion**

On the first sustained healthy candidate, call the store’s LKG method exactly once and rebind same-version sibling candidate descriptors so the generation change does not poison them. Record only app-level telemetry without tab IDs, routes, PIDs, or user data.

**Step 4: Run focused tests**

Run `firstHealthyCandidateMarksLkgExactlyOnce`, `lkgGenerationChangeDoesNotPoisonSiblingAdmission`, `revokedLeaseCannotPassAdmission`, and the pinned-old-version authority cases. Expected: PASS.

### Task 5: Implement crash-loop rollback and two-phase drain

**Files:**
- Modify: `runtime/package/AppRuntimeCoordinator.cpp`

**Step 1: Map supervisor outcomes**

Handle clean exit, startup/admission failure, crash exit, health timeout, and cleanup failure distinctly. A candidate crash-loop starts one rollback; a promoted/LKG sibling follows normal per-tab restart behavior.

**Step 2: Collect and revoke atomically**

Stable-sort affected tabs, call `beginRevoke()` on every affected token before scheduling any drain or recovery action, and emit `Revoke` actions followed by `Stop` actions. Create one batch with all tickets and one shared monotonic deadline; emit `AwaitAuthorityDrain` and invoke only the injected non-blocking consumer.

**Step 3: Complete or time out off lifecycle**

Before-deadline completion emits ordered `RecoverFromLkg`/`Launch` actions. Timeout emits ordered `FailedClosed`, `Stop`, and `IsolateSession`, retains the pending drain, forbids recovery/launch permanently, and treats eventual completion as cleanup only. No coordinator method waits on a ticket.

**Step 4: Run focused tests**

Run the rollback fan-out, revoke ordering, paused `UseGuard`, sibling-heartbeat, timeout, late-event, and unrelated-app tests. Expected: PASS.

### Task 6: Implement shutdown and cleanup semantics

**Files:**
- Modify: `runtime/package/AppRuntimeCoordinator.cpp`

**Step 1: Retire all tabs**

`beginShutdown` revokes/retire all admitted tabs in stable order and rejects all subsequent requests/events. `closeTab` retires only its exact tab authority and leaves siblings intact.

**Step 2: Preserve failed-closed behavior**

Cleanup failures and invalid drain completion transition only the affected runtime/batch to `FailedClosed`; they never reopen a token or relaunch after timeout.

**Step 3: Run focused tests**

Run shutdown, clean-exit, startup/admission failure, crash-exit, cleanup-failure, and repeated rollback tests. Expected: PASS.

### Task 7: Run the prescribed update regression set

**Step 1: Build all selected targets**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\dev --config Debug --target tst_app_runtime_coordinator tst_update_lifecycle tst_crash_rollback tst_offline_lkg tst_production_update_runtime -- /m:1 /nr:false
```

**Step 2: Run the focused CTest set**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\dev -C Debug --output-on-failure --no-tests=error -R '^(app_runtime_coordinator|update_lifecycle|crash_rollback|offline_lkg|production_update_runtime)$' -j1
```

Expected: all selected tests pass.

**Step 3: Review and commit**

Run `git diff --check`, inspect the diff for accidental Host/GUI changes, and commit:

```powershell
git add runtime/package/AppRuntimeCoordinator.h runtime/package/AppRuntimeCoordinator.cpp runtime/package/CMakeLists.txt runtime/package/PackageStore.h tests/integration/update/tst_app_runtime_coordinator.cpp tests/integration/update/CMakeLists.txt
git commit -m "feat: coordinate package authority across tabs"
```

### Task 8: Verify release and repository hygiene

**Step 1: Build the release host target**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build build\task13-release --config Release --target qbrowser-host -- /m:1 /nr:false
```

**Step 2: Verify no-test release registration and source state**

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --test-dir build\task13-release -C Release -N
git diff --check
git status --short
```

Expected: release target succeeds, no release tests are registered, diff check is clean, and only the pre-existing `.task12-npm-cache/` remains untracked.
