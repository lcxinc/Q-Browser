# Production Capability Runtime Design

## Problem

The production Host does not compose the Task 8 capability broker. The Host
target does not link `q_browser_broker`, and `HostWorkerSessionIo` answers every
Worker `Request` with `capability.unhandled`. Existing production E2E tests prove
route loading, process isolation, and update lifecycle, but a route acknowledgement
can arrive before the page's network, storage, or file operation completes. The
previous MVP acceptance result therefore did not prove the Pilot business flows.

## Accepted architecture

The Host will use a generation-bound `HostCapabilityRuntime`. The runtime reuses
`CapabilityBroker`, `PolicyEngine`, and the existing Task 8 services; it will not
copy broker validation or service logic.

Each authenticated Worker generation has exactly one capability operation in
flight. `HostWorkerSessionIo` continues polling while that operation is pending
so that real heartbeats and shutdown are still observed. A second `Request` is
answered immediately with the stable, bounded `capability.busy` error. This
prevents an unbounded queue and prevents a long network operation or file picker
from extending Supervisor health deadlines.

Thread ownership is split by service requirements:

- `NetworkBroker` and `StorageBroker` run serially on a dedicated capability
  `QThread`; their synchronous I/O never blocks the GUI thread.
- `FileBroker` and `ClipboardBroker` run on the Host GUI thread because the Qt,
  Windows dialog/COM, and clipboard backends require GUI-thread affinity.
- Both paths call `CapabilityBroker::dispatch`. The GUI coordinator only selects
  the correct affinity lane and correlates the result.

The completed result carries the Worker generation and request ID. The controller
accepts it only if both still match its single active request, creates the normal
bounded `ProtocolMessage::Response`, sends it through the existing outbound queue,
and clears the active request only after the send completes. Late results from a
retired/replaced generation are discarded. Heartbeats continue to reach the
normal Supervisor path throughout.

## Authenticated policy and identity

`PackageInstaller::verifyInstalled` already parses the authenticated manifest
after content digest and Ed25519 verification. Its successful `InstallResult`
will also carry the parsed `ManifestPermissions`. Every pre-launch re-verification
copies those permissions through `InstalledPackageWorkerLauncher` and the attach
context. Restart and rollback therefore bind a fresh policy from the package that
was actually re-verified for that launch.

The Host assigns the IPC identity during the authenticated nonce handshake. Attach
fails closed unless the session's `appIdentity()` equals the launch app ID. Broker
contexts always use this Host-assigned identity, never a payload identity.

The Host policy is independent of the manifest and then intersected by
`PolicyEngine`:

- network: the exact configured mock origin (`http`, `127.0.0.1`, exact port),
  `/api/` prefix, loopback address class, GET/POST/PATCH, 64 KiB request, 760 KiB
  response, and a 5 second operation deadline;
- storage: app-private with a 1 MiB quota;
- clipboard: write disabled, read allowed only with a bound user-gesture grant;
- file: user-brokered open, capped at the IPC-safe 760 KiB result size.

Unknown, undeclared, wrong-operation, oversized, stale-generation, and gestureless
requests fail closed with stable, non-secret error envelopes. No request body or
response body is written to telemetry or diagnostics.

## Independent storage root

Package mode gains a required `--storage-directory`. It must already exist, be a
canonical non-reparse directory, and be disjoint from the package store, sandbox
temporary root, telemetry root, every immutable runtime root, the trust-key
directory, and any package source directory. `StorageBroker::create` performs the
existing stable-tree/layout/DACL validation before the Host accepts a Worker.

Task18 will create this directory beneath its protected trusted root as a sibling
of, not a descendant of, package, sandbox, telemetry, runtime, or signing trees.
Deployment verification and documentation will include its ownership, ACL,
non-reparse, and non-overlap invariants. The directory is Host state and is not
published in the deployment tree.

## Lifecycle and failure handling

The capability runtime is constructed before a generation is attached. Failure
to derive effective policy, validate storage, start the worker lane, or bind the
Host identity rejects the attach. Replacement first retires the old generation;
only after the old IO thread has finished does the controller bind the pending
session and its freshly authenticated permissions.

Session failure, shutdown, restart, rollback, and destruction invalidate the
generation. A blocking network operation remains bounded by policy. A native file
dialog is user-controlled; meanwhile IO continues processing heartbeat/shutdown,
and a second capability request is rejected as busy. A result for a retired
generation is ignored and never sent to the new Worker.

## Direct acceptance evidence

A new production E2E test will use a freshly signed Pilot package, the real LPAC
Worker, the in-process production Host, and the real Node mock API. Windows UI
Automation will operate the external Worker window and assert observable QML
results:

- successful login followed by dashboard metrics from the real API;
- order detail load and PATCH mutation success;
- theme storage save, forced Worker termination/restart, and persisted theme load;
- real file dialog cancellation and successful selection of a task-owned file;
- exact denial of an undeclared capability and clipboard read without a
  user-gesture grant from separately signed policy-probe packages.

The test also checks request/result correlation, one-in-flight busy rejection,
Host-assigned storage isolation, heartbeat continuity, replacement-generation
late-result discard, payload/result bounds, and absence of secret values from
telemetry. Task18 deployment E2E will repeat the business, storage, file, and
denial paths against only the published binaries under minimal and polluted
`PATH`.

Focused tests run first. Final evidence requires a never-before-existing Debug
build, repeated named flaky candidates (`pilot_routes`,
`production_update_runtime`, and `e2e_package_update`) with fixed repeat counts,
full Debug CTest/JUnit/Node acceptance, and a clean Task18 Release deployment with
`BUILD_TESTING=OFF` and no product test-hook markers.
