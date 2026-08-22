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
publishes `%LOCALAPPDATA%\QBrowserTask18\release-deploy` atomically only after
those gates pass. The Release source is first copied into a protected tracked-file
snapshot under that trusted root. A
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

**Development only:** the signing authority is stored outside the repository at
the exact protected directory `%LOCALAPPDATA%\QBrowserTask18\signing`. Never
commit it, copy private
signing material into `build` or a deployment, attach it to diagnostics, or
promote this trust root to production. The script publishes only the signed
package and public key.

## Manual deployed Host smoke

The following commands create four distinct protected state roots and start
the mock API. Do not substitute a source-build Host, Worker, Qt directory, or
unprotected state path.

```powershell
$task18Root = Join-Path ([Environment]::GetFolderPath(
  [Environment+SpecialFolder]::LocalApplicationData)) 'QBrowserTask18'
$deploy = (Resolve-Path (Join-Path $task18Root 'release-deploy')).Path
$state = Join-Path $task18Root 'manual-deployed-smoke'
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 `
  -PrepareManualState $state
$store = Join-Path $state 'package-store'
$sandbox = Join-Path $state 'sandbox-temp'
$telemetry = Join-Path $state 'telemetry'
$storage = Join-Path $state 'storage'

$mockOut = Join-Path $state 'mock.stdout'
$mockErr = Join-Path $state 'mock.stderr'
$mock = Start-Process node.exe -ArgumentList 'src/server.ts' `
  -WorkingDirectory tools\mock-api -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr
$origin = $null
$mockWait = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 100
  $mock.Refresh()
  if ($mock.HasExited) { throw "Mock API exited early: $($mock.ExitCode)" }
  if (Test-Path $mockOut) {
    try { $origin = (Get-Content $mockOut -First 1 | ConvertFrom-Json).origin }
    catch { $origin = $null }
  }
  if ($mockWait.ElapsedMilliseconds -ge 15000) {
    throw 'Mock API did not publish a valid nonempty loopback origin within 15 seconds.'
  }
} until ($origin -match '^http://127\.0\.0\.1:[1-9][0-9]*$')
$env:PATH = "$deploy\host;$env:SystemRoot\System32;$env:SystemRoot"

$common = @('--package-mode',"--mock-origin=$origin",'--app-id=com.qbrowser.pilot',
  "--trusted-public-key=$deploy\trust\dev-public.pem","--package-store=$store",
  "--sandbox-temp=$sandbox","--runtime-root=$deploy\runtime",
  "--worker-executable=$deploy\runtime\qbrowser-worker.exe",
  "--telemetry-directory=$telemetry","--storage-directory=$storage",
  '--health-window-ms=2000',
  '--heartbeat-timeout-ms=10000')
function ConvertTo-LaunchArguments([string[]]$Values) {
  return (($Values | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' ')
}
function Stop-DeployedHost([Diagnostics.Process]$Process) {
  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes
  $condition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $Process.Id)
  $window = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
    [System.Windows.Automation.TreeScope]::Children, $condition)
  if ($null -eq $window) { throw 'The exact deployed Host window is unavailable.' }
  $pattern = [System.Windows.Automation.WindowPattern]$window.GetCurrentPattern(
    [System.Windows.Automation.WindowPattern]::Pattern)
  $pattern.Close()
  [void]$Process.WaitForExit(60000)
  if (-not $Process.HasExited -or $Process.ExitCode -ne 0) {
    throw 'The deployed Host did not complete checked cleanup.'
  }
}
$pilot = "$deploy\packages\com.qbrowser.pilot-1.0.0.qapkg"
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments ($common + "--install-package=$pilot")) `
  -PassThru
```

Close that Host before an offline restart. Offline startup omits
`--install-package` and re-verifies the selected installed binding:

```powershell
Stop-DeployedHost $appProcess
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments $common) -PassThru
```

For a signed update supplied by the same development authority, inspect it,
then restart with the candidate. Replace only the candidate value, not the
trust key or installed version directories:

```powershell
$candidate = (Resolve-Path (Join-Path $task18Root `
  'candidate\com.qbrowser.pilot-1.1.0.qapkg')).Path
& "$deploy\host\qbrowser-package.exe" inspect --package $candidate `
  --public-key "$deploy\trust\dev-public.pem"
if ($LASTEXITCODE -ne 0) { throw 'Candidate signature verification failed.' }
Stop-DeployedHost $appProcess
$updateCommon = @($common | Where-Object { $_ -notlike '--health-window-ms=*' }) +
  '--health-window-ms=60000'
$appProcess = Start-Process "$deploy\host\qbrowser-host.exe" `
  -ArgumentList (ConvertTo-LaunchArguments ($updateCommon + "--install-package=$candidate")) `
  -PassThru
```

Development-only crash recovery can be observed without editing activation
state. Terminate only Workers whose executable path exactly matches the
deployment; the first crash restarts the candidate and the second recovers the
reverified previous/LKG binding:

```powershell
function Wait-DistinctDeployedWorker([string[]]$Excluded = @()) {
  $deadline = [Diagnostics.Stopwatch]::StartNew()
  do {
    Start-Sleep -Milliseconds 100
    $workers = @(Get-CimInstance Win32_Process -Filter "Name='qbrowser-worker.exe'" |
      Where-Object { $_.ExecutablePath -eq "$deploy\runtime\qbrowser-worker.exe" })
    if ($workers.Count -eq 1) {
      $identity = "$($workers[0].ProcessId)|$($workers[0].CreationDate.ToUniversalTime().Ticks)"
      if ($identity -notin $Excluded) {
        return [pscustomobject]@{ Process = $workers[0]; Identity = $identity }
      }
    }
    if ($deadline.ElapsedMilliseconds -ge 30000) {
      throw 'A unique distinct deployed Worker generation did not appear.'
    }
  } while ($true)
}

function Get-EventMatchCount([string]$Pattern) {
  if (-not (Test-Path "$telemetry\events.jsonl")) { return 0 }
  return @(Get-Content "$telemetry\events.jsonl" |
    Select-String -Pattern $Pattern).Count
}

$restartPattern = '"packageVersion":"1\.1\.0","phase":"worker","code":"restarted"'
$candidateHealthPattern = '"packageVersion":"1\.1\.0","phase":"health","code":"healthy"'
$restartBefore = Get-EventMatchCount $restartPattern
$candidateHealthBefore = Get-EventMatchCount $candidateHealthPattern
$first = Wait-DistinctDeployedWorker
Stop-Process -Id $first.Process.ProcessId -Force
do { Start-Sleep -Milliseconds 100 } while (Get-Process -Id `
  $first.Process.ProcessId -ErrorAction SilentlyContinue)
$second = Wait-DistinctDeployedWorker @($first.Identity)
$restartDeadline = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 100
  $restarted = (Get-EventMatchCount $restartPattern) -gt $restartBefore
  $healthEvent = (Get-EventMatchCount $candidateHealthPattern) -gt
    $candidateHealthBefore
  $current = @(Get-CimInstance Win32_Process -Filter "Name='qbrowser-worker.exe'" |
    Where-Object { $_.ExecutablePath -eq "$deploy\runtime\qbrowser-worker.exe" })
  $healthy = $healthEvent -and $current.Count -eq 1 -and
    "$($current[0].ProcessId)|$($current[0].CreationDate.ToUniversalTime().Ticks)" -eq
      $second.Identity
  if ($restartDeadline.ElapsedMilliseconds -ge 30000) {
    throw 'The distinct replacement Worker did not report healthy restart telemetry.'
  }
} until ($restarted -and $healthy)
$rollbackPattern = '"packageVersion":"1\.0\.0","phase":"rollback","code":"recovered"'
$lkgHealthPattern = '"packageVersion":"1\.0\.0","phase":"health","code":"healthy"'
$rollbackBefore = Get-EventMatchCount $rollbackPattern
$lkgHealthBefore = Get-EventMatchCount $lkgHealthPattern
Stop-Process -Id $second.Process.ProcessId -Force
do { Start-Sleep -Milliseconds 100 } while (Get-Process -Id `
  $second.Process.ProcessId -ErrorAction SilentlyContinue)
$third = Wait-DistinctDeployedWorker @($first.Identity,$second.Identity)
$rollbackDeadline = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 100
  $current = @(Get-CimInstance Win32_Process -Filter "Name='qbrowser-worker.exe'" |
    Where-Object { $_.ExecutablePath -eq "$deploy\runtime\qbrowser-worker.exe" })
  $thirdAlive = $current.Count -eq 1 -and
    "$($current[0].ProcessId)|$($current[0].CreationDate.ToUniversalTime().Ticks)" -eq
      $third.Identity
  $recovered = (Get-EventMatchCount $rollbackPattern) -gt $rollbackBefore
  $lkgHealthy = (Get-EventMatchCount $lkgHealthPattern) -gt $lkgHealthBefore
  if ($rollbackDeadline.ElapsedMilliseconds -ge 30000) {
    throw 'Rollback did not recover a distinct healthy 1.0.0 Worker generation.'
  }
} until ($thirdAlive -and $recovered -and $lkgHealthy)
```

Close only the processes retained by this session and require complete cleanup:

```powershell
Stop-DeployedHost $appProcess
if (-not $mock.HasExited) { Stop-Process -Id $mock.Id }
if (-not $mock.WaitForExit(10000)) { throw 'The owned mock API did not exit.' }
```

Do not delete or edit candidate, activation, version, or telemetry files while
either process is running.
