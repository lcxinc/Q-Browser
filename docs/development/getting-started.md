# Getting started on Windows

Install Visual Studio 2022 Build Tools, CMake 3.30+, Qt 6.11.1 MSVC 2022 x64
including WebEngine, OpenSSL 3 x64, and Node.js 24+. Run commands from the
repository root in PowerShell.

## Build and test

```powershell
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --preset dev-vs2022
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' --build --preset dev-debug
& 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe' --preset dev-debug

$env:npm_config_cache = (Resolve-Path build).Path + '\npm-cache'
npm.cmd ci --prefix tools --ignore-scripts --cache $env:npm_config_cache
npm.cmd run build --prefix tools --ignore-scripts
npm.cmd test --prefix tools --ignore-scripts
```

Run all required Task17 acceptance groups, then create an accepted Release:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-acceptance.ps1 `
  -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Clean
```

The Release command always exercises the staged deployment under a minimal and
polluted `PATH`: a real deployed Host, LPAC Worker, all ten Pilot routes,
deployed WebEngine helper, a signed update, and double-crash LKG recovery. It
publishes `build\release-deploy` atomically only after those gates pass. A
second invocation without `-Clean` is read-only and requires the acceptance
attestation, protected ACLs, signature, identity, and canonical hashes to be
unchanged.

## Development signing authority

Generate/reuse the ignored development authority and create the deterministic
Pilot package only through the guarded script:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\create-dev-package.ps1 `
  -Configuration Release -Clean
```

**Development only:** the signing authority is stored at the exact ignored,
protected directory `.qbrowser-dev\signing`. Never commit it, copy private
signing material into `build` or a deployment, attach it to diagnostics, or
promote this trust root to production. The script publishes only the signed
package and public key.

## Manual deployed Host smoke

The following commands create three distinct protected state roots and start
the mock API. Do not substitute a source-build Host, Worker, Qt directory, or
unprotected state path.

```powershell
$deploy = (Resolve-Path build\release-deploy).Path
$state = Join-Path (Resolve-Path build).Path 'manual-deployed-smoke'
$store = Join-Path $state 'package-store'
$sandbox = Join-Path $state 'sandbox-temp'
$telemetry = Join-Path $state 'telemetry'
New-Item -ItemType Directory -Force $store,$sandbox,$telemetry | Out-Null
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
foreach ($directory in @($store,$sandbox,$telemetry)) {
  & "$env:SystemRoot\System32\icacls.exe" $directory /inheritance:r `
    /grant:r "${me}:(OI)(CI)F" '*S-1-5-18:(OI)(CI)F'
  if ($LASTEXITCODE -ne 0) { throw "ACL protection failed: $directory" }
}

$mockOut = Join-Path $state 'mock.stdout'
$mockErr = Join-Path $state 'mock.stderr'
$mock = Start-Process node.exe -ArgumentList 'src/server.ts' `
  -WorkingDirectory tools\mock-api -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr
do { Start-Sleep -Milliseconds 100 } until (Test-Path $mockOut)
$origin = (Get-Content $mockOut -First 1 | ConvertFrom-Json).origin
$env:PATH = "$deploy\host;$env:SystemRoot\System32;$env:SystemRoot"

$common = @('--package-mode',"--mock-origin=$origin",'--app-id=com.qbrowser.pilot',
  "--trusted-public-key=$deploy\trust\dev-public.pem","--package-store=$store",
  "--sandbox-temp=$sandbox","--runtime-root=$deploy\runtime",
  "--worker-executable=$deploy\runtime\qbrowser-worker.exe",
  "--telemetry-directory=$telemetry",'--health-window-ms=2000',
  '--heartbeat-timeout-ms=10000')
function ConvertTo-LaunchArguments([string[]]$Values) {
  return (($Values | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' ')
}
$pilot = "$deploy\packages\com.qbrowser.pilot-1.0.0.qapkg"
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments ($common + "--install-package=$pilot")) `
  -PassThru
```

Close that Host before an offline restart. Offline startup omits
`--install-package` and re-verifies the selected installed binding:

```powershell
$appProcess.CloseMainWindow(); $appProcess.WaitForExit(15000)
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments $common) -PassThru
```

For a signed update supplied by the same development authority, inspect it,
then restart with the candidate. Replace only the candidate value, not the
trust key or installed version directories:

```powershell
$candidate = (Resolve-Path build\candidate\com.qbrowser.pilot-1.1.0.qapkg).Path
& "$deploy\host\qbrowser-package.exe" inspect --package $candidate `
  --public-key "$deploy\trust\dev-public.pem"
if ($LASTEXITCODE -ne 0) { throw 'Candidate signature verification failed.' }
$appProcess.CloseMainWindow(); $appProcess.WaitForExit(15000)
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments ($common + "--install-package=$candidate")) `
  -PassThru
```

Development-only crash recovery can be observed without editing activation
state. Terminate only Workers whose executable path exactly matches the
deployment; the first crash restarts the candidate and the second recovers the
reverified previous/LKG binding:

```powershell
1..2 | ForEach-Object {
  do {
    Start-Sleep -Milliseconds 100
    $worker = Get-CimInstance Win32_Process -Filter "Name='qbrowser-worker.exe'" |
      Where-Object { $_.ExecutablePath -eq "$deploy\runtime\qbrowser-worker.exe" } |
      Select-Object -First 1
  } until ($null -ne $worker)
  Stop-Process -Id $worker.ProcessId -Force
}
Get-Content "$telemetry\events.jsonl" | Select-String '"phase":"rollback","code":"recovered"'
```

Close the Host and mock process when finished. Do not delete or edit candidate,
activation, version, or telemetry files while either process is running.
