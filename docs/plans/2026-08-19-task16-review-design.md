# Task 16 Review Closure Design

## Decision

Package mode is a production runtime, not a test-configured callback. A validated
`HostRuntimeConfig` is the only package-mode entry point. It owns the trust inputs
(app id, public key, package store, sandbox temp, immutable runtime closure,
absolute Worker executable, telemetry directory, and optional package to install)
and rejects missing, relative, reparse, overlapping, or ambient-PATH-derived
authority. The host may still start in an explicit trusted-shell mode, but that
mode cannot install or launch packages.

`HostApplication` composes and owns `PackageStore`, `PackageInstaller`,
`EventRecorder`, `UpdateLifecycleCoordinator`, and an
`InstalledPackageWorkerLauncher`. The launcher consumes only the verified final
package directory in `UpdateLaunchRequest`, derives an LPAC launch config from the
production `SandboxTrustBoundary`, creates the restricted pipe/session and
`WorkerSurface`, and observes the real process handle. It reports authenticated
attach, heartbeat, and exit with the same activation/attempt key; tests do not
manually report lifecycle events.

## Atomic activation binding

Activation state includes a monotonically increasing persisted generation.
Every attempt captures an `ActivationBinding` containing app id, exact current
version directory (which embeds the content digest), and generation. LKG marking,
rollback, and offline recovery are compare-and-commit operations: under the same
per-app activation `QLockFile`, each operation re-reads and validates state,
compares it to the expected binding, and writes generation+1 atomically. Any
mismatch or lock/storage failure is typed and fail-closed. Thus a stale Worker can
never mark a concurrently activated version healthy, and offline verification of
A cannot launch A after another owner commits B.

## Time and health

Lifecycle deadlines use one injected steady monotonic millisecond source. UTC is
read separately only when constructing `SafeEvent`. Worker supervision and the
coordinator never accept wall-clock time as lifecycle input. One coordinator
helper owns the healthy transition, compare-and-commit LKG update, and one Healthy
event; heartbeat and timer checks both call it, so duplicate or stale observations
cannot commit or record twice. Telemetry results are always ignored.

## Asynchronous Host ownership

There is no GUI-to-lifecycle or lifecycle-to-GUI blocking queued connection.
Coordinator launch/stop decisions produce generation-tagged queued requests.
The GUI-side launcher completes attach/detach asynchronously and queues a typed
result back to the lifecycle owner. Superseded generations are ignored. Shutdown
first closes admission and cancels generations on the GUI thread, then requests
thread quit; late launch/exit callbacks use guarded QObject ownership and cannot
touch destroyed Host state. Host destruction never waits for a lifecycle callback
that waits on the GUI.

## Verification

Tests use real signed package contents for healthy 1.0/1.1, crashing 1.2, and
recovered 1.1. The production launcher must prove its actual executable, package
final path, process identity, authenticated session, surface, automatic process
exit observation, and capability request. Separate deterministic tests cover
double-store compare-and-commit races, offline A-to-B races, wall-clock jumps,
single Healthy emission, shutdown during rollback/relaunch, configuration and
reparse failures, and production CLI install/offline smoke. Debug full, polluted
environment, repeated LPAC timing, and Release `BUILD_TESTING=OFF` remain gates.
