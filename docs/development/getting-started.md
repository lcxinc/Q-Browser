# Getting started on Windows

Install Visual Studio 2022 Build Tools, CMake 3.30+, Qt 6.11.1 MSVC 2022 x64
including WebEngine, OpenSSL 3 x64, and Node.js 24+. The audited local paths are
shown in the repository README and can be overridden on each script.

From a PowerShell prompt at the repository root:

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --preset dev-debug

$env:npm_config_cache = (Resolve-Path build).Path + '\npm-cache'
npm.cmd ci --prefix tools --ignore-scripts --cache $env:npm_config_cache
npm.cmd test --prefix tools --ignore-scripts
```

Run all required acceptance groups, including real LPAC and WebEngine tests:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 `
  -Configuration Release
```

Create and verify a deployment (the default also reruns Release acceptance):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Clean
```

The output is `build\release-deploy`. A second call without `-Clean` never
overwrites it; it only verifies its canonical `SHA-256SUMS`. `-Clean` removes
only validated paths below this repository's `build` directory. Temporary
files are also kept on that local build volume.

`scripts\create-dev-package.ps1` creates or reuses a key below
`.qbrowser-dev\signing`, packs and signs twice to prove determinism, inspects
the result, and publishes only the signed package and public key below `build`.
It prints a development-only warning. Never promote that key to production,
and never copy `.qbrowser-dev\signing\private.pem` into a deployment.

For production package mode, the Host needs absolute, existing, non-overlapping
package-store, sandbox-temp, runtime, telemetry, Worker, public-key, and optional
install-package paths. The trust key must have a restricted owner/SYSTEM ACL.
Package-store, sandbox-temp, telemetry, and immutable Worker-runtime roots must
likewise be protected from untrusted write access and must not overlap. The
deployment keeps the Host/CLI closure in `host` and the complete LPAC Worker
closure in the separate `runtime` directory; pass only the latter as the
immutable runtime root. Do not broaden these ACLs to make a launch succeed.
Use `qbrowser-host.exe --trusted-shell` only for trusted-shell diagnostics; it
does not exercise package execution.
