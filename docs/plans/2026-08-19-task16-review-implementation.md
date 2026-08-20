# Task 16 Review Closure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Close all Task16 review findings with a production package runtime, atomically bound lifecycle state, monotonic timing, and deadlock-free asynchronous Host orchestration.

**Architecture:** A validated production `HostRuntimeConfig` composes the existing package, sandbox, worker, session, surface, supervisor, and telemetry components. Lifecycle attempts carry a persisted activation binding and exchange generation-tagged asynchronous commands with the GUI launcher; all deadlines use an injected steady clock.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Test, Windows LPAC/AppContainer, Win32 process handles and named pipes, CMake/CTest, Ed25519 `.qapkg` fixtures.

---

### Task 1: Production runtime configuration and composition RED→GREEN

**Files:**
- Create: `apps/host/HostRuntimeConfig.h`
- Create: `apps/host/HostRuntimeConfig.cpp`
- Create: `tests/unit/host/tst_host_runtime_config.cpp`
- Modify: `apps/host/main.cpp`
- Modify: `apps/host/HostApplication.h`
- Modify: `apps/host/HostApplication.cpp`
- Modify: `apps/host/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

1. Write tests that demand explicit trusted-shell mode or a complete package-mode
   configuration and reject missing/relative/PATH/reparse/overlapping trust inputs.
2. Run `host_runtime_config`; verify RED because the type and production
   composition API do not exist.
3. Implement bounded CLI/config parsing and valid-by-construction configuration.
4. Make `main` construct `HostApplication` from that configuration and make package
   mode compose all Task16 owners; remove package-mode dependence on a test setter.
5. Run `host_runtime_config` and existing Host tests; verify GREEN.

### Task 2: Production installed-package launcher RED→GREEN

**Files:**
- Create: `apps/host/InstalledPackageWorkerLauncher.h`
- Create: `apps/host/InstalledPackageWorkerLauncher.cpp`
- Create: `tests/integration/update/tst_production_update_runtime.cpp`
- Modify: `apps/host/HostApplication.*`
- Modify: `apps/host/CMakeLists.txt`
- Modify: `tests/integration/update/CMakeLists.txt`

1. Extend the signed package fixture so each version supplies its real QML and
   recovered 1.1 issues a capability request.
2. Write an integration test that installs signed 1.0/1.1/1.2 and expects the
   production launcher to run each exact final package directory and the configured
   absolute Worker through `SandboxTrustBoundary`/`SandboxLauncher`.
3. Verify RED because no production launcher exists and the current test manually
   reports handshake/exit.
4. Implement pipe/session/process/surface ownership, authenticated attach,
   generation-tagged heartbeat, and automatic process-handle exit observation.
5. Make the test terminate the real 1.2 process twice and observe automatic restart,
   atomic rollback, recovered 1.1 handshake and capability response. Verify GREEN.

### Task 3: Activation compare-and-commit binding RED→GREEN

**Files:**
- Modify: `runtime/package/ActivationState.h`
- Modify: `runtime/package/ActivationState.cpp`
- Modify: `runtime/package/PackageStore.h`
- Modify: `runtime/package/PackageStore.cpp`
- Modify: `runtime/package/PackageInstaller.*`
- Modify: `runtime/package/UpdateLifecycleCoordinator.*`
- Modify: `tests/unit/package/tst_package_store.cpp`
- Modify: `tests/integration/update/tst_update_lifecycle.cpp`
- Modify: `tests/integration/update/tst_offline_lkg.cpp`

1. Write double-store tests for stale mark-LKG, stale rollback/recover, lock failure,
   and offline verify-A/concurrent-activate-B.
2. Verify RED: current read-then-write APIs overwrite or launch stale state.
3. Persist activation generation and return exact `ActivationBinding` from verified
   activation/install operations.
4. Under one activation lock, re-read, validate, compare expected current/digest/
   generation, and atomically write generation+1 for mark/rollback/recover.
5. Store the binding in every coordinator attempt and fail closed on mismatch.
6. Run package store/installer/update tests; verify GREEN.

### Task 4: Monotonic lifecycle and single Healthy transition RED→GREEN

**Files:**
- Modify: `runtime/worker/WorkerSupervisor.*`
- Modify: `runtime/package/UpdateLifecycleCoordinator.*`
- Modify: `apps/host/HostApplication.*`
- Modify: `tests/integration/update/tst_update_lifecycle.cpp`
- Modify: `tests/integration/worker/tst_worker_crash.cpp`

1. Write a fake steady clock and independently jumping UTC source test.
2. Write a race test where heartbeat and health timer reach the window together and
   assert one LKG commit and one Healthy record.
3. Verify RED because lifecycle callers currently pass UTC and healthy logic is
   duplicated.
4. Inject steady milliseconds into coordinator/supervisor; keep UTC only inside
   telemetry event creation.
5. Extract one healthy-transition helper and ignore duplicate/stale observations.
6. Run lifecycle/worker/telemetry tests; verify GREEN.

### Task 5: Asynchronous Host launch/stop state machine RED→GREEN

**Files:**
- Modify: `runtime/package/UpdateLifecycleCoordinator.*`
- Modify: `apps/host/HostApplication.*`
- Modify: `apps/host/InstalledPackageWorkerLauncher.*`
- Modify: `tests/integration/host/tst_unified_navigation.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`

1. Write barriers that destroy Host during rollback stop and during a queued recovery
   launch; assert bounded GUI responsiveness, no deadlock, no late attach/launch, and
   no UAF.
2. Verify RED against `BlockingQueuedConnection` and synchronous launch callback.
3. Replace callback flow with generation-tagged queued stop/launch requests and
   queued results. Reject admission and cancel generations before shutdown.
4. Ensure process observer/session/surface handles close once and clean shutdown is
   not reported as crash.
5. Run Host/update tests repeatedly; verify GREEN.

### Task 6: Production CLI and complete verification

**Files:**
- Modify only files required by failures found in the preceding RED→GREEN cycles.

1. Run production CLI smoke with real dev public key, signed package, absolute
   Worker/runtime roots and sandbox temp; then run offline from its committed LKG.
2. Run Task6/9/10/14/16 targets and repeated real LPAC timings.
3. Run fresh Debug full build/CTest, then polluted PATH/QML/plugin tests.
4. Run fresh Release `BUILD_TESTING=OFF`; prove zero tests/targets/hooks.
5. Verify no Worker process, AppContainer profile, session, surface, or handle leak;
   run `git diff --check` and self-review the staged diff.
6. Commit once as the Task16 review follow-up and report RED→GREEN evidence and SHA.
