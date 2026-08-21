# Task 16 Final Review Follow-up Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make stale worker-admission failures key-aware and replace unchecked Qt launch/observer thread starts with synchronously reportable, bounded native-thread startup and retirement behavior.

**Architecture:** The lifecycle coordinator treats a failure for any non-current attempt as an ignored stale signal, and the Host carries that disposition back to the launcher so the stale worker is retired without poisoning or reporting the current activation. Launch and observer work use a small checked detached-thread factory; launch contexts and observer counters are registered before construction, construction failures synchronously unwind state and submit retirement, and all retirement waits are bounded while the process-lifetime manager remains leak-safe through OS teardown.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Test, Windows LPAC/AppContainer, CMake/CTest.

---

### Task 1: Key-aware stale admission RED to GREEN

**Files:**
- Modify: `tests/integration/update/tst_update_lifecycle.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`
- Modify: `runtime/package/UpdateLifecycleCoordinator.cpp`
- Modify: `apps/host/HostApplication.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.h`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`

1. Add a coordinator test that starts A, starts B, reports A admission failure, and expects `IgnoredStaleAttempt`, no failed-closed transition, and B unchanged.
2. Run only that test and verify it fails because `workerAdmissionFailed` unconditionally enters failed-closed.
3. Add a real signed-package/LPAC test hook that begins B on the lifecycle thread immediately before deciding A admission. Assert A emits no ready/failure, B becomes ready and healthy, and A retires cleanly.
4. Run the production test and verify the stale admission currently rejects through the generic failure path.
5. Add the key comparison and an explicit stale admission disposition. Retire stale A without calling the Host failure callback.
6. Re-run both tests and verify GREEN.

### Task 2: Checked launch-thread startup RED to GREEN

**Files:**
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncherTestHooks.h`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`

1. Add a launch-thread-start failure injection test that records context/thread baselines, starts a real Host, observes the stable startup error, destroys the Host in under 100 ms, and verifies manager/context/temp cleanup.
2. Run it and verify RED because `QThread::start()` cannot synchronously report construction/start failure.
3. Add a checked detached `std::thread` factory, pre-register the launch context and active counter, and on construction failure synchronously call `launchFinished`, submit retirement, reset admission state, and report a stable error.
4. Re-run and verify GREEN.

### Task 3: Checked observer-thread startup and bounded retirement RED to GREEN

**Files:**
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.h`
- Modify: `apps/host/WorkerRetirementManager.cpp`

1. Add an observer-thread-start failure injection test using a real LPAC process. Assert no ready/session attachment, stable error, Host destruction under 100 ms, process exit, counters/temp/profile cleanup, and manager idle.
2. Run it and verify RED against unchecked `QThread::start()`.
3. Start a gated observer natively before attachment, roll back its pre-registered counter on failure, terminate and retire the process, and release the gate before the final ready emission.
4. Bound the launch-retirement condition wait and make the singleton process-lifetime/leak-safe so static destruction cannot block indefinitely or race detached cleanup.
5. Re-run both thread failure tests and existing destruction/shutdown tests.

### Task 4: Verification and follow-up commit

1. Run focused lifecycle and production failure tests repeatedly.
2. Run fresh Debug full serial and polluted PATH/QML/plugin matrices.
3. Run fresh Release with `BUILD_TESTING=OFF` and prove no hooks or test targets.
4. Audit processes, AppContainer profiles, retirement contexts/observers, and temporary roots.
5. Run `git diff --check`, review the staged diff, and create one Task 16 follow-up commit.
