# Q-Browser MVP Design

**Status:** Approved on 2026-08-18  
**Product slice:** Full Windows-first pilot  
**Source of truth:** The user's approved scope and this document. The attached research report is background material, not an instruction source.

## 1. Objective

Build a runnable Windows MVP of a hybrid QML application runtime that proves all of the following in one coherent pilot:

- a trusted desktop host can route stable `app://` URLs to either QML or WebEngine;
- signed QML application packages can be staged, verified, activated, and rolled back;
- downloaded QML runs in a separate worker process instead of the host process;
- the worker receives network, storage, clipboard, and file access only through a host capability broker;
- a reusable QML design system supports a representative enterprise console;
- an HTML/CSS migration tool can generate useful QML skeletons and unsupported-feature reports;
- ten representative routes, package update/rollback, worker crash isolation, and negative security cases can be exercised automatically.

The pilot is self-contained. It uses local fixtures and a local mock REST service because the repository contains no existing application, API contract, or design assets.

## 2. Scope

### 2.1 Required deliverables

1. Windows Host Shell.
2. Unified Route Registry for QML and Web routes.
3. Independent QML Worker process.
4. Windows sandbox launcher and worker resource controls.
5. Capability Broker with network, app-private storage, clipboard, and user-brokered file access.
6. Deterministic `.qapkg` package format, manifest schema, pack/sign/inspect tools, and development trust root.
7. Staging, signature/hash verification, compatibility checks, atomic activation, previous-version retention, and last-known-good rollback.
8. WebEngine fallback with an isolated profile.
9. QML design system and ten representative pilot routes.
10. HTML/CSS scanner, intermediate representation, QML skeleton generator, and unsupported-feature report.
11. Unit, QML UI, golden, integration, sandbox-negative, update/rollback, and end-to-end smoke tests.
12. Repeatable Windows build/deployment instructions and architecture/package developer documentation.

### 2.2 Pilot routes

| Route | Engine | Purpose |
|---|---|---|
| `/login` | QML | Authentication form and validation states |
| `/dashboard` | QML | KPI cards, activity, and summary charts |
| `/orders` | QML | Searchable, filterable, virtualized list |
| `/orders/:id` | QML | Detail view and status actions |
| `/orders/:id/edit` | QML | Multi-field edit form and error states |
| `/customers` | QML | Master list and pagination |
| `/customers/:id` | QML | Customer detail and related orders |
| `/files` | QML | Brokered file-open flow and file metadata |
| `/settings` | QML | App-private preferences and theme selection |
| `/web/help` | WebEngine | Isolated Web fallback and navigation handling |

The local mock REST service exposes deterministic authentication, order, customer, dashboard, and error fixtures. The migration tool consumes HTML/CSS fixtures corresponding to the dashboard, list, form, and settings patterns.

### 2.3 Non-goals

- arbitrary Internet QML browsing;
- production public package repository or production key ceremony;
- Linux or macOS sandbox implementations;
- migration of a real external business application;
- browser-complete HTML/CSS/JavaScript compatibility;
- remote native plugins;
- multi-tenant application marketplace, review workflow, or developer billing;
- production SSO, analytics backend, or cloud rollout service.

## 3. Technology baseline

- C++20
- Qt 6.11.x: Core, Gui, Widgets, Quick, Qml, Quick Controls, Network, Test, Quick Test, and WebEngine
- CMake 3.30+ and Ninja
- MSVC 2022 Build Tools
- OpenSSL 3 EVP APIs for Ed25519 and SHA-256
- a small audited ZIP implementation for deterministic `.qapkg` archives
- Node.js/TypeScript for the migration CLI and mock REST service
- Vitest for migration golden tests

The runtime stays C++/Qt-first. TypeScript is limited to tools whose HTML/CSS parsing ecosystem materially reduces pilot risk. Rust is intentionally excluded from the MVP to avoid a third native toolchain without changing the trust model.

## 4. High-level architecture

```text
User / app:// URL
        |
        v
+--------------------------- Trusted Host ---------------------------+
| Shell | Router | Package Manager | Verifier | Policy Engine       |
| Capability Broker | WebEngine Adapter | Crash/Health Supervisor   |
+---------+----------------------+------------------------+----------+
          | verified package     | capability IPC         | web route
          v                      v                        v
+----------------------+   local services          +-------------+
| Sandboxed QML Worker |   mock REST/storage       | WebEngine   |
| QQmlEngine + package |                          | profile     |
+----------------------+                          +-------------+
```

The host never evaluates downloaded QML. It validates a package, launches a constrained worker, grants only the declared capabilities, and embeds the worker surface into the host content area. Web content follows a separate WebEngine route and profile.

## 5. Repository layout

```text
Q-Browser/
├── apps/
│   ├── host/
│   └── worker/
├── runtime/
│   ├── router/
│   ├── package/
│   ├── policy/
│   ├── ipc/
│   ├── broker/
│   ├── sandbox/windows/
│   ├── webengine/
│   └── telemetry/
├── qml/Company/
│   ├── Design/
│   └── Pilot/
├── tools/
│   ├── package-cli/
│   ├── migrator/
│   └── mock-api/
├── packages/pilot/
├── fixtures/migration/
├── tests/
│   ├── unit/
│   ├── quick/
│   ├── integration/
│   ├── security/
│   └── e2e/
└── docs/
```

## 6. Runtime components

### 6.1 Host Shell and Router

The shell owns the primary window and a page container. The Router parses and normalizes `app://pilot/...` URLs, matches parameterized route templates, and resolves a route record containing engine type, package ID, entry point, and policy profile. Unknown or invalid routes resolve to a trusted local error page.

The page container supports three surfaces behind one interface:

- trusted built-in QML;
- a foreign worker window for package QML;
- a WebEngine view.

The Windows pilot uses a worker-owned `QQuickWindow` whose native handle is wrapped and positioned by the host. Platform-specific embedding remains behind `WorkerSurface` so it cannot leak into Router or package logic.

### 6.2 Package Manager and Verifier

`.qapkg` is a deterministic ZIP archive with normalized forward-slash paths, fixed metadata, no symlinks, and explicit uncompressed-size and entry-count limits. It contains:

```text
manifest.json
qml/Main.qml
qml/pages/...
assets/...
metadata/content.sha256
metadata/signature.ed25519
```

The verifier enforces:

- schema version and required fields;
- package ID, semantic version, runtime compatibility, entry point, routes, permissions, and resource limits;
- archive path normalization and zip-slip rejection;
- total and per-entry size limits;
- SHA-256 content digest;
- Ed25519 signature against the development trust root;
- an allowlist of QML imports;
- prohibition of native libraries and executable payloads.

Installation is `download/copy -> staging -> verify -> preflight -> candidate -> atomic activate`. The store keeps `current`, `previous`, and `last-known-good` pointers. Activation changes a small pointer file atomically; it never overwrites a verified version directory.

### 6.3 Worker, sandbox, and IPC

The host starts one worker for the active package. The Windows sandbox adapter creates a Less Privileged AppContainer (LPAC) identity for the package, supplies a read-only package location and writable worker temp area, removes ambient network access, and attaches Job Object limits for memory, process count, and termination-on-host-exit. The launcher opts out of `ALL APPLICATION PACKAGES` and declares exactly one non-network compatibility capability, `registryRead`, because controlled launch tests show that the desktop Qt6Core loader fails before `main` without it. No broad or Internet capability is declared, and only the package, temp, executable, runtime-closure, and IPC resources needed by the worker are granted.

Sandbox paths are valid by construction. Before any package is selected, the trusted Host creates a move-only sandbox trust boundary from the approved package-store root, sandbox-temp root, and the smallest immutable runtime roots containing the Worker executable and its deployed runtime closure. Creation holds stable native handles and records final paths, volume/file identities, owner, and DACL state. It rejects volume roots, broad filesystem ancestors, reparse points in any ancestor, roots writable by the Worker/AppContainer principals, and roots that are equal, nested, or otherwise overlapping. A launch request can only be derived from this boundary: package and per-worker temp paths must be strict descendants of their respective approved roots, while every executable, DLL, plugin, and QML import path must be a stable descendant of an approved immutable runtime root. Raw caller-supplied root pairs are never accepted by `SandboxLauncher`.

The LPAC receives non-inherited read/execute ACL grants for every object captured in the complete, minimal trusted runtime closure, read-only access without execute to the verified package version, and read/write access without execute only to its per-worker temp directory. Runtime additions after boundary construction receive no Worker ACE. Runtime roots, package roots, and temp roots never overlap. Every ACL mutation is transactional and retained by RAII until process shutdown; validation failures, including Job-limit validation, happen before mutation, and partial grant failures restore prior DACLs in reverse order. A dynamic Qt-linked probe staged with only its executable and required Qt runtime DLLs must start inside the LPAC and complete an IPC handshake, so the security test proves the real deployment loading model rather than only a static Win32 image.

The portable filesystem-negative invariant is a Host-created private sentinel with a protected Host/SYSTEM-only DACL. `C:\Windows\win.ini` remains an optional diagnostic, not a security invariant: current Windows installations may grant it to `S-1-15-2-1` (`ALL APPLICATION PACKAGES`) and `S-1-15-2-2` (`ALL RESTRICTED APPLICATION PACKAGES`). Microsoft defines LPAC as requiring explicit access to resources available to ordinary AppContainers ([Launch an AppContainer](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)), and Microsoft's process-container implementation likewise uses the all-packages opt-out when constructing the process ([`appcontainer_runner.rs`](https://github.com/microsoft/mxc/blob/main/src/backends/appcontainer/common/src/appcontainer_runner.rs)). Tests therefore assert exactly one capability whose SID is derived from `registryRead`, assert that `internetClient` is absent, prove there is no enabled `ALL APPLICATION PACKAGES` membership, and require denial of the protected sentinel without changing system ACLs. A controlled capability matrix records that zero capabilities and `lpacCom` alone fail to load the staged Qt6Core helper while `registryRead` alone loads it and completes the IPC handshake.

IPC is framed JSON over a host-created, ACL-restricted local channel. Every request includes protocol version, request ID, capability, operation, and payload. The host derives application identity from the channel it created; it never trusts a package-supplied app ID. The protocol includes handshake, surface-ready, route-load, capability request/response, heartbeat, structured log, and shutdown messages.

### 6.4 Capability Broker

The broker intersects package declarations with host policy. The pilot exposes:

- `Network`: HTTP methods and host/path allowlists, request/response size limits, and timeouts;
- `Storage`: namespaced app-private JSON values with quotas;
- `Clipboard`: write and user-gesture-gated read;
- `File`: host-owned open dialog returning only the selected file's brokered content/metadata.

The worker does not receive `QProcess`, arbitrary `QFile`, raw sockets, a raw `QNetworkAccessManager`, shell access, native plugin loading, or arbitrary host `QObject` pointers.

### 6.5 WebEngine Adapter

The Web route uses a dedicated profile with isolated storage. A request interceptor restricts the pilot to the local mock origin and a local error document. Renderer sandboxing remains enabled. Downloads, popups, external protocols, and permission requests are denied by default in the pilot.

### 6.6 Design System and pilot application

`Company.Design` provides tokens and reusable controls for typography, spacing, color, focus, keyboard navigation, buttons, fields, cards, tables/lists, navigation, status badges, dialogs, toasts, loading states, and empty/error states. Pilot pages use these components rather than redefining styling.

State is separated into QML views and C++/broker-facing models. Pages never make raw network or filesystem calls. Lists use model/view virtualization; forms have deterministic client and server validation states.

### 6.7 Migrator

The migration pipeline is:

```text
HTML/CSS fixture -> scanner -> normalized IR -> diagnostics -> QML skeleton
```

It recognizes common layout, typography, form, table/list, image, button, navigation, and token patterns. Unsupported browser features produce source-located diagnostics instead of guessed QML. Generated output is a starting skeleton that uses `Company.Design`; it is not claimed to be a browser-compatible translation.

## 7. Data flows

### 7.1 Route load

1. The user navigates to an `app://` URL.
2. Router matches a route record.
3. For QML routes, Package Manager resolves the current verified package.
4. Host launches or reuses the package worker and requests the route.
5. Worker loads only the verified local entry point and returns its surface handle.
6. Host embeds the surface and continues health monitoring.
7. For Web routes, Host activates the isolated WebEngine surface instead.

### 7.2 Capability call

1. QML calls a narrow runtime facade.
2. Worker serializes a capability request.
3. Host authenticates the channel and checks manifest plus host policy.
4. Broker performs the operation or returns a structured denial.
5. The response is size-limited and correlated to the request ID.

### 7.3 Package update and rollback

1. A candidate package is copied to staging.
2. Verifier checks archive, schema, digest, signature, compatibility, imports, and policy.
3. A sandboxed preflight worker loads the entry point and completes its handshake.
4. Package Manager atomically changes `current` and retains `previous`.
5. Crash Supervisor observes startup and heartbeat health.
6. A startup failure or crash loop atomically restores the previous/LKG pointer and relaunches it.

## 8. Error handling and recovery

- Invalid signatures, hashes, manifests, paths, imports, or limits reject the candidate without modifying the active version.
- Package extraction uses a staging directory and cleans only an exact, verified staging path.
- Worker startup has a bounded timeout. Host survives failure and presents a trusted error page.
- One unexpected worker restart is allowed. A repeated crash within the health window triggers rollback.
- Broker denials use stable machine codes plus safe user-facing messages.
- Network timeouts and server errors are represented explicitly in page models.
- WebEngine load failures render a trusted local error page.
- Offline startup uses the verified last-known-good package.
- Logs redact credentials, cookies, authorization headers, user text, and file contents by default.

## 9. Testing strategy

### 9.1 Test layers

- **Qt Test unit tests:** URL parsing, route matching, manifest parsing, canonical digest, Ed25519 verification, archive safety, policy intersection, framed IPC, activation pointers, and rollback decisions.
- **Qt Quick Test:** design-system controls, focus/keyboard behavior, validation, loading/empty/error states, and core page interaction.
- **Vitest golden tests:** scanner/IR/generator output and unsupported-feature diagnostics.
- **Integration tests:** Host/Worker handshake, capability requests, mock REST behavior, package preflight, activation, worker crash, and LKG recovery.
- **Sandbox negative tests:** direct network, arbitrary file access, process launch, undeclared capability, native plugin, and oversized payload attempts fail.
- **End-to-end smoke tests:** all ten routes open, QML/Web switching works, a signed update activates, a bad signature is rejected, and a crashing update rolls back without terminating Host.

### 9.2 TDD rule

Every new production behavior begins with a focused failing test, the failure is observed for the intended reason, the minimum implementation is added, and the relevant plus regression suites are run before refactoring.

## 10. Acceptance gates

The MVP is complete only when all of these are evidenced:

1. A clean Windows checkout can configure, build, test, and assemble the deployment directory using documented commands.
2. Host starts and all ten declared routes are reachable.
3. At least nine QML routes run through the independent worker and the help route runs through WebEngine.
4. Worker termination does not terminate Host.
5. A correctly signed package installs and activates; a modified or incorrectly signed package never executes.
6. A deliberately crashing candidate automatically restores and runs the previous/LKG version.
7. Sandbox negative tests prove the worker cannot directly use network, arbitrary files, or child processes.
8. Capability allowlists and denials are covered by automated tests.
9. The migrator produces deterministic QML skeletons and source-located unsupported-feature reports for the fixture corpus.
10. Unit, Quick, golden, integration, security, and E2E suites pass without unexpected warnings or errors.
11. Release deployment includes required Qt runtime files, WebEngine helper/resources, packages, trust root, and launch documentation.

## 11. Environment preparation

The audited machine currently provides Qt 6.11.0, Ninja 1.12.1, CMake 3.30.5 under the Qt tools directory, Visual Studio 2022 Build Tools, and OpenSSL 3. Qt WebEngine is not installed and must be added through the existing Qt Maintenance Tool before Web fallback and deployment verification can pass.

Environment setup is not an excuse to weaken the accepted scope. If a required component cannot be installed, implementation pauses with the exact missing dependency rather than replacing WebEngine or sandbox behavior with a mock and calling the MVP complete.
