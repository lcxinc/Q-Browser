# Q-Browser MVP Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build and verify the approved Windows-first Q-Browser pilot: trusted Host, sandboxed signed-package QML Worker, capability broker, WebEngine fallback, design system, ten routes, migration CLI, package lifecycle, and automated acceptance evidence.

**Architecture:** A C++20/Qt Host owns routing, package trust, policy, capability execution, WebEngine, and recovery. Downloaded QML runs only in a constrained Windows Worker and communicates over bounded framed IPC; TypeScript is limited to the mock REST service and HTML/CSS migration tools.

**Tech Stack:** Qt 6.11.1, C++20, CMake 3.30, MSVC 2022, Qt Test, Qt Quick Test, Qt WebEngine, Windows AppContainer and Job Objects, OpenSSL 3, miniz 3.1.0, Node.js 24, TypeScript 7.0.2, parse5 8.0.1, css-tree 3.2.1, Vitest 4.1.10.

---

## Execution rules

- Read docs/plans/2026-08-18-q-browser-mvp-design.md before starting.
- Use @superpowers:test-driven-development for every production behavior.
- Use @superpowers:systematic-debugging for every unexpected failure.
- Never load package QML in the Host process.
- Never replace WebEngine, AppContainer, signature verification, security-negative tests, or rollback with mocks and call the MVP complete.
- Commit after every task.
- Keep build output under build/, runtime state under .qbrowser-dev/, and generated fixtures under build/generated/.

## Shared commands

Run from L:\project\Q-Browser in PowerShell:

~~~powershell
$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe'
$CTest = 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe'
$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64'
$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64'
& $CMake -S . -B build\dev -G 'Visual Studio 17 2022' -A x64 -DQt6_DIR="$QtRoot\lib\cmake\Qt6" -DOPENSSL_ROOT_DIR="$OpenSslRoot" -DQ_BROWSER_BUILD_WEBENGINE=ON
& $CMake --build build\dev --config Debug --parallel
& $CTest --test-dir build\dev -C Debug --output-on-failure
~~~

## Task 0: Repair and verify the Qt toolchain

**Files:** No repository changes.

**Step 1: Prove WebEngine is currently absent**

Run:

~~~powershell
Test-Path 'E:\DevEnv\qt\6.11.1\msvc2022_64\lib\cmake\Qt6WebEngineWidgets'
~~~

Expected: False before installation.

**Step 2: Install matching Qt and WebEngine packages**

Run:

~~~powershell
& 'E:\DevEnv\qt\MaintenanceTool.exe' --accept-obligations --accept-licenses --confirm-command install qt.qt6.6111.win64_msvc2022_64 extensions.qtwebengine.6111.win64_msvc2022_64
~~~

Expected: installation succeeds. If historical Qt 6.10 metadata is reported corrupt, keep all existing versions and install Qt 6.11.1 through package-manager mode rather than deleting the Qt root.

**Step 3: Verify required modules**

Run:

~~~powershell
$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64'
@('Qt6Core','Qt6Quick','Qt6QuickTest','Qt6Network','Qt6WebEngineCore','Qt6WebEngineWidgets','Qt6WebEngineQuick') | ForEach-Object { [PSCustomObject]@{ Module=$_; Exists=Test-Path "$QtRoot\lib\cmake\$_" } }
~~~

Expected: every Exists value is True.

**Step 4: Verify tools**

Run CMake, Ninja, Node, npm, and OpenSSL version commands. Expected: CMake 3.30.x, Ninja 1.12.x, Node 24.x, npm 11.x, OpenSSL 3.x.

## Task 1: Establish a reproducible build and test skeleton

**Files:**
- Create: .gitignore
- Create: CMakeLists.txt
- Create: CMakePresets.json
- Create: cmake/Dependencies.cmake
- Create: cmake/Warnings.cmake
- Create: tests/CMakeLists.txt
- Create: tests/smoke/tst_build_smoke.cpp
- Create: tools/package.json
- Create: tools/package-lock.json
- Create: tools/tsconfig.json
- Create: README.md

**Step 1: Add the CMake skeleton**

Enable C++20, AUTOMOC/AUTORCC/AUTOUIC, CTest, strict MSVC warnings, and Q_BROWSER_BUILD_WEBENGINE=ON. Find Qt Core, Gui, Widgets, Quick, Qml, QuickControls2, Network, Test, QuickTest, WebEngineCore, WebEngineWidgets, and OpenSSL Crypto.

Pin miniz:

~~~cmake
include(FetchContent)
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 174573d60290f447c13a2b1b3405de2b96e27d6c
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniz)
~~~

**Step 2: Add a smoke test**

~~~cpp
#include <QtTest>
class BuildSmokeTest final : public QObject {
  Q_OBJECT
private slots:
  void qtRuntimeIsUsable() {
    QVERIFY(QVersionNumber::fromString(qVersion()) >= QVersionNumber(6, 11));
  }
};
QTEST_MAIN(BuildSmokeTest)
#include "tst_build_smoke.moc"
~~~

**Step 3: Configure, build, and run the smoke test**

Run the shared configure/build commands, then:

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R build_smoke
~~~

Expected: PASS.

**Step 4: Initialize Node tooling**

Use a private npm workspace with exact devDependencies css-tree 3.2.1, parse5 8.0.1, typescript 7.0.2, and vitest 4.1.10. Scripts: build=tsc -b, test=vitest run, lint=tsc -b --pretty false.

Run:

~~~powershell
npm install --prefix tools --no-audit --no-fund
npm run build --prefix tools
~~~

Expected: lockfile created and build succeeds.

**Step 5: Commit**

~~~powershell
git add .gitignore CMakeLists.txt CMakePresets.json cmake tests tools README.md
git commit -m "build: establish Qt and TypeScript project skeleton"
~~~

## Task 2: Implement normalized application URLs and route matching

**Files:**
- Create: runtime/router/CMakeLists.txt
- Create: runtime/router/AppUrl.h
- Create: runtime/router/AppUrl.cpp
- Create: runtime/router/RouteRecord.h
- Create: runtime/router/RouteRegistry.h
- Create: runtime/router/RouteRegistry.cpp
- Create: tests/unit/router/tst_app_url.cpp
- Create: tests/unit/router/tst_route_registry.cpp
- Modify: tests/CMakeLists.txt

**Step 1: Write failing AppUrl tests**

Cover app://pilot/orders/42?tab=history, wrong schemes and authorities, malformed escapes, dot segments, duplicate slashes, and normalized query preservation.

~~~cpp
QCOMPARE(AppUrl::parse("app://pilot/orders/42?tab=history").path(), QString("/orders/42"));
QVERIFY(!AppUrl::parse("https://example.com").isValid());
QCOMPARE(AppUrl::parse("app://pilot/orders/../settings").error(), AppUrlError::PathTraversal);
~~~

**Step 2: Verify RED**

Build tst_app_url. Expected: FAIL because AppUrl does not exist.

**Step 3: Implement AppUrl**

Use QUrl StrictMode. Require scheme app, configured authority, absolute normalized path, valid encoding, and no dot segments. Never convert decoded query text into file paths.

**Step 4: Verify GREEN**

Run CTest -R app_url. Expected: PASS.

**Step 5: Write failing RouteRegistry tests**

Cover exact and parameter routes, static precedence, engine selection, package ID, duplicate rejection, and not-found.

~~~cpp
registry.add({"/orders/:id", Engine::QmlWorker, "com.qbrowser.pilot", "OrderDetail.qml"});
auto match = registry.match("/orders/42");
QCOMPARE(match.parameters.value("id"), QString("42"));
QCOMPARE(match.record.engine, Engine::QmlWorker);
~~~

**Step 6: Verify RED, implement minimally, and verify GREEN**

Compile templates into segments, reject duplicates, prefer static segments, and return value objects. Run CTest -R "app_url|route_registry". Expected: both PASS.

**Step 7: Commit**

~~~powershell
git add runtime/router tests/unit/router tests/CMakeLists.txt
git commit -m "feat: add normalized app routing"
~~~

## Task 3: Define and validate the package manifest

**Files:**
- Create: runtime/package/CMakeLists.txt
- Create: runtime/package/Manifest.h
- Create: runtime/package/Manifest.cpp
- Create: runtime/package/ManifestError.h
- Create: schemas/qapkg-manifest-v1.schema.json
- Create: tests/unit/package/tst_manifest.cpp
- Create: tests/fixtures/manifests/valid.json
- Create: tests/fixtures/manifests/invalid-*.json
- Modify: tests/CMakeLists.txt

**Step 1: Write failing tests**

The valid fixture includes schemaVersion, appId, semantic version, entryPoint, runtime range, imports, permissions, limits, and routes. Invalid fixtures cover missing fields, invalid IDs/versions, entry traversal, duplicate routes, wildcard hosts, unsupported imports, process=true, and excessive limits.

**Step 2: Verify RED**

Build/run tst_manifest. Expected: FAIL because Manifest does not exist.

**Step 3: Implement strict parsing**

Use QJsonDocument and return either an immutable Manifest or stable ManifestError codes with JSON paths. Reject unknown security-sensitive keys in permissions and limits.

**Step 4: Verify GREEN and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R manifest
git add runtime/package schemas tests/unit/package tests/fixtures/manifests tests/CMakeLists.txt
git commit -m "feat: validate qapkg manifests"
~~~

## Task 4: Add deterministic archive creation and safe extraction

**Files:**
- Create: runtime/package/Archive.h
- Create: runtime/package/Archive.cpp
- Create: runtime/package/ArchiveLimits.h
- Create: tests/unit/package/tst_archive.cpp
- Create: tests/fixtures/archives/README.md
- Modify: runtime/package/CMakeLists.txt

**Step 1: Write failing archive-security tests**

Programmatically create normal, dot-dot, absolute, drive-letter, duplicate-path, symlink, too-many-entry, oversized-entry, aggregate-size, and extreme-compression archives. Assert no output appears outside staging.

**Step 2: Verify RED**

Expected: FAIL because Archive does not exist.

**Step 3: Implement safe inspection/extraction**

Wrap miniz. Accept only forward-slash relative regular files; reject dot/dot-dot/absolute/drive/UNC paths and duplicates; enforce limits before allocation; write with QSaveFile beneath a verified staging root.

**Step 4: Verify GREEN**

Expected: all security cases PASS.

**Step 5: Write a failing deterministic-writer test**

Pack the same tree twice after changing mtimes and assert identical SHA-256.

**Step 6: Implement deterministic writing**

Sort paths bytewise, normalize metadata/timestamps, use fixed compression, and exclude metadata/signature.ed25519 from the signed digest input.

**Step 7: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R archive
git add runtime/package tests/unit/package tests/fixtures/archives
git commit -m "feat: add safe deterministic qapkg archives"
~~~

## Task 5: Add Ed25519 signing, verification, and package CLI

**Files:**
- Create: runtime/package/ContentDigest.h
- Create: runtime/package/ContentDigest.cpp
- Create: runtime/package/SignatureVerifier.h
- Create: runtime/package/SignatureVerifier.cpp
- Create: tools/package-cli/CMakeLists.txt
- Create: tools/package-cli/main.cpp
- Create: tests/unit/package/tst_signature.cpp
- Create: tests/integration/package/tst_package_cli.cpp
- Create: keys/dev/.gitignore
- Create: keys/dev/README.md
- Modify: runtime/package/CMakeLists.txt
- Modify: CMakeLists.txt

**Step 1: Write failing cryptographic tests**

Use an RFC 8032 public vector plus generated temporary keys. Cover valid, changed payload, wrong key, malformed signature, and non-canonical metadata.

**Step 2: Verify RED**

Expected: FAIL because digest/verifier do not exist.

**Step 3: Implement canonical digest and Ed25519**

Use OpenSSL EVP_PKEY_ED25519 and SHA-256. Treat every OpenSSL error as verification failure with a stable safe code.

**Step 4: Verify GREEN**

Expected: cryptographic tests PASS.

**Step 5: Write failing CLI integration tests**

Cover keygen, pack, sign, and inspect JSON. Alter a signed package and expect non-zero exit plus verified=false.

**Step 6: Implement qbrowser-package**

Use QCommandLineParser. Never commit private keys. inspect output must be stable JSON for automation.

**Step 7: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "signature|package_cli"
git add runtime/package tools/package-cli tests keys CMakeLists.txt
git commit -m "feat: sign and inspect qapkg bundles"
~~~

## Task 6: Implement versioned package storage and atomic rollback

**Files:**
- Create: runtime/package/PackageStore.h
- Create: runtime/package/PackageStore.cpp
- Create: runtime/package/ActivationState.h
- Create: runtime/package/ActivationState.cpp
- Create: runtime/package/PackageInstaller.h
- Create: runtime/package/PackageInstaller.cpp
- Create: tests/unit/package/tst_package_store.cpp
- Create: tests/integration/package/tst_package_installer.cpp

**Step 1: Write failing PackageStore tests**

Cover immutable version directories, current/previous/LKG, QSaveFile atomic state, interrupted temporary files, invalid pointer targets, and idempotent activation.

**Step 2: Verify RED, implement, and verify GREEN**

Store versions under apps/<appId>/versions/<version>-<digest>. Resolve every state target beneath the app root. Expected: PackageStore tests PASS.

**Step 3: Write failing installer integration tests**

Cover valid install, invalid signature, incompatible runtime, denied import, preflight rejection, activation, and rollback. Rejected candidates must never change current.

**Step 4: Implement phase orchestration**

Use staging, verify, preflight, candidate, and atomic activate phases. Return InstallResult with phase and stable error. Remove only installer-created exact staging paths.

**Step 5: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "package_store|package_installer"
git add runtime/package tests/unit/package tests/integration/package
git commit -m "feat: activate and roll back verified packages"
~~~

## Task 7: Implement bounded Host and Worker IPC

**Files:**
- Create: runtime/ipc/CMakeLists.txt
- Create: runtime/ipc/FrameCodec.h
- Create: runtime/ipc/FrameCodec.cpp
- Create: runtime/ipc/ProtocolMessage.h
- Create: runtime/ipc/ProtocolMessage.cpp
- Create: runtime/ipc/IpcSession.h
- Create: runtime/ipc/IpcSession.cpp
- Create: runtime/ipc/WinPipeTransport.h
- Create: runtime/ipc/WinPipeTransport.cpp
- Create: tests/unit/ipc/tst_frame_codec.cpp
- Create: tests/unit/ipc/tst_protocol_message.cpp
- Create: tests/integration/ipc/tst_ipc_session.cpp
- Modify: CMakeLists.txt

**Step 1: Write failing frame tests**

Cover fragmented and coalesced frames, zero/oversized length, invalid UTF-8/JSON, unknown type/version, duplicate request IDs, and bounded queued bytes. Format is 4-byte big-endian length plus UTF-8 JSON; maximum payload is 1 MiB.

**Step 2: Verify RED, implement codec/messages, verify GREEN**

Require protocolVersion, type, requestId when applicable, and typed payload validation. Unknown versions close the session.

**Step 3: Write failing session tests**

Use anonymous Windows pipes. Cover launch nonce, Host-assigned app identity, correlation, timeout, peer close, heartbeat, and malformed shutdown.

**Step 4: Implement transport/session**

Host creates pipes and passes Worker ends only. Identity comes from Host launch context, never package payload.

**Step 5: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "frame_codec|protocol_message|ipc_session"
git add runtime/ipc tests/unit/ipc tests/integration/ipc CMakeLists.txt
git commit -m "feat: add bounded host worker protocol"
~~~

## Task 8: Implement policy intersection and capability services

**Files:**
- Create: runtime/policy/CMakeLists.txt
- Create: runtime/policy/HostPolicy.h
- Create: runtime/policy/HostPolicy.cpp
- Create: runtime/policy/EffectivePolicy.h
- Create: runtime/policy/PolicyEngine.h
- Create: runtime/policy/PolicyEngine.cpp
- Create: runtime/broker/CMakeLists.txt
- Create: runtime/broker/CapabilityBroker.h
- Create: runtime/broker/CapabilityBroker.cpp
- Create: runtime/broker/NetworkBroker.h
- Create: runtime/broker/NetworkBroker.cpp
- Create: runtime/broker/StorageBroker.h
- Create: runtime/broker/StorageBroker.cpp
- Create: runtime/broker/ClipboardBroker.h
- Create: runtime/broker/ClipboardBroker.cpp
- Create: runtime/broker/FileBroker.h
- Create: runtime/broker/FileBroker.cpp
- Create: tests/unit/policy/tst_policy_engine.cpp
- Create: tests/unit/broker/tst_capability_broker.cpp
- Create: tests/integration/broker/tst_network_broker.cpp
- Create: tests/integration/broker/tst_storage_broker.cpp

**Step 1: Write failing policy tests**

Effective permissions are the intersection of manifest and Host policy; absent means deny. Cover HTTP method/host/path, storage quota, clipboard gesture, file dialog, payload size, and timeout.

**Step 2: Verify RED, implement, and verify GREEN**

Use explicit allowlist value types. Do not accept package-supplied regex policy.

**Step 3: Write failing broker tests**

Use fake operation backends for dispatch and a real local HTTP test server for network integration. Denied requests must never reach backends.

**Step 4: Implement capability services**

Return stable codes such as capability.denied, network.host_denied, network.timeout, storage.quota, clipboard.gesture_required, and file.cancelled. Namespace storage by Host identity. Host owns the file dialog.

**Step 5: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "policy_engine|capability_broker|network_broker|storage_broker"
git add runtime/policy runtime/broker tests/unit/policy tests/unit/broker tests/integration/broker
git commit -m "feat: broker declared runtime capabilities"
~~~

## Task 9: Launch the Worker in AppContainer and a Job Object

**Files:**
- Create: runtime/sandbox/windows/CMakeLists.txt
- Create: runtime/sandbox/windows/AppContainerProfile.h
- Create: runtime/sandbox/windows/AppContainerProfile.cpp
- Create: runtime/sandbox/windows/AclGrant.h
- Create: runtime/sandbox/windows/AclGrant.cpp
- Create: runtime/sandbox/windows/JobLimits.h
- Create: runtime/sandbox/windows/JobLimits.cpp
- Create: runtime/sandbox/windows/SandboxLauncher.h
- Create: runtime/sandbox/windows/SandboxLauncher.cpp
- Create: tests/helpers/sandbox_probe/CMakeLists.txt
- Create: tests/helpers/sandbox_probe/main.cpp
- Create: tests/security/tst_sandbox_launcher.cpp
- Modify: CMakeLists.txt

**Step 1: Write failing profile/limit tests**

Cover deterministic profile names, SID create/reuse, package read ACL, temp write ACL, kill-on-job-close, process limit=1, and memory limit.

**Step 2: Verify RED and implement wrappers**

Use CreateAppContainerProfile, DeriveAppContainerSidFromAppContainerName, explicit ACL grants, CreateJobObject, SetInformationJobObject, and RAII for every Windows resource.

**Step 3: Write failing launch-negative tests**

`sandbox_probe` attempts allowed package-read/temp-write and denied Host-created protected private-sentinel read, loopback network, `cmd.exe` process, and self-spawn. It also reports `C:\Windows\win.ini` access as a diagnostic only: that file can legitimately grant read access to `S-1-15-2-1` (`ALL APPLICATION PACKAGES`) and `S-1-15-2-2` (`ALL RESTRICTED APPLICATION PACKAGES`) on current Windows, so it is not a portable default-deny assertion. The sentinel uses a protected Host/SYSTEM-only DACL and requires no system ACL change or elevation.

**Step 4: Implement SandboxLauncher**

Use `STARTUPINFOEX`, `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, and `PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY` with `PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT`; inherit IPC handles only, use `CREATE_SUSPENDED`, assign the Job before `ResumeThread`, and declare neither broad nor Internet capabilities. The desktop Qt6Core runtime requires the non-network `registryRead` LPAC compatibility capability: a controlled matrix proves that zero capabilities and `lpacCom` alone fail before `main` with `0xC0000022`, while `registryRead` alone loads Qt6Core and handshakes. Allowlist exactly its derived SID, assert capability count one, assert `internetClient` is absent, and prove the token has no enabled `ALL APPLICATION PACKAGES` membership. The current Windows build reports zero for `TokenIsLessPrivilegedAppContainer` once `registryRead` is present, so the portable invariant is the documented defining LPAC property—`ALL APPLICATION PACKAGES` is disregarded—rather than weakening to an ordinary AppContainer. See Microsoft [Launch an AppContainer](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer) and the official [`appcontainer_runner.rs`](https://github.com/microsoft/mxc/blob/main/src/backends/appcontainer/common/src/appcontainer_runner.rs) opt-out implementation.

Construct launches only through a move-only `SandboxTrustBoundary`. It rejects volume/broad/overlapping/reparse or Worker-writable approved roots, holds stable handles plus file identity and owner/DACL evidence, permits only strict package/temp descendants and executables within a minimal immutable runtime closure, and revalidates everything before transactional ACL mutation. Package grants are read-only without execute, temp grants are read/write without execute, and only each pre-recorded runtime object receives non-inherited read/execute; additions after boundary construction receive no Worker ACE. Validate Job limits before the first grant. Stage only the Qt-linked helper and Qt6Core DLL in the runtime proof. Preserve exact HRESULT/DWORD failures in typed results rather than consulting ambient `GetLastError()` after helper calls.

**Step 5: Verify negative security behavior and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R sandbox
git add runtime/sandbox tests/helpers/sandbox_probe tests/security CMakeLists.txt
git commit -m "feat: isolate workers with Windows AppContainer"
~~~

## Task 10: Build the QML Worker and embedded surface

**Files:**
- Create: apps/worker/CMakeLists.txt
- Create: apps/worker/main.cpp
- Create: apps/worker/WorkerApplication.h
- Create: apps/worker/WorkerApplication.cpp
- Create: apps/worker/RuntimeFacade.h
- Create: apps/worker/RuntimeFacade.cpp
- Create: apps/worker/WorkerWindow.h
- Create: apps/worker/WorkerWindow.cpp
- Create: runtime/worker/CMakeLists.txt
- Create: runtime/worker/WorkerSupervisor.h
- Create: runtime/worker/WorkerSupervisor.cpp
- Create: runtime/worker/WorkerSurface.h
- Create: runtime/worker/WorkerSurface.cpp
- Create: tests/integration/worker/tst_worker_handshake.cpp
- Create: tests/integration/worker/tst_worker_surface.cpp
- Create: tests/integration/worker/tst_worker_crash.cpp
- Modify: CMakeLists.txt

**Step 1: Write failing handshake tests**

Launch a test package and assert nonce handshake, Host-assigned identity, ready, route-load acknowledgement, heartbeat, and clean shutdown. Wrong nonce and direct launch without inherited handles fail closed.

**Step 2: Verify RED**

Expected: FAIL because qbrowser-worker does not exist.

**Step 3: Implement WorkerApplication minimally**

Open inherited transports, complete handshake, create QQmlEngine with package and built-in import paths only, install a restrictive QNetworkAccessManagerFactory, register only RuntimeFacade, and load the verified local entry point.

**Step 4: Verify GREEN**

Expected: handshake tests PASS.

**Step 5: Write failing surface tests**

Assert surface-ready contains a valid Worker-owned HWND, Host wraps it through QWindow::fromWinId and QWidget::createWindowContainer, resize/focus propagate, and Host container closure is safe.

**Step 6: Implement WorkerWindow and WorkerSurface**

Worker owns QQuickView. Host wraps the native window behind WorkerSurface; no native-handle logic is allowed in Router.

**Step 7: Write failing crash tests**

Terminate Worker once and expect restart. Terminate the restarted Worker inside the health window and expect crashLoop plus rollback callback while Host remains alive.

**Step 8: Implement WorkerSupervisor**

Track launch time, heartbeat deadline, restart budget, exit reason, and rollback callback. Do not restart a package after activation state changes.

**Step 9: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R worker
git add apps/worker runtime/worker tests/integration/worker CMakeLists.txt
git commit -m "feat: run package QML in supervised workers"
~~~

## Task 11: Build the Host Shell and WebEngine fallback

**Files:**
- Create: apps/host/CMakeLists.txt
- Create: apps/host/main.cpp
- Create: apps/host/HostApplication.h
- Create: apps/host/HostApplication.cpp
- Create: apps/host/MainWindow.h
- Create: apps/host/MainWindow.cpp
- Create: apps/host/NavigationBar.h
- Create: apps/host/NavigationBar.cpp
- Create: runtime/webengine/CMakeLists.txt
- Create: runtime/webengine/WebSurface.h
- Create: runtime/webengine/WebSurface.cpp
- Create: runtime/webengine/PilotRequestInterceptor.h
- Create: runtime/webengine/PilotRequestInterceptor.cpp
- Create: resources/host.qrc
- Create: resources/web/error.html
- Create: tests/unit/webengine/tst_request_interceptor.cpp
- Create: tests/integration/host/tst_unified_navigation.cpp
- Modify: CMakeLists.txt

**Step 1: Write failing Web interceptor tests**

Allow only the configured local mock origin and trusted qrc error page. Deny external schemes, file URLs, unrelated hosts, popups, downloads, and permission requests.

**Step 2: Verify RED, implement isolated WebSurface, verify GREEN**

Use a dedicated QWebEngineProfile and QWebEnginePage. Keep Chromium sandbox enabled and render failures through the qrc error page.

**Step 3: Write failing unified-navigation tests**

Register Worker and Web routes. Switch between them and assert one active surface, stable URL history, trusted not-found behavior, and isolated resources.

**Step 4: Implement Host Shell**

Use QMainWindow and QStackedWidget for WorkerSurface, WebSurface, and trusted error surface. Delegate engine selection only to RouteRegistry.

**Step 5: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "request_interceptor|unified_navigation"
git add apps/host runtime/webengine resources tests/unit/webengine tests/integration/host CMakeLists.txt
git commit -m "feat: route QML and WebEngine surfaces"
~~~

## Task 12: Add the mock REST service and Web help fixture

**Files:**
- Create: tools/mock-api/package.json
- Create: tools/mock-api/tsconfig.json
- Create: tools/mock-api/src/server.ts
- Create: tools/mock-api/src/fixtures.ts
- Create: tools/mock-api/src/routes.ts
- Create: tools/mock-api/public/help/index.html
- Create: tools/mock-api/test/server.test.ts
- Modify: tools/package.json
- Modify: tools/tsconfig.json

**Step 1: Write failing API tests**

Using built-in fetch, cover login success/failure, dashboard, order list/filter/page, order detail/update, customer list/detail, deliberate 500, timeout, and help HTML.

**Step 2: Verify RED**

Run npm test --prefix tools -- --run mock-api. Expected: FAIL because routes do not exist.

**Step 3: Implement the service**

Use node:http only. Bind 127.0.0.1 on an ephemeral test port, emit deterministic JSON, cap bodies, and never listen on all interfaces.

**Step 4: Verify GREEN and commit**

~~~powershell
npm test --prefix tools -- --run mock-api
git add tools/mock-api tools/package.json tools/tsconfig.json tools/package-lock.json
git commit -m "feat: add deterministic pilot API fixtures"
~~~

## Task 13: Create the QML design system

**Files:**
- Create: qml/Company/Design/CMakeLists.txt
- Create: qml/Company/Design/qmldir
- Create: qml/Company/Design/Theme.qml
- Create: qml/Company/Design/Spacing.qml
- Create: qml/Company/Design/Typography.qml
- Create: qml/Company/Design/AppButton.qml
- Create: qml/Company/Design/AppTextField.qml
- Create: qml/Company/Design/AppCard.qml
- Create: qml/Company/Design/AppTable.qml
- Create: qml/Company/Design/AppNavigation.qml
- Create: qml/Company/Design/AppDialog.qml
- Create: qml/Company/Design/AppToast.qml
- Create: qml/Company/Design/StateView.qml
- Create: tests/quick/CMakeLists.txt
- Create: tests/quick/tst_AppButton.qml
- Create: tests/quick/tst_AppTextField.qml
- Create: tests/quick/tst_AppTable.qml
- Create: tests/quick/tst_StateView.qml
- Modify: CMakeLists.txt

**Step 1: Write failing Quick Tests**

Cover keyboard activation, disabled state, focus visibility, field validation, table empty/selection/virtualization hooks, loading/empty/error state, and light/dark token changes.

**Step 2: Verify RED**

Build qbrowser_quick_tests and run CTest -R quick_design. Expected: FAIL because Company.Design does not exist.

**Step 3: Implement minimum controls**

Use Qt Quick Controls primitives, singleton tokens, accessible names/roles, keyboard focus visuals, and no page-specific colors/spacing.

**Step 4: Verify GREEN and lint**

Run Quick Tests and the generated qmllint target. Expected: PASS without unexpected warnings.

**Step 5: Commit**

~~~powershell
git add qml/Company/Design tests/quick CMakeLists.txt
git commit -m "feat: add accessible QML design system"
~~~

## Task 14: Build the ten-route pilot package

**Files:**
- Create: packages/pilot/manifest.json
- Create: packages/pilot/qml/Main.qml
- Create: packages/pilot/qml/PilotRouter.qml
- Create: packages/pilot/qml/models/RuntimeModels.qml
- Create: packages/pilot/qml/pages/LoginPage.qml
- Create: packages/pilot/qml/pages/DashboardPage.qml
- Create: packages/pilot/qml/pages/OrdersPage.qml
- Create: packages/pilot/qml/pages/OrderDetailPage.qml
- Create: packages/pilot/qml/pages/OrderEditPage.qml
- Create: packages/pilot/qml/pages/CustomersPage.qml
- Create: packages/pilot/qml/pages/CustomerDetailPage.qml
- Create: packages/pilot/qml/pages/FilesPage.qml
- Create: packages/pilot/qml/pages/SettingsPage.qml
- Create: packages/pilot/assets/*
- Create: tests/quick/pilot/tst_LoginPage.qml
- Create: tests/quick/pilot/tst_OrdersPage.qml
- Create: tests/quick/pilot/tst_OrderEditPage.qml
- Create: tests/quick/pilot/tst_SettingsPage.qml
- Create: tests/integration/pilot/tst_pilot_routes.cpp

**Step 1: Write a failing route-inventory test**

Require /login, /dashboard, /orders, /orders/:id, /orders/:id/edit, /customers, /customers/:id, /files, /settings as Worker routes and /web/help as Host Web route.

**Step 2: Verify RED and add route skeletons**

Expected: FAIL because the pilot package does not exist. Add Main, PilotRouter, and placeholder pages using RuntimeFacade only.

**Step 3: Verify route inventory GREEN**

Expected: all ten patterns resolve to intended pages/engines.

**Step 4: Add failing page tests**

Cover login validation, dashboard loading/error, order filter/detail/edit and server errors, customer relations, brokered file cancellation/success, and persisted settings.

**Step 5: Implement page behavior test by test**

Use Company.Design. No XMLHttpRequest, WorkerScript networking, file URL, or dynamic remote import is allowed.

**Step 6: Verify Quick, integration, and qmllint**

Expected: PASS and source-policy checks find no raw network/filesystem use.

**Step 7: Pack and sign**

Create build/generated/com.qbrowser.pilot-1.0.0.qapkg and inspect it with the development public key. Expected: verified=true and manifest matches source.

**Step 8: Commit**

~~~powershell
git add packages/pilot tests/quick/pilot tests/integration/pilot
git commit -m "feat: add the ten-route pilot console"
~~~

## Task 15: Implement the HTML/CSS migration CLI

**Files:**
- Create: tools/migrator/package.json
- Create: tools/migrator/tsconfig.json
- Create: tools/migrator/src/types.ts
- Create: tools/migrator/src/scanner.ts
- Create: tools/migrator/src/css.ts
- Create: tools/migrator/src/ir.ts
- Create: tools/migrator/src/diagnostics.ts
- Create: tools/migrator/src/generator.ts
- Create: tools/migrator/src/cli.ts
- Create: tools/migrator/test/scanner.test.ts
- Create: tools/migrator/test/generator.test.ts
- Create: tools/migrator/test/diagnostics.test.ts
- Create: fixtures/migration/dashboard/*
- Create: fixtures/migration/list/*
- Create: fixtures/migration/form/*
- Create: fixtures/migration/settings/*
- Create: tests/golden/migrator/*
- Modify: tools/package.json
- Modify: tools/tsconfig.json

**Step 1: Write failing scanner and IR tests**

Cover semantic headings/nav/sections, forms, tables/lists, images, CSS variables, flex/grid basics, typography, spacing, deterministic IDs, and source locations.

**Step 2: Verify RED**

Run npm test --prefix tools -- --run migrator. Expected: FAIL because scanner/IR do not exist.

**Step 3: Implement scanner and IR**

Use parse5 sourceCodeLocationInfo and css-tree. Never execute source scripts.

**Step 4: Verify GREEN**

Expected: scanner/IR tests PASS.

**Step 5: Write failing generator/diagnostic golden tests**

Generated QML uses Company.Design and Qt Layouts. script, canvas, iframe, contenteditable, complex selector, animation, and unsupported layout yield source-located diagnostics instead of guessed output.

**Step 6: Implement generator and CLI**

Commands are qbrowser-migrate scan <input> --json <ir.json> and qbrowser-migrate generate <input> --output <dir> --report <report.json>. Sort output and diagnostics.

**Step 7: Verify deterministic golden output and commit**

Run twice and compare byte-for-byte.

~~~powershell
npm test --prefix tools -- --run migrator
git add tools/migrator tools/package.json tools/tsconfig.json tools/package-lock.json fixtures/migration tests/golden
git commit -m "feat: generate QML migration skeletons"
~~~

## Task 16: Complete update, crash-loop, and LKG integration

**Files:**
- Create: runtime/telemetry/CMakeLists.txt
- Create: runtime/telemetry/SafeEvent.h
- Create: runtime/telemetry/SafeEvent.cpp
- Create: runtime/telemetry/EventRecorder.h
- Create: runtime/telemetry/EventRecorder.cpp
- Create: tests/unit/telemetry/tst_safe_event.cpp
- Create: tests/integration/update/tst_update_lifecycle.cpp
- Create: tests/integration/update/tst_crash_rollback.cpp
- Create: tests/integration/update/tst_offline_lkg.cpp
- Modify: runtime/worker/WorkerSupervisor.cpp
- Modify: runtime/package/PackageInstaller.cpp
- Modify: apps/host/HostApplication.cpp

**Step 1: Write failing redaction tests**

Feed headers, token, cookie, password, user text, and file content into events. Assert serialization contains redaction markers and no secrets.

**Step 2: Verify RED, implement safe structured events, verify GREEN**

Allow only timestamp, appId, packageVersion, phase, code, durationMs, routeTemplate, and numeric metrics.

**Step 3: Write failing lifecycle tests**

Install healthy 1.0.0; activate 1.1.0; reject tampered 1.2.0; activate signed crashing 1.2.0; restart once; detect crash loop; restore 1.1.0; restart offline from LKG; recover interrupted activation.

**Step 4: Connect installer, supervisor, telemetry, and Host**

Mark healthy only after handshake plus health window. Change state before rollback relaunch.

**Step 5: Verify and commit**

~~~powershell
& $CTest --test-dir build\dev -C Debug --output-on-failure -R "safe_event|update_lifecycle|crash_rollback|offline_lkg"
git add runtime/telemetry runtime/worker runtime/package apps/host tests/unit/telemetry tests/integration/update
git commit -m "feat: recover packages through last known good"
~~~

## Task 17: Add security and end-to-end acceptance tests

**Files:**
- Create: tests/security/tst_malicious_package.cpp
- Create: tests/security/tst_capability_escape.cpp
- Create: tests/security/tst_worker_api_surface.cpp
- Create: tests/e2e/CMakeLists.txt
- Create: tests/e2e/TestEnvironment.h
- Create: tests/e2e/TestEnvironment.cpp
- Create: tests/e2e/tst_host_routes.cpp
- Create: tests/e2e/tst_web_fallback.cpp
- Create: tests/e2e/tst_package_update.cpp
- Create: tests/e2e/tst_host_survives_worker_crash.cpp
- Create: scripts/run-acceptance.ps1
- Modify: tests/CMakeLists.txt

**Step 1: Write the malicious-package matrix**

Include wrong key, changed signed QML, zip slip, decompression limits, native DLL, forbidden import, remote URL import, undeclared capability, direct network/file/process, oversized IPC, malformed frames, and replayed IDs.

**Step 2: Run and verify intended RED**

Each test must fail because enforcement/harness is missing, not because the fixture cannot launch.

**Step 3: Add missing enforcement through focused TDD**

Do not weaken assertions.

**Step 4: Add E2E lifecycle tests**

Start mock-api on loopback, generate dev keys, pack/sign/install pilot, launch Host automation mode, navigate ten routes, switch QML/Web/QML, update, tamper, crash, and rollback.

**Step 5: Implement run-acceptance.ps1**

It configures/builds, runs every CTest group, runs Node tests, packages/inspects pilot, runs windeployqt, and verifies deployment. Any skipped required group returns non-zero.

**Step 6: Run full acceptance and commit**

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 -Configuration Debug
git add tests/security tests/e2e scripts tests/CMakeLists.txt
git commit -m "test: prove Q-Browser MVP acceptance gates"
~~~

Expected: all suites PASS and report lists ten routes, WebEngine, sandbox negatives, signature rejection, crash isolation, and rollback.

## Task 18: Assemble Windows deployment and documentation

**Files:**
- Create: cmake/Deploy.cmake
- Create: scripts/build-release.ps1
- Create: scripts/create-dev-package.ps1
- Create: docs/architecture/runtime.md
- Create: docs/package-spec/qapkg-v1.md
- Create: docs/security/threat-model.md
- Create: docs/security/windows-sandbox.md
- Create: docs/development/getting-started.md
- Create: docs/development/migrator.md
- Create: docs/operations/update-rollback.md
- Create: docs/operations/diagnostics.md
- Modify: README.md
- Modify: CMakeLists.txt

**Step 1: Write a failing deployment verifier**

Require Host/Worker executables, Qt DLLs/plugins, QtWebEngineProcess, WebEngine resources/locales/icudtl.dat, OpenSSL runtime when dynamic, signed pilot package, public trust key, docs, and absence of private keys.

**Step 2: Verify RED**

Expected: FAIL because deployment assembly does not exist.

**Step 3: Implement deployment**

Use CMake install plus windeployqt --webengine. Copy signed package and public key only. Generate SHA-256SUMS.

**Step 4: Build Release**

Run scripts/build-release.ps1. Expected: build/release-deploy is complete.

**Step 5: Verify from deployment only**

Launch with source-tree Qt bin paths removed for child processes. Expected: ten routes, WebEngine helper, signed update, and rollback all work.

**Step 6: Complete docs and commit**

Document trust boundaries, package canonical bytes, permissions, sandbox limitations, development key warning, exact commands, migrator, rollback, and safe diagnostics.

~~~powershell
git add cmake/Deploy.cmake scripts docs README.md CMakeLists.txt
git commit -m "docs: ship and operate the Q-Browser pilot"
~~~

## Task 19: Final requirement-by-requirement audit

**Files:**
- Create: docs/verification/mvp-acceptance-report.md
- Modify production only after adding a new focused failing test.

**Step 1: Build the audit matrix**

Compare the approved design, this plan, and selected Full Pilot scope. For every deliverable/gate, record the proving command/test and latest result.

**Step 2: Run clean verification**

Resolve build/dev and build/release-deploy to absolute paths inside the repository before removing them. Then run acceptance and Release build from clean state.

**Step 3: Inspect runtime evidence**

Confirm Host survives Worker termination; Worker token is AppContainer and Job-bound; network/file/process probes fail; help uses WebEngine; Host never loads package QML; tampered execution count is zero; crashing candidate returns to previous healthy version; deployment has no private key.

**Step 4: Record exact evidence**

Write commands, test counts, timestamps, tool versions, deployment path, and unsupported non-goals. Indirect evidence does not pass a gate.

**Step 5: Review and completion**

Use @superpowers:requesting-code-review for the full diff. Fix important findings and rerun affected plus full verification. Then use @superpowers:verification-before-completion and @superpowers:finishing-a-development-branch.

**Step 6: Commit**

~~~powershell
git add docs/verification/mvp-acceptance-report.md
git commit -m "docs: record verified MVP acceptance"
~~~
