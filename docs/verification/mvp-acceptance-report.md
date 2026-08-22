# Q-Browser MVP acceptance report

Status: **PASS**

This report audits the approved Full Pilot design and Tasks 0–18 against the
source commit `e222e77ee6d1642d6dcb6f2f49afe36485f01431`. The verification window was
2026-08-22 11:41:26 through 15:31:30 China Standard Time (UTC+08:00,
Asia/Shanghai). The final documentation commit is intentionally not part of the
audited executable input.

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
| Audited source | `e222e77ee6d1642d6dcb6f2f49afe36485f01431` |
| Final deployment | `C:\Users\10428\AppData\Local\QBrowserTask18\release-deploy` |
| Deployment manifest SHA-256 | `D70C9DEE4517949C63FC7D9807A36EC097022EADB4FD9C6DFB6C9149C848E15B` |
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
# Fresh Debug acceptance at e222e77 (the build path did not previously exist)
$b = 'L:\project\Q-Browser\.worktrees\q-browser-mvp-impl\build\task19-debug-final-e222e77'
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
| Fresh Debug CTest | 42/42 passed, 0 failed, 481.26 s |
| Fresh Release CTest | Two independent clean runs passed 42/42 with 0 failures; final run 211.66 s (prior run 245.22 s) |
| Required Debug JUnit | 12 XML files, 162 cases, 0 failures, 0 errors, 0 skipped, 253.592 s |
| Required Release JUnit | Final clean run: 12 XML files, 162 cases, 0 failures, 0 errors, 0 skipped, 142.961 s (prior clean run 157.724 s) |
| Node/Vitest in Debug | 9 files, 162/162 tests passed, 48.31 s |
| Node/Vitest in Release | Final clean run: 9 files, 162/162 tests passed, 44.86 s (prior clean run 46.11 s) |
| Fresh Debug end-to-end acceptance | PASS at 2026-08-22 14:49:39+08:00 |
| Fresh Task18 Release and atomic publish | Two independent clean runs PASS; final atomic publish at 2026-08-22 15:30:37+08:00 |
| Clean-build reproducibility | Both independent clean runs produced manifest SHA-256 `D70C9DEE4517949C63FC7D9807A36EC097022EADB4FD9C6DFB6C9149C848E15B` |
| Deployment-only E2E | `routes=10 webEngine=deployed update=1.1.0 rollback=1.0.0 paths=minimal+polluted` |
| No-clean verifier | PASS at 2026-08-22 15:31:04+08:00; manifest before/after identical; no mutation |
| Independent minimal and polluted PATH verifier | PASS / PASS on the final published tree at 2026-08-22 15:31:30+08:00 |
| Release product build | `BUILD_TESTING:BOOL=OFF`; `ctest -N` reports `Total Tests: 0`; deployed product test-hook marker scan found 0 |

## Task 0–18 deliverable matrix

Every row cites an executable test or verifier, not the existence of a file as
the sole proof.

| Task | Required deliverable / gate | Direct proof and final result |
|---:|---|---|
| 0 | Repair and verify Qt toolchain | The plan's direct probe returned `True` for all seven required modules: `Qt6Core`, `Qt6Quick`, `Qt6QuickTest`, `Qt6Network`, `Qt6WebEngineCore`, `Qt6WebEngineWidgets`, and `Qt6WebEngineQuick`. Both clean configure runs also detected MSVC 19.44, Qt 6.11.1 and OpenSSL 3.0.16; WebEngine was exercised by `e2e_web_fallback`, `request_interceptor`, and the deployed `QtWebEngineProcess.exe`. PASS. |
| 1 | Reproducible CMake/test skeleton | Fresh Debug and Release builds plus `build_smoke`; 42/42 in both configurations. PASS. |
| 2 | Normalized `app://` URLs and route matching | `app_url`, `normalized_path`, `route_registry`, and `pilot_routes::hostInventoryResolvesPatternToPageAndEngine`. PASS. |
| 3 | Strict package manifest | `manifest` and `pilot_routes::packageManifestDeclaresWorkerInventory` / `manifestDeclarationsMatchDirectSourceUsage`. PASS. |
| 4 | Deterministic archive and safe extraction | `archive` plus malicious archive/path/reparse cases in `malicious_package`. PASS. |
| 5 | Ed25519 verification and package CLI | `signature`, `package_cli`, Debug/Release package pack-inspect-sign acceptance, and tampered-package E2E. PASS. |
| 6 | Versioned store, atomic install, rollback | `package_store`, `package_installer`, `update_lifecycle`, `offline_lkg`, `crash_rollback`. PASS. |
| 7 | Bounded Host/Worker IPC | `frame_codec`, `protocol_message`, `ipc_session`, `worker_handshake`, and `worker_api_surface::rejectsOversizedMalformedAndReplayedFrames`. PASS. |
| 8 | Policy intersection and brokers | `policy_engine`, `capability_broker`, `network_broker`, `storage_broker`, and `capability_escape::undeclaredCapabilitiesNeverReachServices`. PASS. |
| 9 | LPAC/AppContainer Worker and Job | `sandbox_launcher` JUnit: 17/17 including `launchProbeProvesPositiveAndNegativeBoundaries`, `jobObjectHasKillProcessAndMemoryLimits`, and `closingJobKillsAssignedProcess`. PASS. |
| 10 | Independent QML Worker/surface | `worker_handshake`, `worker_surface`, `worker_crash`, `worker_confinement`, `worker_api_surface`; real deployed Worker route acknowledgements were also checked. PASS. |
| 11 | Host shell and WebEngine fallback | `unified_navigation`, `request_interceptor`, `webengine_file_selection`, `e2e_web_fallback`; deployment-only E2E observed the helper command line under the deployment `host` directory. PASS. |
| 12 | Deterministic mock REST and Web help fixture | Node/Vitest mock-api suites are part of each 162/162 run; `/web/help` passed both C++ E2E and deployment-only E2E. PASS. |
| 13 | `Company.Design` system | `quick_design` and the fresh QML deployment scan; design controls compiled and loaded in Debug and Release. PASS. |
| 14 | Ten-route Pilot package | `pilot_routes` inventory/source-policy/real-Worker cases, `pilot_qmllint`, `e2e_host_routes`, and deployment-only 10-route E2E. PASS. |
| 15 | HTML/CSS migration CLI | Migrator Vitest/golden suites within each 162/162 Node run prove deterministic output and source-located diagnostics. PASS. |
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

## Design acceptance gates

| Gate | Direct evidence | Result |
|---:|---|---|
| 1 | Fresh Debug acceptance and guarded fresh Task18 Release build/deploy from `e222e77`; no-clean verification repeated without mutation. | PASS |
| 2 | `e2e_host_routes`, `pilot_routes`, and Task18 deployment marker `routes=10`. | PASS |
| 3 | Nine concrete Worker route acknowledgements plus `/web/help` using the deployed WebEngine renderer. | PASS |
| 4 | `e2e_host_survives_worker_crash::crashingCandidateRollsBackWithoutTerminatingHost`: the Host remained navigable after both forced Worker terminations. | PASS |
| 5 | `e2e_package_update::signedUpdateActivatesAndTamperNeverExecutes`: `1.1.0` activated; tampered `1.2.0` produced exact `signature_invalid`, canary count 0, no ready event, and unchanged current/LKG. | PASS |
| 6 | The same Host-survival E2E forced two crashes of `1.1.0` and observed healthy `1.0.0`; Task18 independently reported `rollback=1.0.0`. | PASS |
| 7 | Sandbox launch probe and capability-escape tests directly denied arbitrary file read, loopback connection, `cmd`/self child creation, package/temp execute, QML raw network, file URL load, and process API access. | PASS |
| 8 | `policy_engine`, `capability_broker`, broker tests, and undeclared-capability zero-invocation assertion. | PASS |
| 9 | Migrator Vitest/golden corpus in both 162/162 Node runs. | PASS |
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
| Release test simulated a new process before the old in-process Host had performed production `main()` retirement shutdown, causing an immediate `1.3.0` test-only rollback. | Assert retirement flush/idle before cross-process CLI restart, commit `e222e77`. | Focused JUnit 24/24; then fresh Debug 42/42 and fresh Release 42/42. |

## Known diagnostics

CMake reported missing optional Vulkan headers and the optional private
`Qt6TaskTree` / `Qt6QmlAssetDownloaderPrivate` plugin dependency. `windeployqt`
also emitted an optional position-plugin dependency diagnostic in the Debug
deployment scan. None is a required Full Pilot component: required Qt modules
configured, the WebEngine renderer executed from the deployment, and the
authoritative verifier proved the final PE/runtime closure. These are recorded
rather than relabeled as silent success.

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

All selected MVP deliverables and security gates have direct passing evidence at
the audited source commit. No open acceptance blocker remains.
