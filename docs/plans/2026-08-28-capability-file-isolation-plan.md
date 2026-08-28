# Capability and File Isolation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver Task16's owner-bound asynchronous file/capability path, bounded IPC submission, and same-root storage serialization without blocking GUI or sibling tabs.

**Architecture:** Add a process-owned `FileDialogCoordinator`, split file policy into prepare/complete phases, and route completion through `AppTabRuntimeController`'s full authority. Refactor `IpcSession`/`WinPipeTransport` to owned bounded FIFO submission while preserving bounded synchronous Worker wrappers. Serialize storage-root initialization and keep one final `UseGuard`/publication gate for every asynchronous result.

**Tech Stack:** C++20, Qt 6 Widgets/Core, Windows COM STA and message-only window, Windows named-pipe transport, QTest/CTest.

---

### Task 1: Establish RED capability-isolation contract

**Files:**
- Create: `tests/integration/host/tst_browser_capability_isolation.cpp`
- Modify: `tests/integration/host/CMakeLists.txt`
- Modify: `tests/unit/broker/tst_capability_broker.cpp`
- Modify: `tests/unit/host/tst_host_capability_runtime.cpp`

**Steps:**
1. Add tests for two owner authorities: background file requests are denied before dialog creation; only owner tab receives success/cancel/failure; switching and reopening cannot receive a late result; closing calls cancellation exactly once.
2. Add tests for `prepareFileRequest` returning a bounded prepared value and `completeFileRequest` rejecting malformed name/size/content.
3. Add tests for a pending file request not blocking sibling heartbeat/network/storage/clipboard lanes and for stable `file.busy` on a second dialog.
4. Build the new target and run it; record the expected RED compile/API failures.

Run:
`cmake --build build/dev --config Debug --target tst_browser_capability_isolation tst_capability_broker tst_host_capability_runtime -j1`

Expected: FAIL because the coordinator, two-phase broker API, and host pending-request path do not exist.

### Task 2: Add two-phase file validation

**Files:**
- Modify: `runtime/broker/CapabilityBroker.h/.cpp`
- Modify: `runtime/broker/FileBroker.h/.cpp`
- Modify: `runtime/broker/CMakeLists.txt`
- Modify: `tests/unit/broker/tst_capability_broker.cpp`

**Steps:**
1. Define move-only `PreparedFileRequest` and value-only `FileDialogSelection` with maximum-size metadata.
2. Implement `prepareFileRequest(policy,payload)` with existing operation/policy/IPC-bound validation and no UI or blocking I/O.
3. Implement `completeFileRequest(prepared,selection)` with name, size, content, base64, and response-size validation; keep synchronous `invoke` as a compatibility wrapper for tests/legacy callers.
4. Run broker tests and confirm GREEN for pure validation before any host/transport changes.

### Task 3: Implement process-owned STA file coordinator

**Files:**
- Create: `runtime/broker/FileDialogCoordinator.h/.cpp`
- Modify: `runtime/broker/FileDialogTestHooks.h/.cpp`
- Modify: `runtime/broker/CMakeLists.txt`
- Modify: `tests/integration/host/tst_browser_capability_isolation.cpp`

**Steps:**
1. Define opaque operation token, cancellation handle, callback, and coordinator state (`Idle`, `Showing`, terminal).
2. Run `IFileOpenDialog` on a dedicated STA; create a message-only cancel window before `Show()` and retain the exact dialog pointer.
3. Enforce one process-wide dialog, owner/background checks, exact-once terminal callback, and late-callback discard.
4. Keep the selected stable handle/QIODevice on the STA, validate identity/ancestry/size, read maximum+1, encode owned bytes, close it, and return value-only selection.
5. Add deterministic hooks that orchestrate Show/read/encode/cancel without bypassing the authority path; do not expose production test methods.
6. Run coordinator isolation tests; verify the GUI remains responsive while the dialog is blocked.

### Task 4: Route file requests through HostCapabilityRuntime

**Files:**
- Modify: `apps/host/HostCapabilityRuntime.h/.cpp`
- Modify: `apps/host/AppTabRuntimeController.h/.cpp`
- Modify: `apps/host/HostApplication.h/.cpp`
- Modify: `apps/host/HostWorkerSessionController.h/.cpp`
- Modify: `apps/host/HostWorkerSessionIo.h/.cpp`
- Modify: `runtime/broker/CapabilityBroker.h/.cpp`

**Steps:**
1. Inject one `FileDialogCoordinator` into every per-tab runtime and cancel all operations during close/fail-closed.
2. Map operation token to complete `TabCapabilityAuthority`, request ID, and session generation; allow only one pending capability request per session.
3. Queue coordinator callbacks to GUI, re-run `completeFileRequest`, and enqueue an immutable result only when tab/runtime/session/token still match.
4. At actual send dequeue, acquire `UseGuard`, compare full authority, and call `publishIfStillAdmitted`; discard revoked or stale completions.
5. Add RED/GREEN tests for rollback/close races, reopened tabs, delayed capability completion, and sibling progress.

### Task 5: Add owned bounded asynchronous IPC submission

**Files:**
- Modify: `runtime/ipc/IpcSession.h/.cpp`
- Modify: `runtime/ipc/WinPipeTransport.h/.cpp`
- Modify: `apps/host/HostWorkerSessionIo.h/.cpp`
- Modify: `apps/host/HostWorkerSessionController.h/.cpp`
- Modify: `tests/integration/ipc/tst_ipc_session.cpp`

**Steps:**
1. Add `submitSend` with owned frame bytes and exactly-once completion/cancellation state.
2. Implement per-transport FIFO with a persistent ordered writer, 64-frame and 4 MiB limits, and close-time completion.
3. Preserve Worker-side synchronous helpers as bounded waits on the dedicated IO thread only.
4. Retain Host `UseGuard` through transport completion; ensure queue-full never reorders or drops accepted frames.
5. Add tests for FIFO, exact limits, blocked peer responsiveness, cancellation, close/join, and revocation after queueing but before final publication.

### Task 6: Serialize same-root storage initialization

**Files:**
- Modify: `runtime/broker/StorageBroker.h/.cpp`
- Modify: `runtime/broker/StorageTestHooks.h/.cpp`
- Modify: `runtime/broker/CMakeLists.txt`
- Modify: `tests/integration/broker/tst_storage_broker.cpp`

**Steps:**
1. Key a process-wide lock by canonical app root and retain the existing stable inter/intra-process lock.
2. Revalidate root identity and DACL immediately before each transaction; never let broker retirement remove a sibling's root.
3. Add deterministic same-root concurrent initialization/update tests and Release canary coverage.

### Task 7: Integration and regression verification

**Files:**
- Modify: `tests/integration/host/CMakeLists.txt`
- Modify: `tests/e2e/tst_e2e_pilot_capabilities.cpp` (only if the existing harness needs new assertions)

**Steps:**
1. Build Debug targets: `tst_browser_capability_isolation`, `tst_capability_broker`, `tst_host_capability_runtime`, `tst_ipc_session`, `tst_network_broker`, `tst_storage_broker`, `tst_e2e_pilot_capabilities`.
2. Run CTest with `-C Debug --output-on-failure -j1` for those targets plus existing Task15 protocol/unified/update tests.
3. Run the production update runtime test after the final host relink.
4. Build Release `q_browser_host` and run `git diff --check`; inspect that no test hook or cache is staged.

### Task 8: Commit the verified slice

**Steps:**
1. Stage only Task16 source, tests, CMake, and this plan; exclude `.task12-npm-cache/`.
2. Commit with `feat: isolate capability work by tab`.
3. Verify `git status --short`, `git log --oneline -3`, and the Debug/Release evidence before reporting completion.
