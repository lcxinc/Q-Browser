# Q-Browser MVP acceptance report

Status: **PASS**

This report audits the approved Full Pilot design, Tasks 0–18, and the
production capability-runtime follow-up against source commit
`cdda98bbc31d560e41f7f85c4b7688545a0acd5b`. The final guarded Release run and
read-only verification completed on 2026-08-23 from 10:40 through 10:56 China
Standard Time (UTC+08:00, Asia/Shanghai). The report-only commit is
intentionally not part of the audited executable input.

## Browser-style title-area follow-up (2026-09-04)

The browser-style title-area change was automatically verified from source
commit `fef635b` in the isolated worktree
`L:\project\Q-Browser\.worktrees\browser-style-frameless-tabs`. This follow-up
does not replace the immutable Task 0–18 deployment evidence below. The
automated results and the subsequent Windows UI observations are reported
separately.

| Check | Fresh result |
|---|---|
| Focused Debug build | `tst_browser_chrome`, `tst_browser_shell`, and `qbrowser-host` built successfully; 9.846 s |
| Focused Debug CTest | `browser_chrome` and `browser_shell`: 2/2 passed, 0 failed; 35.26 s |
| Complete Debug build | Passed; 273.894 s. The changed pilot E2E target was rebuilt again after its test-only coordinate fix. |
| Complete Debug CTest | 55/55 passed, 0 failed; 609.49 s |
| Node/Vitest | 9/9 files and 172/172 tests passed; Vitest 42.38 s, complete command 44.281 s |
| Release product configure | Independent `build/release-browser-tabs-product` configured with `BUILD_TESTING=OFF`; 15.262 s |
| Release Host build | `qbrowser-host.exe` built successfully; 55.566 s; artifact size 1,858,048 bytes |
| Release test inventory | `ctest -C Release -N` reported `Total Tests: 0` |

The first complete Debug CTest run retained one real RED:
`e2e_pilot_capabilities` sampled a centered login control at a fixed y-coordinate
from the previous 1100x647 Worker surface. The 42-pixel title row changed the
observed Worker surface to 1106x606, so the old sample hit the background. Commit
`fef635b` derives the login and centered clipboard-control points from the actual
surface center, verifies that every point is in bounds, and retains the color,
focus, real-input, and capability-response assertions. The focused E2E then
passed 1/1 in 33.20 s before the passing full-suite rerun.

The tools worktree initially had no installed dependencies, so the first command
stopped at `tsc` not found. `npm ci --prefix tools` installed the exact lockfile
set (54 packages, 0 reported vulnerabilities). The first post-install Vitest
process then exited before listing tests with Windows fast-fail `0xC0000409`, the
same environmental diagnostic already retained later in this report; an
independent complete rerun produced the 172/172 passing result above. Release
configure also repeated the already-known optional Vulkan-header and private
`Qt6TaskTree` / `Qt6QmlAssetDownloaderPrivate` diagnostics.

### Windows UI smoke observations

Launching the Debug Host directly without the Qt binary directory on `PATH`
first produced loader errors for the Debug Qt WebEngine Widgets/Core DLLs. The
same executable started successfully after applying the Qt `PATH` environment
used by CTest. This was a launch-environment diagnostic, not an observed product
failure.

With `QT_SCALE_FACTOR=1.0`, the captured window measured 1102x728 pixels. No
separate title band was visible: tabs and the Windows caption buttons occupied
the same top row. The New Tab `+` created a tab; dragging a tab did not move the
window; double-clicking unused title-row space maximized and restored the window;
and the maximize, restore, and close caption buttons completed their actions.
Resizing from the native lower-right edge changed the window from 1102x728 to
1002x652 pixels. With eight tabs open, the tab bar exposed both scroll buttons;
the New Tab button and caption buttons remained separate, and a visible inactive
tab could be activated.

With `QT_SCALE_FACTOR=1.25`, the captured window measured 1377x908 pixels. The
title area remained visually integrated, and the tabs, New Tab button, and
caption buttons were clear and did not overlap. The close caption button also
completed its action.

The immediate `sky.drag` automation sequence did not make actual movement of the
window through the unused title area, or drag-to-edge snap, reliably observable.
Those two interactions therefore remain an automation limitation and are not
recorded as manually passed. The exercised implementation path is bounded by the
drag-threshold unit test and the Windows `startSystemMove()` return diagnostic;
the separate maximize/restore, native-edge resize, and caption-button observations
above provide supporting window-integration evidence. No smoke-test screenshot
was added to the repository.

## Audited environment and immutable outputs

| Item | Observed value |
|---|---|
| OS | Windows 10 Pro for Workstations, build 26300, x64 (Qt Test reports the platform family as Windows 11) |
| Compiler | MSVC 19.44.35226.0, toolset 14.44.35207; MSBuild 17.14.40 |
| CMake / CTest | 3.30.5 |
| Ninja | 1.12.1 |
| Qt | 6.11.1, including Quick, QML, Test, Quick Test, and WebEngine |
| OpenSSL | 3.0.16 |
| Node.js / npm | 24.15.0 / 11.13.0 |
| PowerShell | PowerShell Core 7.6.4; Windows PowerShell 5.1.26100.9032 |
| Git | 2.53.0.windows.3 |
| Audited source | `cdda98bbc31d560e41f7f85c4b7688545a0acd5b` |
| Final deployment | `C:\Users\10428\AppData\Local\QBrowserTask18\release-deploy` |
| Deployment manifest SHA-256 | `46BE6743928B339DB4E5E311F49D624CBD842A4C0D18BC287E0A2D29B43B9BDC` |
| Deployment inventory | 392 files, 128 directories, 120 PE files; 391 manifest entries (every file except the manifest itself) |

The final deployment contains no reparse point and exactly one PEM file,
`trust/dev-public.pem`. The trusted deployment verifier proved complete PE
runtime closure, every manifest hash, safe ownership/DACLs, and the absence of
private-key material, test/source residue, and undeclared files.

## Reproduction commands and aggregate results

Commands below were run from
`L:\project\Q-Browser\.worktrees\q-browser-mvp-impl`. LPAC tests were serial;
Debug temporary files stayed on `L:`. The Release script used only its guarded
Task18 root after the absolute cleanup targets were resolved and checked as
strict descendants of that root.

```powershell
# Fresh Debug acceptance for the production capability runtime
$b = 'L:\project\Q-Browser\.worktrees\q-browser-mvp-impl\build\capability-debug-final'
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
  -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File scripts\run-acceptance.ps1 -Configuration Debug -BuildDirectory $b

# Fresh, guarded Task18 Release build, test, deployment-only E2E, and publish
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
  -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File scripts\build-release.ps1 -Clean

# Read-only no-clean reproducibility verification
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
  -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File scripts\build-release.ps1

# Task 0 required-module probe (all seven Exists values were True)
$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64'
@('Qt6Core','Qt6Quick','Qt6QuickTest','Qt6Network','Qt6WebEngineCore',
  'Qt6WebEngineWidgets','Qt6WebEngineQuick') | ForEach-Object {
  [PSCustomObject]@{ Module=$_; Exists=Test-Path "$QtRoot\lib\cmake\$_" }
}

# Independent authoritative verifier (run once for each PATH below)
$deploy = 'C:\Users\10428\AppData\Local\QBrowserTask18\release-deploy'
$verify = 'C:\Users\10428\AppData\Local\QBrowserTask18\control\Deploy.cmake'
$cmake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe'
$minimal = "$(Join-Path $deploy 'host');$env:SystemRoot\System32;$env:SystemRoot"
foreach ($path in @($minimal, "C:\polluted-does-not-exist;$minimal")) {
  $env:PATH = $path
  & $cmake -DQ_BROWSER_DEPLOY_MODE=VERIFY "-DQ_BROWSER_DEPLOY_DIR=$deploy" -P $verify
}
```

| Suite / assertion | Final result |
|---|---|
| Fresh Debug CTest | 44/44 passed, 0 failed |
| Fresh Release CTest | 44/44 passed, 0 failed, 223.42 s |
| Required Debug JUnit | 14 XML files, 176 cases, 0 failures, 0 errors, 0 skipped |
| Required Release JUnit | 14 XML files, 176 cases, 0 failures, 0 errors, 0 skipped, 164.010 s |
| Node/Vitest | 9 files, 172/172 tests passed; final Release run 43.46 s |
| Fresh Task18 Release and atomic publish | PASS; the guarded run completed and published during the 2026-08-23 10:40–10:56+08:00 acceptance window |
| Clean-build artifact identity | The final clean build produced manifest SHA-256 `46BE6743928B339DB4E5E311F49D624CBD842A4C0D18BC287E0A2D29B43B9BDC`; read-only and independent verification preserved it byte-for-byte |
| Deployment-only E2E | `routes=10 networkReads=4 business=login+order+customers+storage+file+clipboard webEngine=deployed update=1.1.0 rollback=1.0.0 paths=minimal+polluted` |
| No-clean verifier | PASS on 2026-08-23; manifest hash and root ACL were byte-for-byte unchanged |
| Independent minimal and polluted PATH verifier | PASS / PASS on the final published tree; manifest remained unchanged |
| Release product build | `BUILD_TESTING:BOOL=OFF`; `ctest -N` reports `Total Tests: 0`; deployed product test-hook marker scan found 0 |

## Task 0–18 deliverable matrix

Every row cites an executable test or verifier, not the existence of a file as
the sole proof.

| Task | Required deliverable / gate | Direct proof and final result |
|---:|---|---|
| 0 | Repair and verify Qt toolchain | The plan's direct probe returned `True` for all seven required modules: `Qt6Core`, `Qt6Quick`, `Qt6QuickTest`, `Qt6Network`, `Qt6WebEngineCore`, `Qt6WebEngineWidgets`, and `Qt6WebEngineQuick`. Both clean configure runs also detected MSVC 19.44, Qt 6.11.1 and OpenSSL 3.0.16; WebEngine was exercised by `e2e_web_fallback`, `request_interceptor`, and the deployed `QtWebEngineProcess.exe`. PASS. |
| 1 | Reproducible CMake/test skeleton | Fresh Debug and Release builds plus `build_smoke`; 44/44 in both configurations. PASS. |
| 2 | Normalized `app://` URLs and route matching | `app_url`, `normalized_path`, `route_registry`, and `pilot_routes::hostInventoryResolvesPatternToPageAndEngine`. PASS. |
| 3 | Strict package manifest | `manifest` and `pilot_routes::packageManifestDeclaresWorkerInventory` / `manifestDeclarationsMatchDirectSourceUsage`. PASS. |
| 4 | Deterministic archive and safe extraction | `archive` plus malicious archive/path/reparse cases in `malicious_package`. PASS. |
| 5 | Ed25519 verification and package CLI | `signature`, `package_cli`, Debug/Release package pack-inspect-sign acceptance, and tampered-package E2E. PASS. |
| 6 | Versioned store, atomic install, rollback | `package_store`, `package_installer`, `update_lifecycle`, `offline_lkg`, `crash_rollback`. PASS. |
| 7 | Bounded Host/Worker IPC | `frame_codec`, `protocol_message`, `ipc_session`, `worker_handshake`, and `worker_api_surface::rejectsOversizedMalformedAndReplayedFrames`. PASS. |
| 8 | Policy intersection and brokers | `policy_engine`, `capability_broker`, `network_broker`, `storage_broker`, `host_capability_runtime`, and `capability_escape::undeclaredCapabilitiesNeverReachServices`; deployed business E2E exercised real network, storage, file, and clipboard brokers. PASS. |
| 9 | LPAC/AppContainer Worker and Job | `sandbox_launcher` JUnit: 17/17 including `launchProbeProvesPositiveAndNegativeBoundaries`, `jobObjectHasKillProcessAndMemoryLimits`, and `closingJobKillsAssignedProcess`. PASS. |
| 10 | Independent QML Worker/surface | `worker_handshake`, `worker_surface`, `worker_crash`, `worker_confinement`, `worker_api_surface`, and `e2e_pilot_capabilities`; real deployed Worker input, route acknowledgements, and capability responses were checked. PASS. |
| 11 | Host shell and WebEngine fallback | `unified_navigation`, `request_interceptor`, `webengine_file_selection`, `e2e_web_fallback`; deployment-only E2E observed the helper command line under the deployment `host` directory. PASS. |
| 12 | Deterministic mock REST and Web help fixture | Node/Vitest mock-api suites are part of the 172/172 run; `/web/help` passed both C++ E2E and deployment-only E2E. PASS. |
| 13 | `Company.Design` system | `quick_design` and the fresh QML deployment scan; design controls compiled and loaded in Debug and Release. PASS. |
| 14 | Ten-route Pilot package | `pilot_routes`, `pilot_qmllint`, `e2e_host_routes`, and deployment-only 10-route E2E; real input proved login, order GET→PATCH, customer list→detail, settings persistence, and native file selection. PASS. |
| 15 | HTML/CSS migration CLI | Migrator Vitest/golden suites within the 172/172 Node run prove deterministic output and source-located diagnostics. PASS. |
| 16 | Update, crash loop, offline LKG, safe telemetry | `update_lifecycle`, `crash_rollback`, `offline_lkg`, `safe_event`, and required `production_update_runtime` JUnit (24/24, no skip). PASS. |
| 17 | Security and end-to-end acceptance | `malicious_package`, `capability_escape`, `worker_api_surface`, all four E2E binaries, required JUnit, and full CTest. PASS. |
| 18 | Windows deployment and documentation | Guarded `build-release.ps1 -Clean`, deployment E2E, adversarial verifier probes, atomic publish, no-clean verification, independent PATH verification, and `BUILD_TESTING=OFF`/0-test product build. PASS. |

## Pilot route matrix

`pilot_routes` proved the registry mapping. Task18 then launched the deployed
Host and LPAC Worker, navigated these concrete URLs, required the expected
surface and route acknowledgement. While the unique bound Worker was suspended,
the Host did not accept or complete the route without its acknowledgement; that
missing-ack check passed ten consecutive times.

| Declared route | Concrete deployment E2E URL | Expected and observed engine |
|---|---|---|
| `/login` | `app://pilot/login` | independent QML Worker |
| `/dashboard` | `app://pilot/dashboard` | independent QML Worker |
| `/orders` | `app://pilot/orders` | independent QML Worker |
| `/orders/:id` | `app://pilot/orders/ORD-1001` | independent QML Worker |
| `/orders/:id/edit` | `app://pilot/orders/ORD-1001/edit` | independent QML Worker |
| `/customers` | `app://pilot/customers` | independent QML Worker |
| `/customers/:id` | `app://pilot/customers/CUS-001` | independent QML Worker |
| `/files` | `app://pilot/files` | independent QML Worker |
| `/settings` | `app://pilot/settings` | independent QML Worker |
| `/web/help` | `app://pilot/web/help` | deployed WebEngine helper |

## Production capability-runtime evidence

The follow-up replaced the earlier test-only broker proof with production Host
composition. The authenticated manifest permission set is attached to the
launched Worker generation, and `HostCapabilityRuntime` intersects each request
before dispatching it to the production broker. Background network/storage work
is bounded; file and clipboard work stays on the GUI thread; one serialized lane
rejects overlap and ignores late results from retired generations.

| Capability / lifecycle | Direct deployed proof | Result |
|---|---|---|
| Network | Login consumed its successor route; order detail issued `GET /api/orders/ORD-1001`, edit issued `PATCH`, and dashboard/customer flows produced four observed reads in total. | PASS |
| Storage | A real settings click stored `theme=dark`; after Host restart the Worker read it, then changed it to `light`. | PASS |
| File | The native Windows dialog was opened and cancelled, reopened with a real click, accepted a selected file, closed, and caused a material Worker-surface update. | PASS |
| Clipboard | An expired grant and an undeclared permission were denied; a foreground input gesture authorized exactly one read; replay was denied. The grant was bound to Worker HWND, PID, generation, and Host foreground root. | PASS |
| Generation / routing | A suspended bound Worker could not acknowledge a route; the Host rejected the missing acknowledgement 10/10 times without changing Worker identity. | PASS |
| Shutdown | Deployed Host clean exits completed in roughly 0.6 s while capability work and retired Worker teardown remained nonblocking. | PASS |

The native automation used only rendered desktop pixels, the deployed Worker
client geometry, real keyboard/mouse input, and the system file dialog. It did
not discover or invoke named QML controls through an accessibility backdoor.

## Design acceptance gates

| Gate | Direct evidence | Result |
|---:|---|---|
| 1 | Fresh Debug acceptance and guarded fresh Task18 Release build/deploy from `cdda98b`; no-clean verification repeated without mutation. | PASS |
| 2 | `e2e_host_routes`, `pilot_routes`, and Task18 deployment marker `routes=10`. | PASS |
| 3 | Nine concrete Worker route acknowledgements plus `/web/help` using the deployed WebEngine renderer. | PASS |
| 4 | `e2e_host_survives_worker_crash::crashingCandidateRollsBackWithoutTerminatingHost`: the Host remained navigable after both forced Worker terminations. | PASS |
| 5 | `e2e_package_update::signedUpdateActivatesAndTamperNeverExecutes`: `1.1.0` activated; tampered `1.2.0` produced exact `signature_invalid`, canary count 0, no ready event, and unchanged current/LKG. | PASS |
| 6 | The same Host-survival E2E forced two crashes of `1.1.0` and observed healthy `1.0.0`; Task18 independently reported `rollback=1.0.0`. | PASS |
| 7 | Sandbox launch probe and capability-escape tests directly denied arbitrary file read, loopback connection, `cmd`/self child creation, package/temp execute, QML raw network, file URL load, and process API access. | PASS |
| 8 | `policy_engine`, `capability_broker`, `host_capability_runtime`, broker tests, deployed network/storage/file/clipboard business flows, and undeclared-capability zero-invocation assertion. | PASS |
| 9 | Migrator Vitest/golden corpus in the final 172/172 Node run. | PASS |
| 10 | Debug and Release unit, Quick, golden, integration, security and E2E suites all passed; diagnostics are classified below. | PASS |
| 11 | Trusted verifier proved Qt/WebEngine resources, packages, public trust root, docs, full PE closure and manifest integrity. | PASS |

## Direct security evidence

- **LPAC/AppContainer and Job:** the live sandbox probe asserted an AppContainer
  token, queried restricted/token capability groups, had exactly one declared
  capability, was not a member of All Application Packages, and returned the
  same AppContainer SID observed by the Host. The Job query directly proved
  `ACTIVE_PROCESS=1`, process-memory limit, and kill-on-close; closing the Job
  terminated its assigned process.
- **Handle and environment inheritance:** the probe inherited only its intended
  IPC handles. A deliberately inheritable sentinel pipe was broken in the
  child, and `Q_BROWSER_SANDBOX_SENTINEL_SECRET` was absent. Task18 also cleared
  all Qt/QML/WebEngine/OpenSSL loader override variables before deployment E2E.
- **Production capability authority:** the production Host constructed the
  policy engine, gesture store, and all four brokers from authenticated manifest
  permissions. Requests were correlated to the admitted Worker generation;
  cancellation and late-result tests proved a retired generation could not
  consume a response.
- **File/network/process denial:** the LPAC probe could read the selected package
  and write its worker temp, but could not read a protected host file, execute
  package/temp files, connect to a reachable loopback listener, or create
  `cmd.exe`/itself. `capability_escape` additionally proved zero listener
  connections, failed external file `Loader`, and undefined `QFile`/`Process`.
- **Worker-only package QML:** the route registry resolved all nine package QML
  routes to `QmlWorker`; deployment E2E required a real Worker route ack and
  Worker surface for each. The Host routed `/web/help` separately. While the
  unique bound Worker was suspended, the Host did not accept or complete the
  route without its acknowledgement; this missing-ack check passed 10/10 times.
  Thus the trusted Host/router never evaluated package QML.
- **WebEngine provenance:** Task18 captured the live renderer command line and
  proved its executable was the deployed
  `release-deploy/host/QtWebEngineProcess.exe`, with deployed resources/locales;
  minimal and polluted PATH runs both passed.
- **Update integrity:** the tamper E2E asserted exact `signature_invalid`, zero
  canary executions, no `1.2.0` ready event, and exactly unchanged current/LKG
  directory-name values. The adversarial deployment verifier also regenerated
  package hashes after tampering and still rejected the signed package.
- **Crash isolation and recovery:** forced termination of the candidate Worker
  left the Host window alive, caused one restart, then restored and ran the
  healthy `1.0.0` LKG after the second crash.
- **Release hygiene:** the published tree has zero reparse points, zero private
  PEMs, zero test/source residue, safe ACLs, 120 closed-over PE files, and a
  complete 391-entry SHA-256 manifest. Adversarial missing/moved dependency,
  reparse, private-key-preamble, package-tamper, parent-replacement, and unsafe
  ACL probes all failed closed.

## Audit REDs and focused remediation

The audit did not suppress failures. Each reproduced RED was reduced before a
minimal change and followed by focused plus full verification:

| RED | Minimal remediation | Focused proof |
|---|---|---|
| Concurrent storage quota updates exhausted a 1 s lock wait under clean Debug load (8/10 failures). | Bounded 10 s serialization wait, commit `42f73bf`. | 10/10 repeat plus `storage_broker` 16/16. |
| Cold Windows LPAC loader work could exceed the original launch/fixture bounds before `main`. | Bounded cold-start observations and launcher phase, commit `e23b171`. | Worker/security affected set 7/7 and crash E2E. |
| The required production lifecycle JUnit still used 30 s observations shorter than the bounded cold launcher. | Test observations raised to 120 s, commit `6826031`. | Required JUnit 24/24, no skip. |
| Release test simulated a new process before the old in-process Host had performed production `main()` retirement shutdown, causing an immediate `1.3.0` test-only rollback. | Assert retirement flush/idle before cross-process CLI restart, commit `e222e77`. | Focused JUnit 24/24; later full Debug and Release suites passed 44/44. |
| The production Host did not yet compose the declared network/storage/file/clipboard services. | Added `HostCapabilityRuntime`, authenticated permission propagation, bounded dispatch, generation cancellation, and real deployed business flows, commit `a1ee454`. | `host_capability_runtime`, affected C++ suites, and deployment markers for network/storage/file/clipboard. |
| Clipboard grants were not cryptographically relevant unless tied to the actual admitted Worker input source. | Bound one-shot grants to Worker HWND/PID/generation and the Host foreground root, commits `de1abb8` and `f911bc1`. | Expired and undeclared denied; gesture allowed once; replay denied. |
| Heartbeat elapsed time included pre-admission cold startup, and Host/Worker teardown could block the GUI path. | Start timing after admission and make retirement/window teardown nonblocking, commits `dce8344`, `7b3913a`, and `9aba32e`. | Worker lifecycle/E2E suites plus repeated deployed clean exits around 0.6 s. |
| Deployment automation sampled an offscreen/declared geometry and initially missed composited Qt pixels and customer rows. | Capture once from the desktop DC, scale from the rendered 1100×679 client, and use corrected rendered regions, commits `7d4d35f` through `b10ec95`. | Real login/order/customer/storage/file business markers in the guarded deployment. |
| Cross-process native file automation used an ANSI-marshaled `SendMessageW` path and attempted direct-process text reads; StrictMode also rejected a newly added clipboard permission. | Use Unicode `WM_SETTEXT`, system-marshaled `WM_GETTEXT`, `Add-Member`, foreground revalidation, and one bounded missed-input retry, commits `c3fae8a`, `fed4eba`, `e4eb12a`, and `cdda98b`. | Static regression suite 11/11 and final deployed cancel+success+consumed file marker. |

## Known diagnostics

CMake reported missing optional Vulkan headers and the optional private
`Qt6TaskTree` / `Qt6QmlAssetDownloaderPrivate` plugin dependency. `windeployqt`
also emitted an optional position-plugin dependency diagnostic in the Debug
deployment scan. None is a required Full Pilot component: required Qt modules
configured, the WebEngine renderer executed from the deployment, and the
authoritative verifier proved the final PE/runtime closure. These are recorded
rather than relabeled as silent success.

Two pre-final environmental failures were also retained in the audit trail. One
isolated Node process exited with Windows fast-fail `0xC0000409`; an immediate
standalone full run passed, and the final clean Release run passed 172/172. A
later deployment run overlapped another workspace's five-run startup benchmark,
producing a 65 s scheduling pause and the expected Worker heartbeat restart.
That external benchmark was allowed to finish without interference; the final
uncontended run then passed the 10/10 stable-generation check. Neither failure
was converted into a retry inside the security assertions.

## Supported scope and non-goals

The accepted scope is the Windows-first signed-package Pilot: trusted Host,
LPAC Worker, typed capability IPC, nine QML routes, one isolated WebEngine route,
deterministic mock/migration tooling, signed update, crash rollback, and guarded
deployment.

The approved non-goals remain unsupported: arbitrary Internet QML browsing;
production package repository/key ceremony; Linux/macOS sandboxes; migration of
a real external business app; browser-complete HTML/CSS/JavaScript; remote native
plugins; marketplace/review/billing; and production SSO, analytics, or cloud
rollout services.

All selected MVP deliverables, production capability flows, and security gates
have direct passing evidence at the audited source commit. No open acceptance
blocker remains.
