# Q-Browser

Q-Browser is a Windows-first hybrid application runtime. A trusted Qt Host
routes signed package QML to an independent LPAC Worker, brokers capabilities,
provides an isolated WebEngine fallback, and recovers failed updates to a
last-known-good version. The repository includes the ten-route Pilot, package
CLI, HTML/CSS migrator, security tests, and repeatable Release deployment.

## Audited Windows toolchain

The development preset and commands use the audited tools directly; no global `PATH` configuration is required.

| Tool | Audited path |
| --- | --- |
| CMake | `E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe` |
| CTest | `E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe` |
| Qt 6.11.1 | `E:\DevEnv\qt\6.11.1\msvc2022_64` |
| Qt CMake package | `E:\DevEnv\qt\6.11.1\msvc2022_64\lib\cmake\Qt6` |
| OpenSSL 3 | `E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64` |
| Generator | Visual Studio 17 2022, x64 |
| Node.js | Node 24 or newer |

Qt WebEngine is enabled by default. Set `Q_BROWSER_BUILD_WEBENGINE=OFF` only for build-system diagnostics; the approved MVP and its acceptance build require WebEngine.

## Configure, build, and test

Run from the repository root in PowerShell:

```powershell
$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe'
$CTest = 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe'
$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64'
$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64'

& $CMake -S . -B build\dev -G 'Visual Studio 17 2022' -A x64 `
  -DQt6_DIR="$QtRoot\lib\cmake\Qt6" `
  -DOPENSSL_ROOT_DIR="$OpenSslRoot" `
  -DQ_BROWSER_BUILD_WEBENGINE=ON
& $CMake --build build\dev --config Debug --parallel
& $CTest --test-dir build\dev -C Debug --output-on-failure
```

The equivalent checked-in presets are:

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --preset dev-debug
```

Install and verify the locked TypeScript workspaces with lifecycle scripts disabled:

```powershell
npm.cmd ci --ignore-scripts --prefix tools
npm.cmd run build --ignore-scripts --prefix tools
npm.cmd test --ignore-scripts --prefix tools
```

The checked-in `mock-api` and `migrator` workspaces are both built and tested
by that command. See the [migrator guide](docs/development/migrator.md).

## Full acceptance and Release deployment

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 `
  -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Clean
```

The deployment is published atomically to
`%LOCALAPPDATA%\QBrowserTask18\release-deploy` only after
deployment-only UI, update, double-crash rollback, and optional full acceptance
pass. CMake install and `windeployqt` build the image, which is checked against
a canonical sorted `SHA-256SUMS`. It
contains Host, Worker, package CLI, Qt/OpenSSL/WebEngine runtime closure, a
signed Pilot package, the development public key, and operating documentation.
The Host/CLI closure is under `host`; the separate immutable LPAC Worker
closure is under `runtime`.
Private keys, tests, fixtures, source, symbols, and test-hook binaries are
rejected. A rerun without `-Clean` verifies rather than overwrites the output.

The bundled trust root is **development-only**. Local signing authority remains
under the protected, non-repository
`%LOCALAPPDATA%\QBrowserTask18\signing` directory and must never
be copied into build/deployment output or promoted to production.

Start with [runtime architecture](docs/architecture/runtime.md), the
[package format](docs/package-spec/qapkg-v1.md), [Windows sandbox](docs/security/windows-sandbox.md),
the [threat model](docs/security/threat-model.md), and
[developer setup](docs/development/getting-started.md). Operations guidance
covers [updates and rollback](docs/operations/update-rollback.md) and
[safe diagnostics](docs/operations/diagnostics.md).

## Generated directories

- `build/` contains CMake output and generated fixtures, including `build/generated/`.
- `.qbrowser-dev/` contains local runtime state, installed development packages, and logs.
- `tools/node_modules/`, `dist/`, `coverage/`, TypeScript build metadata, Visual Studio user files, and logs are generated and ignored.

Do not commit generated output or runtime state.

## Package signing CLI

`qbrowser-package` provides pack, sign, inspect, and key-generation primitives.
Use the guarded development workflow rather than handling key paths manually:

```powershell
$task18Root = Join-Path ([Environment]::GetFolderPath(
  [Environment+SpecialFolder]::LocalApplicationData)) 'QBrowserTask18'
powershell -ExecutionPolicy Bypass -File scripts\create-dev-package.ps1 `
  -Configuration Release -Clean
& "$task18Root\build\release\tools\package-cli\Release\qbrowser-package.exe" inspect `
  --package "$task18Root\release-package\com.qbrowser.pilot-1.0.0.qapkg" `
  --public-key "$task18Root\release-package\dev-public.pem"
```

`pack` writes a deterministic archive and canonical lowercase payload digest.
`sign` validates that digest, signs a separate domain-separated digest that
includes `metadata/content.sha256`, and writes a new archive. `inspect` emits
one compact JSON object on standard output and returns zero only when the
archive, manifest, digest, public key, and Ed25519 signature all verify.
