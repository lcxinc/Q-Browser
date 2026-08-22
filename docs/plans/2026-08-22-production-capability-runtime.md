# Production Capability Runtime Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Compose the existing Task 8 brokers into the production Host and prove real Pilot business capabilities through the signed LPAC deployment path.

**Architecture:** Authenticated manifest permissions flow with every verified launch into a generation-bound Host capability runtime. Network/storage execute serially on a dedicated thread, file/clipboard execute on the GUI thread, and the existing controller/IO path enforces one correlated in-flight request while continuing to observe heartbeat and shutdown messages.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Network/Test, Windows LPAC/AppContainer, Windows UI Automation, CMake/CTest, PowerShell deployment verification, Node mock API.

---

### Task 1: Audit Task-owned artifacts and defer destructive cleanup

**Files:** No source changes.

1. Resolve the workspace and Task18 trusted root with `[IO.Path]::GetFullPath`.
2. Enumerate `build/task19-*` directories and verify every resolved target is a strict workspace/build descendant with a `task19-` leaf.
3. Do not let slow cleanup block implementation; perform the complete build-tree cleanup only after all verification and review processes exit, as Task 11.
4. Never remove `.task12-npm-cache`, the final LocalAppData deployment, tracked files, `.git`, an unknown `.qbrowser-dev`, or another workspace.

### Task 2: Establish the production Pilot RED

**Files:**
- Create: `tests/e2e/WorkerUiAutomation.h`
- Create: `tests/e2e/WorkerUiAutomation.cpp`
- Create: `tests/e2e/tst_pilot_capabilities.cpp`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/e2e/TestEnvironment.h`
- Modify: `tests/e2e/TestEnvironment.cpp`

1. Add a Windows UI Automation helper that binds only the current Worker HWND/process and finds controls by exact accessible name and control type.
2. Add `productionHostCompletesPilotBusinessCapabilities`, operating real login, dashboard, order PATCH, settings, and file UI.
3. Add signed probe-package coverage for undeclared capability and gestureless clipboard read, with the Worker exposing the exact response through an allowed observable route/navigation result.
4. Run only `e2e_pilot_capabilities` in the existing Debug build.
5. Verify RED is the expected `capability.unhandled`/missing business-state failure, not a UI Automation setup error.

### Task 3: Require and validate an independent storage root

**Files:**
- Modify: `tests/unit/host/tst_host_runtime_config.cpp`
- Modify: `apps/host/HostRuntimeConfig.h`
- Modify: `apps/host/HostRuntimeConfig.cpp`
- Modify: `tests/e2e/TestEnvironment.h`
- Modify: `tests/e2e/TestEnvironment.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`
- Modify: `tests/integration/host/tst_unified_navigation.cpp`
- Modify: `docs/development/getting-started.md`

1. Add failing config tests for missing storage directory, reparse storage, and overlap with every protected root/category.
2. Run `host_runtime_config` and confirm the new cases fail because the argument is not implemented.
3. Add the required `--storage-directory`, canonical/reparse checks, accessor, and pairwise overlap validation.
4. Update package-mode test fixtures with independent temporary storage roots.
5. Run `host_runtime_config`, `unified_navigation`, and `production_update_runtime` to GREEN.

### Task 4: Propagate authenticated permissions and bind identity

**Files:**
- Modify: `runtime/package/PackageInstaller.h`
- Modify: `runtime/package/PackageInstaller.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.h`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`
- Modify: `apps/host/HostApplication.h`
- Modify: `apps/host/HostApplication.cpp`
- Test: `tests/integration/package/tst_package_installer.cpp`
- Test: `tests/integration/host/tst_unified_navigation.cpp`

1. Add failing tests that verified results expose exact manifest permissions and attach rejects a launch/session identity mismatch.
2. Run the two focused tests and confirm the expected RED.
3. Add `ManifestPermissions` to successful `InstallResult`, carry it through the launch Ready/Attach context, and validate Host-assigned identity at attach.
4. Run the focused tests to GREEN.

### Task 5: Add the affinity-aware capability runtime

**Files:**
- Create: `apps/host/HostCapabilityRuntime.h`
- Create: `apps/host/HostCapabilityRuntime.cpp`
- Modify: `apps/host/CMakeLists.txt`
- Modify: `apps/host/HostApplication.cpp`
- Test: `tests/integration/host/tst_unified_navigation.cpp`
- Test: `tests/unit/policy/tst_policy_engine.cpp`

1. Add failing tests for exact mock-origin intersection, service-thread identity, unknown/undeclared denial, gestureless clipboard denial, storage identity, bounded request/result, and late-generation discard.
2. Run the focused tests and confirm their precise RED.
3. Build `HostPolicy` from the configured exact origin and intersect it with authenticated permissions.
4. Implement the GUI coordinator plus serial network/storage worker lane, each invoking `CapabilityBroker`.
5. Link `q_browser_broker` into `q_browser_host` and platform UI Automation dependencies only into tests.
6. Run the focused tests to GREEN.

### Task 6: Correlate production IPC requests without losing heartbeat

**Files:**
- Modify: `apps/host/HostWorkerSessionIo.h`
- Modify: `apps/host/HostWorkerSessionIo.cpp`
- Modify: `apps/host/HostWorkerSessionController.h`
- Modify: `apps/host/HostWorkerSessionController.cpp`
- Modify: `apps/host/HostApplication.cpp`
- Test: `tests/integration/host/tst_unified_navigation.cpp`
- Test: `tests/integration/ipc/tst_ipc_session.cpp`

1. Add failing tests for full request forwarding, exactly one active request, `capability.busy` on a second request, heartbeat delivery while pending, response correlation, resume-after-send, and old-generation result discard.
2. Run the focused tests and confirm the expected RED.
3. Replace the fixed unhandled response with the generation-bound dispatch path; keep IO polling heartbeats/shutdown while capability work is pending.
4. Use the existing outbound command queue for responses and clear the active request only after successful send.
5. Run the focused tests to GREEN.

### Task 7: Turn the real production Pilot E2E GREEN

**Files:**
- Modify: `tests/e2e/tst_pilot_capabilities.cpp`
- Modify: `tests/e2e/WorkerUiAutomation.cpp`
- Modify: `tests/e2e/TestEnvironment.cpp`

1. Run `e2e_pilot_capabilities` and diagnose each remaining failure at the component boundary.
2. Make only test-harness corrections needed to operate real UI; do not add a fake runtime or production test command.
3. Prove login/dashboard/order mutation, storage across real Worker restart, file cancel/success, undeclared denial, and clipboard gesture denial.
4. Repeat the E2E a fixed 5 times and require 5/5.

### Task 8: Strengthen Task18 deployment acceptance

**Files:**
- Modify: `scripts/build-release.ps1`
- Modify: `cmake/Deploy.cmake.in`
- Modify: `docs/deployment/windows-release.md`

1. Add the protected independent storage root and its ACL/reparse/non-overlap verification.
2. Extend deployment UI Automation to prove real mock API business state, storage restart persistence, real file cancel/success, and policy denial using only deployed binaries.
3. Ensure the deployment response/telemetry probes contain no login password, token, selected file content, or storage value.
4. Keep Release `BUILD_TESTING=OFF` and product hook-marker checks.

### Task 9: Diagnose named fluctuation candidates

**Files:** Modify production/tests only if a deterministic RED establishes a root cause.

1. Run fixed repeats, not `until-pass`: `pilot_routes` 10 times, `production_update_runtime` 5 times, and `e2e_package_update` 5 times with serial LPAC execution and `L:` temporary paths.
2. Before repeating `production_update_runtime`, add phase events, child stdout/stderr capture, owned-process scope, and failure cleanup so a watchdog identifies the exact stuck phase and never leaves a Host behind.
3. Do not increase the existing 30/60/120/900 second bounds. Treat a 900 second watchdog as a hang to diagnose, not a slow success to accommodate.
4. Capture the first failure of `pilot_routes` and `e2e_package_update` with the same per-run identity/log/process evidence.
5. On any failure, apply systematic debugging, add the smallest reproducer, and only then fix.
6. Repeat the same fixed counts after any fix.

### Task 10: Full verification and acceptance report

**Files:**
- Modify: `docs/verification/mvp-acceptance-report.md`

1. Run focused tests and fixed repeats.
2. Run a never-before-existing fresh Debug acceptance directory: full CTest, required JUnit with zero skips, Node/Vitest, and packaging.
3. Run minimal and polluted environment repeats.
4. Run guarded Task18 clean Release build/deployment E2E, no-clean verification, independent minimal/polluted PATH verifier, and Release nohooks/0-test proof.
5. Record exact commands, timestamps, counts, runtime security evidence, final deployment inventory, and manifest SHA in the report; remove the stale capability claims.
6. Request independent code review, resolve findings, inspect the complete diff, and commit the follow-up without touching `.task12-npm-cache`.

### Task 11: Final exact artifact cleanup

**Files:** No tracked source changes except the acceptance report's cleanup evidence.

1. Confirm no build, test, review, mock API, Host, Worker, MSBuild, CMake, or Node process owns the implementation worktree.
2. Generate an exact item/byte inventory for the implementation worktree's git-ignored `build` tree and explicit root `.task16-*`/task-test temporary directories and logs.
3. Resolve every target and verify it is inside the implementation worktree and is untracked/ignored; reject tracked files, `.git`, `.task12-npm-cache`, unknown `.qbrowser-dev`, other workspaces, and the LocalAppData deployment.
4. Delete only the verified literal targets with one PowerShell implementation and re-enumerate zero residue.
5. Record removed item/byte counts, run `git status`, then prove the protected published deployment still passes the independent verifier and that no-clean verification remains possible.
