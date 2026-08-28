# App-tab runtime isolation design

## Goal

Give every package App tab an independent runtime tuple: launcher/process,
authenticated IPC session generation, capability runtime, worker surface, and
`WorkerAttemptKey`. Package installation and candidate/LKG authority remain
shared per app through `AppRuntimeCoordinator`; no GUI object reads its
container directly.

## Options considered

1. Keep the current HostApplication singleton and multiplex tab IDs through it.
   This preserves the old API but leaves process, session generation, pending
   capability, and surface ownership coupled. It cannot make late events safe
   for two simultaneous tabs.
2. Turn one launcher into an unbounded process pool. This gives superficial
   concurrency but makes retirement, immutable package guards, and per-tab
   admission hard to prove, and violates the one-launcher-per-App-tab contract.
3. Add one `AppTabRuntimeController` per package tab and keep a narrow active-tab
   compatibility view. This is the selected design: ownership is explicit,
   every callback carries a tab/incarnation or session generation, and the
   existing public accessors can remain useful while callers migrate.

## Ownership and event flow

`HostApplication` owns the shared `RuntimePackageAuthority`, one
`AppRuntimeCoordinator` per app, the gesture router, a bounded authority-drain
executor, and a map of `AppTabRuntimeController` instances. A controller owns
one launcher, one `HostWorkerSessionController`, one generation-bound
`HostCapabilityRuntime`, one process lifetime, and one `WorkerSurface` attached
to its `TabController`.

Lifecycle operations execute on the lifecycle thread. Results are ordered
`AppRuntimeAction` values and are posted to the GUI, where the action's full
tab authority selects exactly one controller. Heartbeat/health observations may
be coalesced by full tab/attempt key; admission, exit, revoke, rollback, and
cleanup events are never coalesced.

The launcher performs the final lease comparison, acquires a short-lived
`UseGuard`, and commits the session/surface attachment only through
`publishIfStillAdmitted`. Revocation therefore wins over a paused attach. Each
controller creates a fresh move-only `SandboxTrustBoundary` over the approved
roots.

## Close/reload contract

Close is nonblocking and ordered: stop admission, revoke gesture/capability,
invalidate session generation, request bounded IPC shutdown, stop process and
surface, submit remaining cleanup to `WorkerRetirementManager`, and report
`Retired` only from the retirement completion. Reload preserves the tab model
and history, advances the runtime incarnation, retires the old tuple, and
requests the current lease. Stop only cancels Starting/Loading; it advances the
incarnation before cancellation so late attach cannot publish.

`VisibilityChanged(active)` is authenticated Host-to-Worker state only. It is
accepted for the current session generation, updates a read-only worker
property, and never grants authority or suppresses heartbeat.

## Compatibility

`UpdateLifecycleCoordinator` becomes a reserved legacy-tab adapter over the
shared app coordinator. Existing lifecycle result/action names and telemetry
remain available, while `AwaitAuthorityDrain` is surfaced without waiting on
the GUI or lifecycle thread. Historical accessors (`workerSurface()`,
`workerSessionController()`, and ready/exited signals) resolve the active tab;
new signals include tab ID, runtime incarnation, lease epoch, and process ID.

## Failure invariants

- A stale tab/incarnation/session generation cannot mutate another tab.
- A pending or timed-out authority drain cannot launch or recover a worker.
- Rollback/reverification failure emits Stop + Isolate and remains sticky
  failed-closed.
- A background tab remains alive and heartbeats continue; only its surface is
  hidden and it receives inactive visibility.
- A failed worker is retired asynchronously; closing one tab never stops a
  sibling.
