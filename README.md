# Q-Browser

Q-Browser is a Windows-first hybrid application runtime. The current repository scope is the reproducible C++20/Qt and TypeScript build-and-test skeleton for the approved secure QML pilot. Host, worker, package, broker, WebEngine, design-system, pilot-page, and migration-tool behavior will be added incrementally under test.

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

Initialize and verify the TypeScript solution workspace with:

```powershell
npm install --prefix tools --no-audit --no-fund
npm run build --prefix tools
npm run lint --prefix tools
```

`tools/package.json` reserves the `mock-api` and `migrator` npm workspaces.
Each workspace is registered automatically when its directory and
`package.json` are created. Add its TypeScript project to the root
`tools/tsconfig.json` `references` array in that same change; references must
never point at a workspace that does not exist yet.

`npm test --prefix tools` becomes applicable when the first TypeScript source project adds its Vitest suite.

## Generated directories

- `build/` contains CMake output and generated fixtures, including `build/generated/`.
- `.qbrowser-dev/` contains local runtime state, installed development packages, and logs.
- `tools/node_modules/`, `dist/`, `coverage/`, TypeScript build metadata, Visual Studio user files, and logs are generated and ignored.

Do not commit generated output or runtime state.

## Package signing CLI

`qbrowser-package` provides a stable four-command development workflow:

```powershell
qbrowser-package keygen --private-key keys/dev/private.pem --public-key keys/dev/public.pem
qbrowser-package pack --source path/to/package-tree --output unsigned.qapkg
qbrowser-package sign --package unsigned.qapkg --private-key keys/dev/private.pem --output signed.qapkg
qbrowser-package inspect --package signed.qapkg --public-key keys/dev/public.pem
```

`pack` writes a deterministic archive and canonical lowercase payload digest.
`sign` validates that digest, signs a separate domain-separated digest that
includes `metadata/content.sha256`, and writes a new archive. `inspect` emits
one compact JSON object on standard output and returns zero only when the
archive, manifest, digest, public key, and Ed25519 signature all verify.
