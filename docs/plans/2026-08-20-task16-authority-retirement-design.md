# Task 16 Authority, Retirement, and Admission Design

## Decision

Package authority is one shared object. `RuntimePackageAuthority` constructs and
owns `PackageStore` before `PackageInstaller`, exposes only the package operations
needed by the Host runtime, and is captured strongly by the lifecycle runtime and
every asynchronous validator. No `PackageInstaller` can outlive its store.

Worker retirement is independent of `HostApplication` and
`InstalledPackageWorkerLauncher`. A process-wide `WorkerRetirementManager` owns a
generic cleanup attempt that captures the complete launch retirement context.
It performs bounded exponential retries, retains persistent failures for an
explicit production retry, and provides checked status, flush, and shutdown APIs.
Destroying a Host only closes admission and schedules retirement; it never drops
process, ACL, or stable temporary-directory authority.

Cancellation registers retirement with the manager immediately, even while the
launch RAII scope is still running. The manager-owned cleanup attempt waits off
the GUI/lifecycle threads for that scope to finish, so checked shutdown cannot
observe an empty manager between Host destruction and retirement registration.
Thread-start failure atomically transitions the retained record to `Fatal`; a
successful flush does not return until its completion callback has returned.

Each process observer duplicates one wait-only process handle before observation.
It waits on that immutable handle and reports `Finished`, `Timeout`, or `Error`;
closing the `SandboxProcess` cannot turn observation into an invalid-handle loop.
The retirement manager remains the sole cleanup owner.

Authenticated IPC is necessary but not sufficient for GUI admission. After the
post-handshake package validation, the launcher submits a generation-scoped
admission request to the lifecycle thread. The coordinator compares the expected
activation binding under the package-store lock and returns asynchronously. Only
an accepted, current, single-use result may attach the session/surface and emit
`ready`; rejection, timeout, replay, or Host destruction retires the payload.
The duplicated observer handle is transferred and its observer started before
`ready`; emitting `ready` is the launch completion's final operation, allowing a
direct receiver to destroy the Host without a post-signal access to the launcher.

## Alternatives considered

Keeping the validator callback and capturing both store and installer would close
one UAF but leave package authority split and easy to misuse. Moving all launch
and cleanup work onto the lifecycle thread would serialize ownership but would
couple blocking process waits to package state. Letting each launch self-retire
would avoid a service but cannot retain persistent cleanup failures after Host
destruction. The selected design keeps authority explicit and blocking cleanup
off both GUI and lifecycle threads.

## Verification

Real signed-package integration tests place barriers before first validation,
between validations, and after handshake before admission. They destroy the Host,
race activation, inject cleanup failures, and repeat live cancellation while
asserting no ready/session/capability exposure, no process/profile/temp residue,
and zero observer/context counts. Sandbox tests exercise the fixed wait handle's
three states. Debug full, polluted environment, timing repeats, and Release
`BUILD_TESTING=OFF` remain gates.
