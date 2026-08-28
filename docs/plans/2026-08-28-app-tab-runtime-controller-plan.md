# App-tab runtime isolation implementation plan

## Step 1: Protocol and generation RED

Add `VisibilityChanged(active)` to `ProtocolMessage`, enforce exact payload and
Host-to-Worker direction in `IpcSession`, and add the read-only `active`
property to `RuntimeFacade`. Add protocol/session/worker RED cases for stale
or malformed visibility and generation-bound delivery.

## Step 2: Tab surface and session boundaries

Add tab-keyed worker surface attach/detach and tab-bearing navigation/closing
signals to `MainWindow`. Let `TabController` publish active state and worker
metadata through its own incarnation, and make each
`HostWorkerSessionController` use a tab-specific navigation callback.

## Step 3: `AppTabRuntimeController`

Implement the strict per-tab owner around one launcher, session controller,
capability runtime, process lifetime, surface, lease, attempt key, and
incarnation. Route final attach through the launcher `UseGuard` publication
gate. Implement nonblocking close/reload/stop and tab-scoped signals.

## Step 4: Host ownership migration

Create the shared app coordinator and bounded authority-drain executor on the
lifecycle thread. Replace package-mode singleton process/session/capability
fields with a tab-keyed controller map while retaining active-tab compatibility
accessors. Dispatch ordered coordinator actions by tab authority and batch only
heartbeat/health observations.

## Step 5: Legacy adapter and RED update cases

Adapt `UpdateLifecycleCoordinator` to the shared app coordinator for the
reserved legacy tab, preserving existing tests and telemetry while returning
`AwaitAuthorityDrain` nonblocking. Add update tests for multi-tab candidate
rollback, late admission, and permanent timeout failure.

## Step 6: Verification

Build protocol, IPC, host, worker, and update targets in Debug; run the focused
CTest set serially; run the production update suite; run Release host build
with no-test discovery; inspect `git diff --check`, status, and process
hygiene. Commit the implementation as `feat: isolate app runtime per tab`.
