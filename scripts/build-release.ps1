[CmdletBinding()]
param(
    [string]$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64',
    [string]$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64',
    [string]$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe',
    [string]$BuildDirectory = '',
    [string]$DeploymentDirectory = '',
    [switch]$Clean,
    [bool]$RunAcceptance = $true,
    [ValidateSet('', 'BeforePublish')]
    [string]$FailureInjection = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoBuild = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
$defaultBuild = [IO.Path]::GetFullPath((Join-Path $repoBuild 'release'))
$defaultDeployment = [IO.Path]::GetFullPath((Join-Path $repoBuild 'release-deploy'))
$packageOutput = [IO.Path]::GetFullPath((Join-Path $repoBuild 'release-package'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = $defaultBuild }
if ([string]::IsNullOrWhiteSpace($DeploymentDirectory)) {
    $DeploymentDirectory = $defaultDeployment
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
$deployment = [IO.Path]::GetFullPath($DeploymentDirectory)
$ownedMarkerName = '.qbrowser-task18-owned'
$ownedMarkerText = "Q-BROWSER TASK18 OWNED v1`n"
$releaseMarkerText = "Q-BROWSER TASK18 RELEASE v1`n"

function Assert-ChildPath([string]$Path, [string]$Parent, [string]$Label) {
    $parentPrefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath($Path)
    if (-not $candidate.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must remain below $Parent"
    }
}

function Assert-NoReparseAncestor([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not (Test-Path -LiteralPath $candidate)) {
        $parent = [IO.Directory]::GetParent($candidate)
        if ($null -eq $parent) { throw "No existing ancestor for $Path" }
        $candidate = $parent.FullName
    }
    $item = Get-Item -LiteralPath $candidate -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse ancestor is forbidden: $($item.FullName)"
        }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}

function Get-PathIdentity([string]$Path) {
    $text = & "$env:SystemRoot\System32\fsutil.exe" file queryfileid $Path 2>&1
    if ($LASTEXITCODE -ne 0 -or ($text -join "`n") -notmatch '0x[0-9a-fA-F]+') {
        throw "Cannot pin filesystem identity: $Path"
    }
    return $Matches[0].ToLowerInvariant()
}

function New-OwnedDirectory([string]$Path) {
    Assert-NoReparseAncestor $Path
    if (Test-Path -LiteralPath $Path) { throw "Owned path already exists: $Path" }
    New-Item -ItemType Directory -Path $Path | Out-Null
    [IO.File]::WriteAllText((Join-Path $Path $ownedMarkerName), $ownedMarkerText,
        [Text.UTF8Encoding]::new($false))
}

function Remove-OwnedTree([string]$Path, [string]$ExactAllowedPath,
        [string]$MarkerName = $ownedMarkerName,
        [string]$MarkerText = $ownedMarkerText) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.Equals([IO.Path]::GetFullPath($ExactAllowedPath),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a non-owned Task18 path: $full"
    }
    if (-not (Test-Path -LiteralPath $full)) { return }
    Assert-NoReparseAncestor $full
    $root = Get-Item -LiteralPath $full -Force
    if (-not $root.PSIsContainer -or
        ($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Owned cleanup root is not a plain directory: $full"
    }
    $identity = Get-PathIdentity $full
    $marker = Join-Path $full $MarkerName
    $actualMarker = if (Test-Path -LiteralPath $marker -PathType Leaf) {
        (Get-Content -LiteralPath $marker -Raw) -replace "`r`n", "`n"
    } else { '' }
    if ($actualMarker -ne ($MarkerText -replace "`r`n", "`n")) {
        throw "Owned cleanup marker is absent or invalid: $full"
    }
    $entries = @(Get-ChildItem -LiteralPath $full -Force -Recurse)
    $entryIdentities = @{}
    foreach ($entry in $entries) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Owned cleanup tree contains a reparse point: $($entry.FullName)"
        }
        $entryIdentities[$entry.FullName] = Get-PathIdentity $entry.FullName
    }
    foreach ($file in @($entries | Where-Object { -not $_.PSIsContainer } |
            Sort-Object { $_.FullName.Length } -Descending)) {
        if ((Get-PathIdentity $full) -ne $identity) {
            throw "Owned cleanup root identity changed: $full"
        }
        $current = Get-Item -LiteralPath $file.FullName -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-PathIdentity $file.FullName) -ne $entryIdentities[$file.FullName]) {
            throw "Cleanup member changed identity: $($file.FullName)"
        }
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            continue
        }
        catch {
            $memberDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
            $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $file.FullName, '/remove:d', '*S-1-1-0')
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $file.FullName, '/inheritance:r', '/grant:r',
                "${currentName}:F", '*S-1-5-18:F')
            $afterAcl = Get-Item -LiteralPath $file.FullName -Force
            if (($afterAcl.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                (Get-PathIdentity $file.FullName) -ne $entryIdentities[$file.FullName] -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash -ne
                    $memberDigest) {
                throw "Cleanup member identity/content changed during ACL recovery: $($file.FullName)"
            }
            [IO.File]::SetAttributes($file.FullName,
                $current.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
        }
        Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
    }
    foreach ($directory in @($entries | Where-Object { $_.PSIsContainer } |
            Sort-Object { $_.FullName.Length } -Descending)) {
        if ((Get-PathIdentity $full) -ne $identity) {
            throw "Owned cleanup root identity changed: $full"
        }
        $current = Get-Item -LiteralPath $directory.FullName -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-PathIdentity $directory.FullName) -ne
                $entryIdentities[$directory.FullName]) {
            throw "Cleanup member changed identity: $($directory.FullName)"
        }
        $memberIdentity = $entryIdentities[$directory.FullName]
        try {
            Remove-Item -LiteralPath $directory.FullName -Force -ErrorAction Stop
        }
        catch {
            $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $directory.FullName, '/remove:d', '*S-1-1-0')
            Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
                $directory.FullName, '/inheritance:r', '/grant:r',
                "${currentName}:F", '*S-1-5-18:F')
            $afterAcl = Get-Item -LiteralPath $directory.FullName -Force
            if (($afterAcl.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                (Get-PathIdentity $directory.FullName) -ne $memberIdentity -or
                (Get-PathIdentity $full) -ne $identity) {
                throw "Cleanup directory identity changed during ACL recovery: $($directory.FullName)"
            }
            Remove-Item -LiteralPath $directory.FullName -Force -ErrorAction Stop
        }
    }
    if ((Get-PathIdentity $full) -ne $identity) {
        throw "Owned cleanup root identity changed before removal: $full"
    }
    Remove-Item -LiteralPath $full -Force
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Protect-Path([string]$Path, [switch]$Container) {
    Assert-NoReparseAncestor $Path
    $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $grant = if ($Container) { "${currentName}:(OI)(CI)F" } else { "${currentName}:F" }
    $systemGrant = if ($Container) { '*S-1-5-18:(OI)(CI)F' } else { '*S-1-5-18:F' }
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $Path, '/inheritance:r', '/grant:r', $grant, $systemGrant)
}

function Get-DeploymentSnapshot([string]$Root) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $items = @((Get-Item -LiteralPath $rootPath -Force)) +
        @(Get-ChildItem -LiteralPath $rootPath -Recurse -Force |
            Sort-Object FullName)
    $snapshot = foreach ($item in $items) {
        $relative = if ($item.FullName.Equals($rootPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            '.'
        }
        else { $item.FullName.Substring($rootPath.Length + 1).Replace('\', '/') }
        $acl = (Get-Acl -LiteralPath $item.FullName).Sddl
        if ($item.PSIsContainer) { "D|$relative|$acl" }
        else {
            $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash
            "F|$relative|$($item.Length)|$digest|$acl"
        }
    }
    return ($snapshot -join "`n")
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs, [string]$Failure) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $TimeoutMs) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw $Failure
}

Assert-ChildPath $build $repoBuild 'BuildDirectory'
Assert-ChildPath $deployment $repoBuild 'DeploymentDirectory'
Assert-NoReparseAncestor $build
Assert-NoReparseAncestor $deployment
if ($build.Equals($deployment, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and DeploymentDirectory must be distinct.'
}
if ($Clean -and
    (-not $build.Equals($defaultBuild, [StringComparison]::OrdinalIgnoreCase) -or
     -not $deployment.Equals($defaultDeployment, [StringComparison]::OrdinalIgnoreCase))) {
    throw '-Clean is restricted to the exact repository Task18 build/release paths.'
}

$deployScript = Join-Path $repo 'cmake\Deploy.cmake'
if ((Test-Path -LiteralPath $deployment) -and -not $Clean) {
    $before = Get-DeploymentSnapshot $deployment
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment", '-P', $deployScript)
    $after = Get-DeploymentSnapshot $deployment
    if ($before -ne $after) { throw 'Read-only deployment verification changed hash or ACL state.' }
    Write-Output "Verified existing accepted deployment without mutation: $deployment"
    return
}

if ($Clean) {
    Remove-OwnedTree $build $defaultBuild
    Remove-OwnedTree $deployment $defaultDeployment '.qbrowser-release-root' $releaseMarkerText
    Remove-OwnedTree $packageOutput $packageOutput
}
elseif (Test-Path -LiteralPath $build) {
    throw "Build output already exists; inspect it before using -Clean: $build"
}

$runId = [Guid]::NewGuid().ToString('N')
$taskTemp = Join-Path $repoBuild ".task18-release-temp-$runId"
$staging = Join-Path $repoBuild ".task18-release-deploy-$runId"
New-OwnedDirectory $taskTemp
New-OwnedDirectory $staging
Protect-Path $taskTemp -Container
Protect-Path $staging -Container
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousPath = $env:PATH
$node = (Get-Command node.exe -ErrorAction Stop).Source
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$env:QTEST_FUNCTION_TIMEOUT = '900000'

function New-SignedUpdatePackage([string]$Version, [string]$Destination) {
    $source = Join-Path $taskTemp "package-source-$Version"
    New-Item -ItemType Directory -Path $source | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $repo 'packages\pilot') -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Pilot source contains a reparse point: $($entry.FullName)"
        }
        Copy-Item -LiteralPath $entry.FullName -Destination $source -Recurse
    }
    $manifestPath = Join-Path $source 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.version = $Version
    [IO.File]::WriteAllText($manifestPath,
        ($manifest | ConvertTo-Json -Depth 20 -Compress), [Text.UTF8Encoding]::new($false))
    $unsigned = Join-Path $taskTemp "$Version.unsigned.qapkg"
    $cli = Join-Path $staging 'host\qbrowser-package.exe'
    $privateKey = Join-Path $repo '.qbrowser-dev\signing\private.pem'
    Invoke-Checked $cli @('pack', '--source', $source, '--output', $unsigned)
    Invoke-Checked $cli @('sign', '--package', $unsigned, '--private-key',
        $privateKey, '--output', $Destination)
    $inspection = & $cli inspect --package $Destination `
        --public-key (Join-Path $staging 'trust\dev-public.pem')
    if ($LASTEXITCODE -ne 0) { throw "Update $Version inspection failed." }
    $value = $inspection | ConvertFrom-Json
    if (-not $value.verified -or $value.appId -ne 'com.qbrowser.pilot' -or
        $value.version -ne $Version) {
        throw "Update $Version has unexpected signed identity."
    }
}

function Get-DeployedProcesses([string]$Name, [string]$ExpectedPath) {
    if ([string]::IsNullOrWhiteSpace($ExpectedPath)) {
        throw "Expected deployed process path is empty for $Name"
    }
    $normalized = [IO.Path]::GetFullPath($ExpectedPath)
    return @(Get-CimInstance Win32_Process -Filter "Name='$Name'" |
        Where-Object {
            if ([string]::IsNullOrWhiteSpace([string]$_.ExecutablePath)) { $false }
            else { try {
                [IO.Path]::GetFullPath([string]$_.ExecutablePath).Equals(
                    $normalized, [StringComparison]::OrdinalIgnoreCase)
            }
            catch { $false } }
        })
}

function Wait-DeployedWorker([int[]]$Excluded = @()) {
    $expected = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $script:observedWorker = $null
    Wait-Until {
        $candidate = @(Get-DeployedProcesses 'qbrowser-worker.exe' $expected |
            Where-Object { [int]$_.ProcessId -notin $Excluded }) | Select-Object -First 1
        if ($null -ne $candidate) { $script:observedWorker = $candidate; return $true }
        return $false
    } 30000 'The deployed LPAC Worker did not start from the staged runtime.'
    return $script:observedWorker
}

function Start-DeployedHost([string]$MockOrigin, [string]$Store,
        [string]$Sandbox, [string]$Telemetry, [string]$InstallPackage,
        [int]$HealthWindowMs) {
    $arguments = @('--package-mode', "--mock-origin=$MockOrigin",
        '--app-id=com.qbrowser.pilot',
        "--trusted-public-key=$(Join-Path $staging 'trust\dev-public.pem')",
        "--package-store=$Store", "--sandbox-temp=$Sandbox",
        "--runtime-root=$(Join-Path $staging 'runtime')",
        "--worker-executable=$(Join-Path $staging 'runtime\qbrowser-worker.exe')",
        "--telemetry-directory=$Telemetry", "--health-window-ms=$HealthWindowMs",
        '--heartbeat-timeout-ms=10000')
    if (-not [string]::IsNullOrEmpty($InstallPackage)) {
        $arguments += "--install-package=$InstallPackage"
    }
    $quoted = ($arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    $process = Start-Process -FilePath (Join-Path $staging 'host\qbrowser-host.exe') `
        -ArgumentList $quoted -PassThru
    Wait-Until { $process.Refresh(); $process.HasExited -or
        $process.MainWindowHandle -ne [IntPtr]::Zero } 30000 'Deployed Host window did not appear.'
    if ($process.HasExited) { throw "Deployed Host exited early: $($process.ExitCode)" }
    return $process
}

function Wait-Telemetry([string]$Telemetry, [string]$Pattern, [int]$TimeoutMs = 30000) {
    $file = Join-Path $Telemetry 'events.jsonl'
    Wait-Until { (Test-Path -LiteralPath $file -PathType Leaf) -and
        (Get-Content -LiteralPath $file -Raw) -match $Pattern } $TimeoutMs `
        "Telemetry did not contain required event: $Pattern"
}

function Stop-OwnedHost([Diagnostics.Process]$Process) {
    if ($Process.HasExited) { return }
    [void]$Process.CloseMainWindow()
    if ($Process.WaitForExit(15000)) { return }
    $Process.Refresh()
    $expectedHost = [IO.Path]::GetFullPath((Join-Path $staging 'host\qbrowser-host.exe'))
    if ($Process.HasExited) { return }
    if ([string]::IsNullOrWhiteSpace($Process.Path) -or
        -not [IO.Path]::GetFullPath($Process.Path).Equals(
            $expectedHost, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Host PID identity changed while closing: $($Process.Id)"
    }
    Stop-Process -Id $Process.Id -Force
    if (-not $Process.WaitForExit(10000)) {
        throw "Exact deployed Host remained after forced test cleanup: $($Process.Id)"
    }
}

function Invoke-DeployedRouteAcceptance([Diagnostics.Process]$AppProcess) {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $root = [System.Windows.Automation.AutomationElement]::FromHandle(
        [IntPtr]$AppProcess.MainWindowHandle)
    if ($null -eq $root) { throw 'Windows UI Automation could not open the Host window.' }
    $editCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Edit)
    $address = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        $editCondition)
    $goCondition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, 'Go'))
    $go = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $goCondition)
    if ($null -eq $address -or $null -eq $go) {
        throw 'Host navigation controls are unavailable to Windows UI Automation.'
    }
    $value = [System.Windows.Automation.ValuePattern]$address.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern)
    $invoke = [System.Windows.Automation.InvokePattern]$go.GetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern)
    $routes = @('app://pilot/login', 'app://pilot/dashboard',
        'app://pilot/orders', 'app://pilot/orders/ORD-1001',
        'app://pilot/orders/ORD-1001/edit', 'app://pilot/customers',
        'app://pilot/customers/CUS-001', 'app://pilot/files',
        'app://pilot/settings', 'app://pilot/web/help')
    $workerPath = Join-Path $staging 'runtime\qbrowser-worker.exe'
    foreach ($route in $routes) {
        $value.SetValue($route)
        $invoke.Invoke()
        Wait-Until { $value.Current.Value -eq $route } 10000 `
            "Host address did not accept route $route"
        $surfaceName = if ($route -eq 'app://pilot/web/help') {
            'Q-Browser Pilot Help'
        } else { 'qbrowser-worker' }
        $nameCondition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $surfaceName)
        Wait-Until { $null -ne $root.FindFirst(
                [System.Windows.Automation.TreeScope]::Descendants, $nameCondition) } `
            30000 "Deployed route did not expose '$surfaceName': $route"
        if ($AppProcess.HasExited) { throw "Host exited while navigating $route" }
        if ($route -ne 'app://pilot/web/help' -and
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw "LPAC Worker was not alive for route $route"
        }
    }
    $helperPath = Join-Path $staging 'host\QtWebEngineProcess.exe'
    Wait-Until { @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $helperPath).Count -gt 0 } `
        15000 'The WebEngine route did not start the deployed helper.'
    $helper = @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $helperPath)
    foreach ($process in $helper) {
        if ($process.CommandLine -match [regex]::Escape((Join-Path $repo 'packages') + '\') -or
            $process.CommandLine -match [regex]::Escape($build + '\') -or
            $process.CommandLine -match [regex]::Escape((Join-Path $QtRoot 'bin') + '\')) {
            throw 'WebEngine helper command line contains an implicit source path.'
        }
    }
    $value.SetValue('app://pilot/dashboard')
    $invoke.Invoke()
}

function Invoke-DeploymentOnlyE2E {
    $mockOut = Join-Path $taskTemp 'mock-api.stdout'
    $mockErr = Join-Path $taskTemp 'mock-api.stderr'
    $mock = $null
    $initialHostProcess = $null
    $secondHost = $null
    $ownedPids = [Collections.Generic.HashSet[int]]::new()
    $workerPath = Join-Path $staging 'runtime\qbrowser-worker.exe'
    $webEnginePath = Join-Path $staging 'host\QtWebEngineProcess.exe'
    try {
        $mock = Start-Process -FilePath $node -ArgumentList 'src/server.ts' `
            -WorkingDirectory (Join-Path $repo 'tools\mock-api') -WindowStyle Hidden `
            -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr -PassThru
        [void]$ownedPids.Add($mock.Id)
        $script:mockOrigin = $null
        Wait-Until {
            if (-not (Test-Path -LiteralPath $mockOut -PathType Leaf)) { return $false }
            $line = Get-Content -LiteralPath $mockOut -First 1 -ErrorAction SilentlyContinue
            if ($line) {
                try { $script:mockOrigin = ($line | ConvertFrom-Json).origin } catch { return $false }
            }
            return $script:mockOrigin -match '^http://127\.0\.0\.1:[1-9][0-9]*$'
        } 15000 "Deployed mock API did not start: $(Get-Content $mockErr -Raw -ErrorAction SilentlyContinue)"

        $state = Join-Path $taskTemp 'deployment-e2e'
        $store = Join-Path $state 'package-store'
        $sandbox = Join-Path $state 'sandbox-temp'
        $telemetry = Join-Path $state 'telemetry'
        New-Item -ItemType Directory -Path $store, $sandbox, $telemetry | Out-Null
        Protect-Path $store -Container
        Protect-Path $sandbox -Container
        Protect-Path $telemetry -Container
        $minimalPath = "$(Join-Path $staging 'host');$env:SystemRoot\System32;$env:SystemRoot"
        $env:PATH = $minimalPath
        $pilot = Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'
        $initialHostProcess = Start-DeployedHost $script:mockOrigin $store $sandbox $telemetry $pilot 1000
        [void]$ownedPids.Add($initialHostProcess.Id)
        $worker = Wait-DeployedWorker
        [void]$ownedPids.Add([int]$worker.ProcessId)
        if ($worker.CommandLine -match [regex]::Escape($QtRoot + '\bin') -or
            $worker.CommandLine -match [regex]::Escape((Join-Path $repo 'packages'))) {
            throw 'LPAC Worker command line contains an implicit source runtime path.'
        }
        Wait-Telemetry $telemetry '"packageVersion":"1\.0\.0","phase":"health","code":"healthy"' 30000
        Stop-OwnedHost $initialHostProcess
        Wait-Until { @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 } `
            15000 'Initial deployed Worker remained after Host shutdown.'

        $candidateRoot = Join-Path $taskTemp 'candidate'
        New-Item -ItemType Directory -Path $candidateRoot | Out-Null
        Protect-Path $candidateRoot -Container
        $update = Join-Path $candidateRoot 'com.qbrowser.pilot-1.1.0.qapkg'
        New-SignedUpdatePackage '1.1.0' $update
        Protect-Path $update
        $env:PATH = "C:\polluted-does-not-exist;$minimalPath"
        $secondHost = Start-DeployedHost $script:mockOrigin $store $sandbox $telemetry $update 60000
        [void]$ownedPids.Add($secondHost.Id)
        $firstUpdateWorker = Wait-DeployedWorker
        [void]$ownedPids.Add([int]$firstUpdateWorker.ProcessId)
        $activation = Join-Path $store 'apps\com.qbrowser.pilot\activation.json'
        Wait-Until {
            (Test-Path -LiteralPath $activation -PathType Leaf) -and
            ((Get-Content -LiteralPath $activation -Raw | ConvertFrom-Json).current `
                -like 'versions/1.1.0-*')
        } 30000 'Signed 1.1.0 update was not activated.'
        Start-Sleep -Seconds 5
        if ($secondHost.HasExited -or
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Signed 1.1.0 candidate was not stably running before crash injection.'
        }
        Stop-Process -Id ([int]$firstUpdateWorker.ProcessId) -Force
        Wait-Until { -not (Get-Process -Id ([int]$firstUpdateWorker.ProcessId) `
                -ErrorAction SilentlyContinue) } 10000 'First crashed Worker did not exit.'
        $restartedWorker = Wait-DeployedWorker @([int]$firstUpdateWorker.ProcessId)
        [void]$ownedPids.Add([int]$restartedWorker.ProcessId)
        Wait-Telemetry $telemetry '"packageVersion":"1\.1\.0","phase":"worker","code":"restarted"' 30000
        if (((Get-Content -LiteralPath $activation -Raw | ConvertFrom-Json).current `
                -notlike 'versions/1.1.0-*')) {
            throw 'The first crash did not retain the signed 1.1.0 candidate.'
        }
        Start-Sleep -Seconds 5
        if ($secondHost.HasExited -or
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Restarted 1.1.0 Worker was not stable before the second crash.'
        }
        Stop-Process -Id ([int]$restartedWorker.ProcessId) -Force
        Wait-Until { -not (Get-Process -Id ([int]$restartedWorker.ProcessId) `
                -ErrorAction SilentlyContinue) } 10000 'Second crashed Worker did not exit.'
        Wait-Telemetry $telemetry '"packageVersion":"1\.0\.0","phase":"rollback","code":"recovered"' 60000
        Wait-Until { ((Get-Content -LiteralPath $activation -Raw |
                ConvertFrom-Json).current -like 'versions/1.0.0-*') } 30000 `
            'Double crash did not recover the 1.0.0 last-known-good package.'
        $recoveryWorker = Wait-DeployedWorker @(
            [int]$firstUpdateWorker.ProcessId, [int]$restartedWorker.ProcessId)
        [void]$ownedPids.Add([int]$recoveryWorker.ProcessId)
        if ($secondHost.HasExited) { throw 'Host exited during Worker crash recovery.' }
        Start-Sleep -Seconds 5
        if (@(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -ne 1) {
            throw 'Recovered 1.0.0 Worker was not stable before route acceptance.'
        }
        Invoke-DeployedRouteAcceptance $secondHost
        Stop-OwnedHost $secondHost
        Wait-Until {
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 -and
            @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $webEnginePath).Count -eq 0
        } 20000 'Deployed Worker/WebEngine processes remained after acceptance.'
        Write-Output 'DEPLOYMENT_E2E_OK routes=10 webEngine=deployed update=1.1.0 rollback=1.0.0 paths=minimal+polluted'
    }
    catch {
        Write-Output "DEPLOYMENT_E2E_FAILURE=$($_.Exception.Message)"
        Write-Output "DEPLOYMENT_E2E_STACK=$($_.ScriptStackTrace)"
        if ((Get-Variable telemetry -ErrorAction SilentlyContinue) -and
            (Test-Path -LiteralPath (Join-Path $telemetry 'events.jsonl') -PathType Leaf)) {
            Write-Output 'DEPLOYMENT_E2E_TELEMETRY_TAIL_BEGIN'
            Get-Content -LiteralPath (Join-Path $telemetry 'events.jsonl') -Tail 40
            Write-Output 'DEPLOYMENT_E2E_TELEMETRY_TAIL_END'
        }
        throw
    }
    finally {
        $childDefinitions = @(
            [pscustomobject]@{ Name = 'qbrowser-worker.exe'; Path = Join-Path $staging 'runtime\qbrowser-worker.exe' }
            [pscustomobject]@{ Name = 'QtWebEngineProcess.exe'; Path = Join-Path $staging 'host\QtWebEngineProcess.exe' })
        foreach ($definition in $childDefinitions) {
            foreach ($owned in @(Get-DeployedProcesses $definition.Name $definition.Path)) {
                Stop-Process -Id ([int]$owned.ProcessId) -Force -ErrorAction SilentlyContinue
                Wait-Until { -not (Get-Process -Id ([int]$owned.ProcessId) `
                        -ErrorAction SilentlyContinue) } 10000 `
                    "Owned child process remained: $($owned.ProcessId)"
            }
        }
        foreach ($process in @($secondHost, $initialHostProcess, $mock)) {
            if ($null -ne $process -and -not $process.HasExited -and
                $ownedPids.Contains($process.Id)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                [void]$process.WaitForExit(10000)
            }
        }
        $env:PATH = $previousPath
    }
}

function Invoke-AdversarialDeploymentTests {
    $key = Join-Path $staging 'trust\dev-public.pem'
    $keyBackup = Join-Path $taskTemp 'public-key-backup.pem'
    Move-Item -LiteralPath $key -Destination $keyBackup
    New-Item -ItemType SymbolicLink -Path $key -Target $keyBackup | Out-Null
    try {
        $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=VERIFY' `
            "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
        if ($LASTEXITCODE -eq 0 -or ($output -join "`n") -notmatch 'reparse point rejected') {
            throw 'Verifier did not reject an externally linked trust key.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Force }
        Move-Item -LiteralPath $keyBackup -Destination $key
        Protect-Path $key
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_REPARSE_REJECT=PASS'

    $transitiveDependency = Join-Path $staging 'host\Qt6Qml.dll'
    $transitiveBackup = Join-Path $taskTemp 'Qt6Qml.dll.backup'
    Move-Item -LiteralPath $transitiveDependency -Destination $transitiveBackup
    try {
        $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=SEAL' `
            "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
        if ($LASTEXITCODE -eq 0 -or
            ($output -join "`n") -notmatch 'missing PE dependencies') {
            throw 'Verifier did not reject a missing transitive PE dependency.'
        }
    }
    finally {
        Move-Item -LiteralPath $transitiveBackup -Destination $transitiveDependency
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Write-Output 'ADVERSARIAL_PE_CLOSURE_REJECT=PASS dependency=Qt6Qml.dll'

    $privateProbe = Join-Path $staging 'docs\private-material-probe.txt'
    [IO.File]::WriteAllText($privateProbe,
        "-----BEGIN RSA PRIVATE KEY-----`n-----BEGIN EC PRIVATE KEY-----`n" +
        "-----BEGIN DSA PRIVATE KEY-----`n-----BEGIN OPENSSH PRIVATE KEY-----`n" +
        "-----BEGIN ENCRYPTED PRIVATE KEY-----`n-----BEGIN PRIVATE KEY-----`n",
        [Text.UTF8Encoding]::new($false))
    Protect-Path $privateProbe
    $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=SEAL' `
        "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
    if ($LASTEXITCODE -eq 0 -or ($output -join "`n") -notmatch 'private key material') {
        throw 'Verifier did not reject all PEM private-key forms by content.'
    }
    Remove-Item -LiteralPath $privateProbe -Force
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_PRIVATE_PEM_REJECT=PASS forms=RSA,EC,DSA,OpenSSH,PKCS8-encrypted,PKCS8-unencrypted'

    $package = Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'
    $packageBackup = Join-Path $taskTemp 'pilot-backup.qapkg'
    $manifest = Join-Path $staging 'SHA-256SUMS'
    $manifestBackup = Join-Path $taskTemp 'manifest-backup.txt'
    Copy-Item -LiteralPath $package -Destination $packageBackup
    Copy-Item -LiteralPath $manifest -Destination $manifestBackup
    $bytes = [IO.File]::ReadAllBytes($package)
    $bytes[[Math]::Min(32, $bytes.Length - 1)] = $bytes[[Math]::Min(32, $bytes.Length - 1)] -bxor 1
    [IO.File]::WriteAllBytes($package, $bytes)
    $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=SEAL' `
        "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
    if ($LASTEXITCODE -eq 0 -or ($output -join "`n") -notmatch 'signature/identity') {
        throw 'Regenerated hashes blessed a tampered signed package.'
    }
    Copy-Item -LiteralPath $packageBackup -Destination $package -Force
    Copy-Item -LiteralPath $manifestBackup -Destination $manifest -Force
    Protect-Path $package
    Protect-Path $manifest
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_TAMPERED_SIGNED_PACKAGE_REJECT=PASS regeneratedHashes=true'
}

function Test-CleanupReparseDefense {
    $external = Join-Path $taskTemp 'external-sentinel'
    $junction = Join-Path $taskTemp 'owned-junction-attack'
    New-Item -ItemType Directory -Path $external | Out-Null
    [IO.File]::WriteAllText((Join-Path $external 'sentinel.txt'), 'must-survive',
        [Text.UTF8Encoding]::new($false))
    New-Item -ItemType Junction -Path $junction -Target $external | Out-Null
    $rejected = $false
    try { Remove-OwnedTree $junction $junction } catch { $rejected = $true }
    if (-not $rejected -or
        -not (Test-Path -LiteralPath (Join-Path $external 'sentinel.txt')) -or
        -not (Test-Path -LiteralPath $junction)) {
        throw 'Cleanup reparse defense mutated or accepted the external sentinel link.'
    }
    Remove-Item -LiteralPath $junction -Force
    Write-Output 'CLEANUP_REPARSE_DEFENSE=PASS externalSentinel=unchanged'
}

$buildParentIdentity = Get-PathIdentity $repoBuild
try {
    Test-CleanupReparseDefense
    New-OwnedDirectory $build
    Invoke-Checked $CMake @('-S', $repo, '-B', $build,
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_PREFIX_PATH=$QtRoot", "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        '-DBUILD_TESTING=OFF', '-DQ_BROWSER_BUILD_WEBENGINE=ON')
    Invoke-Checked $CMake @('--build', $build, '--config', 'Release', '--parallel', '2')
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
    if ($cache -notmatch '(?m)^BUILD_TESTING:BOOL=OFF\r?$') {
        throw 'Release build unexpectedly enabled test code.'
    }
    $ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
    $testInventory = & $ctest --test-dir $build -C Release -N
    if ($LASTEXITCODE -ne 0 -or ($testInventory -join "`n") -notmatch 'Total Tests: 0') {
        throw 'Production Release build contains tests or CTest inventory failed.'
    }

    & (Join-Path $repo 'scripts\create-dev-package.ps1') -Configuration Release `
        -BuildDirectory $build -OutputDirectory $packageOutput -Clean:$Clean
    if ($LASTEXITCODE -ne 0) { throw 'Development Pilot package creation failed.' }

    $stagingHost = Join-Path $staging 'host'
    $stagingRuntime = Join-Path $staging 'runtime'
    New-Item -ItemType Directory -Path $stagingHost, $stagingRuntime | Out-Null
    Invoke-Checked $CMake @('--install', $build, '--config', 'Release',
        '--prefix', $stagingHost, '--component', 'Runtime')
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=ASSEMBLE',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", "-DQ_BROWSER_REPO_ROOT=$repo",
        "-DQ_BROWSER_QT_ROOT=$QtRoot", "-DQ_BROWSER_OPENSSL_ROOT=$OpenSslRoot",
        "-DQ_BROWSER_PACKAGE_FILE=$(Join-Path $packageOutput 'com.qbrowser.pilot-1.0.0.qapkg')",
        "-DQ_BROWSER_PUBLIC_KEY=$(Join-Path $packageOutput 'dev-public.pem')",
        '-P', $deployScript)
    foreach ($container in @($staging, $stagingHost, $stagingRuntime,
            (Join-Path $staging 'packages'), (Join-Path $staging 'trust'))) {
        Protect-Path $container -Container
    }
    foreach ($file in @((Join-Path $staging 'packages\com.qbrowser.pilot-1.0.0.qapkg'),
            (Join-Path $staging 'trust\dev-public.pem'),
            (Join-Path $staging 'SHA-256SUMS'))) {
        Protect-Path $file
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=PREVERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)

    $forbiddenSymbols = @('qbrowser_host_testing', 'qbrowser_archive_testing',
        'forceLifecycleQueueFullForTesting', 'retryWorkerCleanupForTesting')
    foreach ($binary in Get-ChildItem -LiteralPath $staging -Recurse -File -Filter '*.exe') {
        foreach ($symbol in $forbiddenSymbols) {
            & "$env:SystemRoot\System32\findstr.exe" /P /M /C:$symbol $binary.FullName | Out-Null
            if ($LASTEXITCODE -eq 0) {
                throw "Production binary contains test-hook surface '$symbol': $($binary.Name)"
            }
        }
    }

    Invoke-DeploymentOnlyE2E
    [IO.File]::WriteAllText((Join-Path $staging 'release-attestation.json'),
        "{`"schema`":1,`"deploymentOnlyE2E`":true,`"routeCount`":10," +
        "`"webEngine`":`"deployed`",`"signedUpdate`":`"1.1.0`"," +
        "`"rollback`":`"1.0.0`"}`n", [Text.UTF8Encoding]::new($false))
    Protect-Path (Join-Path $staging 'release-attestation.json')
    Remove-Item -LiteralPath (Join-Path $staging $ownedMarkerName) -Force
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Invoke-AdversarialDeploymentTests

    if ($RunAcceptance) {
        $env:PATH = $previousPath
        & (Join-Path $repo 'scripts\run-acceptance.ps1') -Configuration Release `
            -BuildDirectory (Join-Path $repoBuild 'release-acceptance') `
            -QtRoot $QtRoot -OpenSslRoot $OpenSslRoot
        if ($LASTEXITCODE -ne 0) { throw 'Release acceptance failed.' }
    }

    $privatePattern = '^\s*-----BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----'
    $privateScanRoots = @($build, $packageOutput, $staging)
    if ($RunAcceptance) { $privateScanRoots += Join-Path $repoBuild 'release-acceptance' }
    foreach ($file in $privateScanRoots | ForEach-Object {
            Get-ChildItem -LiteralPath $_ -File -Recurse -Force }) {
        if ($file.Length -le 0) { continue }
        $stream = $file.OpenRead()
        try {
            $bytes = [byte[]]::new([int][Math]::Min(8192L, $file.Length))
            $count = $stream.Read($bytes, 0, $bytes.Length)
        }
        finally { $stream.Dispose() }
        $prefix = [Text.Encoding]::ASCII.GetString($bytes, 0, $count)
        if ($prefix -match $privatePattern) {
            throw "Build/acceptance output contains PEM private key material: $($file.FullName)"
        }
    }

    $minimalPath = "$(Join-Path $staging 'host');$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($verificationPath in @($minimalPath, "C:\polluted-does-not-exist;$minimalPath")) {
        $env:PATH = $verificationPath
        Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
            "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    }
    $env:PATH = $previousPath
    if ($FailureInjection -eq 'BeforePublish') {
        throw 'Injected Task18 failure before atomic publication.'
    }
    if ((Get-PathIdentity $repoBuild) -ne $buildParentIdentity) {
        throw 'Repository build root identity changed before publication.'
    }
    Assert-NoReparseAncestor $deployment
    if (Test-Path -LiteralPath $deployment) {
        throw "Deployment appeared during staging; refusing to overwrite: $deployment"
    }
    Move-Item -LiteralPath $staging -Destination $deployment
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment", '-P', $deployScript)
    Write-Output "Q-Browser Release deployment created and accepted: $deployment"
}
finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:PATH = $previousPath
    if (Test-Path -LiteralPath $staging) {
        if (Test-Path -LiteralPath (Join-Path $staging '.qbrowser-release-root')) {
            Remove-OwnedTree $staging $staging '.qbrowser-release-root' $releaseMarkerText
        }
        else { Remove-OwnedTree $staging $staging }
    }
    if (Test-Path -LiteralPath $taskTemp) {
        Remove-OwnedTree $taskTemp $taskTemp
    }
}
