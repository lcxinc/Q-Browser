# Task 16 Authority, Retirement, and Admission Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Close the remaining Task 16 lifetime, observation, retirement, and admission races without blocking the GUI or lifecycle thread.

**Architecture:** A shared package-authority aggregate owns store and installer. A process-level retirement manager retains cleanup contexts beyond Host lifetime, while a duplicated process wait handle observes exit independently. A generation-scoped asynchronous coordinator decision gates GUI attachment.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Test, Windows process/job/ACL APIs, LPAC/AppContainer, CMake/CTest.

---

### Task 1: Shared package authority RED to GREEN

**Files:**
- Create: `apps/host/RuntimePackageAuthority.h`
- Create: `apps/host/RuntimePackageAuthority.cpp`
- Modify: `apps/host/HostApplication.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`

1. Add real launch barriers before first validation and between validation phases,
   destroy the Host, release the barrier, and assert process/temp cleanup and the
   authority destruction counter occurs only after the validator completes.
2. Run the individual tests and verify lifetime assertions fail.
3. Implement `RuntimePackageAuthority` and capture it strongly from lifecycle and
   launch validation work.
4. Re-run the tests and verify they pass.

### Task 2: Stable observer handle RED to GREEN

**Files:**
- Modify: `runtime/sandbox/windows/SandboxLauncher.h`
- Modify: `runtime/sandbox/windows/SandboxLauncher.cpp`
- Modify: `tests/security/tst_sandbox_launcher.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`

1. Add a three-state duplicated wait-handle test and repeated live-cancel test.
2. Verify the current observer can loop after retirement clears the process handle.
3. Add a movable wait-only handle and capture it once before starting observation.
4. Re-run sandbox and production tests.

### Task 3: Host-independent retirement manager RED to GREEN

**Files:**
- Create: `apps/host/WorkerRetirementManager.h`
- Create: `apps/host/WorkerRetirementManager.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`
- Modify: `apps/host/InstalledPackageWorkerLauncher.h`
- Modify: `apps/host/main.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`

1. Inject a cleanup failure, destroy the Host after the first attempt, remove the
   condition, and assert automatic cleanup succeeds without a launcher callback.
2. Verify RED because the launcher currently releases the failed context.
3. Implement bounded manager retries, persistent fatal retention, production
   retry/status/flush/shutdown, and checked application shutdown.
4. Re-run cleanup, destruction, and cancel-repeat tests.
5. Block an in-flight launch, destroy Host, call checked shutdown, and prove the
   shutdown remains pending until launch RAII completion and exact cleanup.
6. Inject retirement thread creation failure and prove the record becomes Fatal,
   counters are rolled back, and production retry reaches an idle checked state.

### Task 4: Attach-before-admission race RED to GREEN

**Files:**
- Modify: `apps/host/InstalledPackageWorkerLauncher.h`
- Modify: `apps/host/InstalledPackageWorkerLauncher.cpp`
- Modify: `apps/host/HostApplication.cpp`
- Modify: `runtime/package/UpdateLifecycleCoordinator.cpp`
- Modify: `tests/integration/update/tst_production_update_runtime.cpp`

1. Activate B in `afterHandshakeBeforeCompletionQueued` and assert A never emits
   ready, attaches a worker context, or exposes capability traffic.
2. Verify RED against the current immediate attach path.
3. Queue a single-use admission request to the lifecycle coordinator, return the
   result to the launcher, and attach only for an accepted current serial/binding.
4. Cover rejection, timeout, replay, and Host destruction; re-run production tests.
5. Destroy Host synchronously from a direct `ready` receiver; start the observer
   before the signal and make the signal the final launcher operation.

### Task 5: Verification and commit

1. Run sandbox/package/update/worker/host targeted tests and timing repeats.
2. Run fresh Debug full and polluted environment tests.
3. Run fresh Release with `BUILD_TESTING=OFF` and prove no test hooks/targets.
4. Audit process, profile, handle, observer/context, and temporary-directory cleanup.
5. Run `git diff --check`, self-review, and create one Task 16 follow-up commit.
