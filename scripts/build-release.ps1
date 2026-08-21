[CmdletBinding()]
param(
    [string]$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64',
    [string]$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64',
    [string]$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe',
    [string]$BuildDirectory = '',
    [string]$DeploymentDirectory = '',
    [switch]$Clean,
    [bool]$RunAcceptance = $true,
    [string]$PrepareManualState = '',
    [ValidateSet('', 'BeforePublish')]
    [string]$FailureInjection = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('QBrowser.Task18.FileIdentity' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace QBrowser.Task18 {
  public static class FileIdentity {
    [StructLayout(LayoutKind.Sequential)] struct Info {
      public uint attributes; public System.Runtime.InteropServices.ComTypes.FILETIME creation;
      public System.Runtime.InteropServices.ComTypes.FILETIME access;
      public System.Runtime.InteropServices.ComTypes.FILETIME write;
      public uint volume; public uint sizeHigh; public uint sizeLow; public uint links;
      public uint indexHigh; public uint indexLow;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Info info);
    public static string Read(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      using (var handle = CreateFileW(native, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        Info info; if (!GetFileInformationByHandle(handle, out info))
          throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        return info.volume.ToString("x8") + ":" + info.indexHigh.ToString("x8") + info.indexLow.ToString("x8");
      }
    }
  }
  public static class ProcessControl {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(
      uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr handle);
    [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr handle);
    static void Apply(int processId, bool suspend) {
      IntPtr handle = OpenProcess(0x0800, false, processId);
      if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
      try {
        int status = suspend ? NtSuspendProcess(handle) : NtResumeProcess(handle);
        if (status != 0) throw new Win32Exception("NTSTATUS 0x" + status.ToString("x8"));
      } finally { CloseHandle(handle); }
    }
    public static void Suspend(int processId) { Apply(processId, true); }
    public static void Resume(int processId) { Apply(processId, false); }
  }
}
'@
}

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
    return [QBrowser.Task18.FileIdentity]::Read($Path)
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
    foreach ($file in @($entries | Where-Object {
            -not $_.PSIsContainer -and
            -not $_.FullName.Equals($marker, [StringComparison]::OrdinalIgnoreCase) } |
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
    if ((Get-PathIdentity $marker) -ne $entryIdentities[$marker]) {
        throw "Owned cleanup marker identity changed: $marker"
    }
    Remove-Item -LiteralPath $marker -Force -ErrorAction Stop
    Remove-Item -LiteralPath $full -Force
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Protect-Path([string]$Path, [switch]$Container) {
    Assert-NoReparseAncestor $Path
    $item = Get-Item -LiteralPath $Path -Force
    if ($Container -and -not $item.PSIsContainer) { throw "Expected directory: $Path" }
    if (-not $Container -and $item.PSIsContainer) { throw "Expected file: $Path" }
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $existingAcl = Get-Acl -LiteralPath $Path
    $trustedSids = @($current.Value, $system.Value)
    $identities = @($existingAcl.GetAccessRules($true, $false,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -notin $trustedSids } |
        ForEach-Object { $_.IdentityReference.Value } | Sort-Object -Unique)
    foreach ($identity in $identities) {
        Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
            $Path, '/remove:g', "*$identity")
    }
    $grants = if ($Container) {
        @("${currentName}:(OI)(CI)F", '*S-1-5-18:(OI)(CI)F')
    } else { @("${currentName}:F", '*S-1-5-18:F') }
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" `
        (@($Path, '/inheritance:r', '/grant:r') + $grants)
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $Path, '/setowner', $currentName)
    $verified = Get-Acl -LiteralPath $Path
    $allowed = @($current.Value, $system.Value)
    $ownerSid = ([Security.Principal.NTAccount]$verified.Owner).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    if (-not $verified.AreAccessRulesProtected -or $ownerSid -ne $current.Value -or
        @($verified.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -notin $allowed }).Count -ne 0) {
        throw "ACL sanitization failed: $Path"
    }
}

function Assert-ProtectedPath([string]$Path) {
    Assert-NoReparseAncestor $Path
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $trusted = @($current, 'S-1-5-18')
    $acl = Get-Acl -LiteralPath $Path
    $owner = ([Security.Principal.NTAccount]$acl.Owner).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    $untrustedAllow = @($acl.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
            $_.AccessControlType -eq 'Allow' -and
            $_.IdentityReference.Value -notin $trusted })
    if (-not $acl.AreAccessRulesProtected -or $owner -ne $current -or
        $untrustedAllow.Count -ne 0) {
        throw "Unsafe ACL: $Path"
    }
}

function Assert-PlainTree([string]$Path) {
    Assert-NoReparseAncestor $Path
    foreach ($entry in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse member is forbidden: $($entry.FullName)"
        }
    }
}

function Test-PrivatePem([string]$Path) {
    $pattern = [regex]::new(
        '(?m)^[ \t]*-----BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----')
    $reader = [IO.StreamReader]::new($Path, [Text.Encoding]::ASCII, $false, 65536)
    try {
        $buffer = [char[]]::new(65536); $carry = ''
        while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $text = $carry + [string]::new($buffer, 0, $count)
            if ($pattern.IsMatch($text)) { return $true }
            $carry = if ($text.Length -gt 128) {
                $text.Substring($text.Length - 128)
            } else { $text }
        }
        return $false
    }
    finally { $reader.Dispose() }
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

if (-not [string]::IsNullOrWhiteSpace($PrepareManualState)) {
    $manualState = [IO.Path]::GetFullPath($PrepareManualState)
    $expectedManualState = [IO.Path]::GetFullPath(
        (Join-Path $repoBuild 'manual-deployed-smoke'))
    if (-not $manualState.Equals($expectedManualState,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manual state is restricted to $expectedManualState"
    }
    Assert-NoReparseAncestor $manualState
    New-Item -ItemType Directory -Path $manualState -Force | Out-Null
    Protect-Path $manualState -Container
    Assert-PlainTree $manualState
    foreach ($name in @('package-store','sandbox-temp','telemetry')) {
        $directory = Join-Path $manualState $name
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Protect-Path $directory -Container
    }
    Assert-PlainTree $manualState
    Write-Output "Protected manual deployment state prepared: $manualState"
    return
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
$previousSourceDateEpoch = [Environment]::GetEnvironmentVariable(
    'SOURCE_DATE_EPOCH', 'Process')
$loaderEnvironmentNames = @('QML_IMPORT_PATH','QML2_IMPORT_PATH','QT_PLUGIN_PATH',
    'QT_QPA_PLATFORM_PLUGIN_PATH','QTWEBENGINEPROCESS_PATH','OPENSSL_CONF',
    'OPENSSL_MODULES','QTDIR','QT_ROOT_DIR','Qt6_DIR','CMAKE_PREFIX_PATH')
$previousLoaderEnvironment = @{}
foreach ($name in $loaderEnvironmentNames) {
    $previousLoaderEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$node = (Get-Command node.exe -ErrorAction Stop).Source
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$env:SOURCE_DATE_EPOCH = '946684800'
$env:QTEST_FUNCTION_TIMEOUT = '900000'

function New-SignedUpdatePackage([string]$Version, [string]$Destination) {
    $source = Join-Path $taskTemp "package-source-$Version"
    New-Item -ItemType Directory -Path $source | Out-Null
    $pilotSource = Join-Path $repo 'packages\pilot'
    Assert-PlainTree $pilotSource
    foreach ($entry in Get-ChildItem -LiteralPath $pilotSource -Force) {
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
    $started = [Diagnostics.Stopwatch]::StartNew()
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $processCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $Process.Id)
    $window = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
        [System.Windows.Automation.TreeScope]::Children, $processCondition)
    if ($null -eq $window) {
        throw "Exact deployed Host top-level window is unavailable: $($Process.Id)"
    }
    $windowPattern = [System.Windows.Automation.WindowPattern]$window.GetCurrentPattern(
        [System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    if ($Process.WaitForExit(60000)) {
        Write-Output "DEPLOYED_HOST_CLEAN_EXIT_MS=$($started.ElapsedMilliseconds)"
        return
    }
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

function Get-AclTreeSnapshot([string[]]$Roots) {
    $lines = foreach ($rootPath in $Roots) {
        $rootFull = [IO.Path]::GetFullPath($rootPath).TrimEnd('\')
        foreach ($item in @((Get-Item -LiteralPath $rootFull -Force)) +
                @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force |
                    Sort-Object FullName)) {
            $relative = $item.FullName.Substring($rootFull.Length).TrimStart('\').Replace('\','/')
            "$rootFull|$relative|$((Get-Acl -LiteralPath $item.FullName).Sddl)"
        }
    }
    return ($lines -join "`n")
}

function Assert-AclLeaseRestored([string]$Before, [string[]]$Roots, [string]$Label,
        [switch]$AllowAdditional, [switch]$IgnoreActivationLockLifecycle) {
    $after = Get-AclTreeSnapshot $Roots
    if ($AllowAdditional) {
        $afterLines = @($after -split "`n")
        foreach ($line in @($Before -split "`n")) {
            if ($IgnoreActivationLockLifecycle -and
                $line -match '\|apps/com\.qbrowser\.pilot/\.activation\.lock\|') {
                continue
            }
            if ($line -notin $afterLines) {
                throw "$Label changed a pre-existing ACL lease: $line"
            }
        }
    }
    elseif ($after -ne $Before) {
        $beforeLines = @($Before -split "`n")
        $afterLines = @($after -split "`n")
        foreach ($line in @($beforeLines | Where-Object { $_ -notin $afterLines } |
                Select-Object -First 10)) {
            Write-Output "ACL_LEASE_MISSING=$line"
        }
        foreach ($line in @($afterLines | Where-Object { $_ -notin $beforeLines } |
                Select-Object -First 10)) {
            Write-Output "ACL_LEASE_ADDED=$line"
        }
        throw "$Label ACL lease was not restored exactly."
    }
    if ($after -match 'S-1-15-2-') { throw "$Label retained an AppContainer SID ACE." }
}

function Invoke-DeployedRouteAcceptance([Diagnostics.Process]$AppProcess,
        [string]$Telemetry, [int]$NegativeWorkerPid = 0) {
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
    $templates = @{
        'app://pilot/login'='/login'; 'app://pilot/dashboard'='/dashboard'
        'app://pilot/orders'='/orders'; 'app://pilot/orders/ORD-1001'='/orders/:id'
        'app://pilot/orders/ORD-1001/edit'='/orders/:id/edit'
        'app://pilot/customers'='/customers'
        'app://pilot/customers/CUS-001'='/customers/:id'
        'app://pilot/files'='/files'; 'app://pilot/settings'='/settings'
    }
    $eventFile = Join-Path $Telemetry 'events.jsonl'
    if ($NegativeWorkerPid -gt 0) {
        $negativeRoute = 'app://pilot/settings'
        $negativePattern = '"phase":"worker","code":"completed".*' +
            '"routeTemplate":"/settings".*"queueDepth":0'
        $before = if (Test-Path $eventFile) {
            @([regex]::Matches((Get-Content $eventFile -Raw), $negativePattern)).Count
        } else { 0 }
        [QBrowser.Task18.ProcessControl]::Suspend($NegativeWorkerPid)
        $missingAckDetected = $false
        try {
            try {
                $value.SetValue($negativeRoute); $invoke.Invoke()
                try {
                    Wait-Until {
                        (Test-Path $eventFile) -and
                        @([regex]::Matches((Get-Content $eventFile -Raw), $negativePattern)).Count `
                            -gt $before
                    } 6000 'Injected ignored route produced no acknowledgement.'
                }
                catch { $missingAckDetected = $true }
            }
            catch {
                if ($_.Exception.ToString() -notmatch 'Operation timed out|0x80131505') {
                    throw
                }
                $missingAckDetected = $true
            }
        }
        finally { [QBrowser.Task18.ProcessControl]::Resume($NegativeWorkerPid) }
        if (-not $missingAckDetected) {
            throw 'Negative route injection was incorrectly accepted.'
        }
        Write-Output 'DEPLOYMENT_ROUTE_NEGATIVE=PASS ignoredWorkerAck=rejected'
        return
    }
    foreach ($route in $routes) {
        $template = $templates[$route]
        $ackPattern = if ($null -ne $template) {
            '"phase":"worker","code":"completed".*"routeTemplate":"' +
                [regex]::Escape($template) + '".*"queueDepth":0'
        } else { $null }
        $ackBefore = if ($null -ne $ackPattern -and (Test-Path $eventFile)) {
            @([regex]::Matches((Get-Content $eventFile -Raw), $ackPattern)).Count
        } else { 0 }
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
        if ($null -ne $ackPattern) {
            Wait-Until {
                (Test-Path $eventFile) -and
                @([regex]::Matches((Get-Content $eventFile -Raw), $ackPattern)).Count `
                    -gt $ackBefore
            } 15000 "Worker did not acknowledge normalized route with pending=0: $route"
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
        foreach ($name in $loaderEnvironmentNames) {
            [Environment]::SetEnvironmentVariable(
                $name, $null, 'Process')
        }
        Write-Output "DEPLOYMENT_LOADER_ENV_CLEARED=$($loaderEnvironmentNames -join ',')"
        $mock = Start-Process -FilePath $node -ArgumentList 'src/server.ts' `
            -WorkingDirectory (Join-Path $repo 'tools\mock-api') -WindowStyle Hidden `
            -RedirectStandardOutput $mockOut -RedirectStandardError $mockErr -PassThru
        [void]$ownedPids.Add($mock.Id)
        $script:mockOrigin = $null
        Wait-Until {
            $mock.Refresh()
            if ($mock.HasExited) {
                throw "Mock API exited $($mock.ExitCode): $(Get-Content $mockErr -Raw -ErrorAction SilentlyContinue)"
            }
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
        $deployAclRoots = @((Join-Path $staging 'runtime'), (Join-Path $staging 'packages'))
        $deployAclBefore = Get-AclTreeSnapshot $deployAclRoots
        $initialHostProcess = Start-DeployedHost $script:mockOrigin $store $sandbox $telemetry $pilot 1000
        [void]$ownedPids.Add($initialHostProcess.Id)
        $worker = Wait-DeployedWorker
        [void]$ownedPids.Add([int]$worker.ProcessId)
        if ($worker.CommandLine -match [regex]::Escape($QtRoot + '\bin') -or
            $worker.CommandLine -match [regex]::Escape((Join-Path $repo 'packages'))) {
            throw 'LPAC Worker command line contains an implicit source runtime path.'
        }
        Wait-Telemetry $telemetry '"packageVersion":"1\.0\.0","phase":"health","code":"healthy"' 30000
        Invoke-DeployedRouteAcceptance $initialHostProcess $telemetry `
            -NegativeWorkerPid ([int]$worker.ProcessId)
        Stop-OwnedHost $initialHostProcess
        if ($initialHostProcess.ExitCode -ne 0) {
            throw "Initial deployed Host cleanup exited $($initialHostProcess.ExitCode)."
        }
        Wait-Until { @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 } `
            15000 'Initial deployed Worker remained after Host shutdown.'
        Assert-AclLeaseRestored $deployAclBefore $deployAclRoots 'Initial Host'
        $storeAclBefore = Get-AclTreeSnapshot @($store)

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
        Invoke-DeployedRouteAcceptance $secondHost $telemetry
        Stop-OwnedHost $secondHost
        if ($secondHost.ExitCode -ne 0) {
            throw "Updated deployed Host cleanup exited $($secondHost.ExitCode)."
        }
        Wait-Until {
            @(Get-DeployedProcesses 'qbrowser-worker.exe' $workerPath).Count -eq 0 -and
            @(Get-DeployedProcesses 'QtWebEngineProcess.exe' $webEnginePath).Count -eq 0
        } 20000 'Deployed Worker/WebEngine processes remained after acceptance.'
        Assert-AclLeaseRestored $deployAclBefore $deployAclRoots 'Updated Host deployment'
        Assert-AclLeaseRestored $storeAclBefore @($store) 'Updated Host package store' `
            -AllowAdditional -IgnoreActivationLockLifecycle
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
        foreach ($name in $loaderEnvironmentNames) {
            [Environment]::SetEnvironmentVariable(
                $name, $previousLoaderEnvironment[$name], 'Process')
        }
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

    $bogusDirectory = Join-Path $staging 'host\bogus-loader-directory'
    New-Item -ItemType Directory -Path $bogusDirectory | Out-Null
    Move-Item -LiteralPath $transitiveDependency `
        -Destination (Join-Path $bogusDirectory 'Qt6Qml.dll')
    try {
        $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=SEAL' `
            "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
        if ($LASTEXITCODE -eq 0 -or
            ($output -join "`n") -notmatch 'missing PE dependencies') {
            throw 'Verifier accepted a dependency moved outside loader search directories.'
        }
    }
    finally {
        Move-Item -LiteralPath (Join-Path $bogusDirectory 'Qt6Qml.dll') `
            -Destination $transitiveDependency
        Remove-Item -LiteralPath $bogusDirectory -Force
    }
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Protect-Path (Join-Path $staging 'SHA-256SUMS')
    Write-Output 'ADVERSARIAL_PE_MOVED_REJECT=PASS loaderSearch=importer+closureRoot'

    $privateProbe = Join-Path $staging 'docs\private-material-probe.txt'
    $privateLabels = @('RSA PRIVATE KEY','EC PRIVATE KEY','DSA PRIVATE KEY',
        'OPENSSH PRIVATE KEY','ENCRYPTED PRIVATE KEY','PRIVATE KEY')
    foreach ($privateLabel in $privateLabels) {
        $preamble = if ($privateLabel -eq 'RSA PRIVATE KEY') { 'x' * 9000 } else { '' }
        [IO.File]::WriteAllText($privateProbe,
            "$preamble`n-----BEGIN $privateLabel-----`n",
            [Text.UTF8Encoding]::new($false))
        Protect-Path $privateProbe
        $output = & $CMake '-DQ_BROWSER_DEPLOY_MODE=SEAL' `
            "-DQ_BROWSER_DEPLOY_DIR=$staging" '-P' $deployScript 2>&1
        if ($LASTEXITCODE -eq 0 -or
            ($output -join "`n") -notmatch 'forbidden PEM material') {
            throw "Verifier did not reject PEM form: $privateLabel"
        }
    }
    Remove-Item -LiteralPath $privateProbe -Force
    Invoke-Checked $CMake @('-DQ_BROWSER_DEPLOY_MODE=SEAL',
        "-DQ_BROWSER_DEPLOY_DIR=$staging", '-P', $deployScript)
    Write-Output 'ADVERSARIAL_PRIVATE_PEM_REJECT=PASS preamble=9000 forms=RSA,EC,DSA,OpenSSH,PKCS8-encrypted,PKCS8-unencrypted'

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

    $longRoot = Join-Path $taskTemp 'owned-long-path-cleanup'
    New-OwnedDirectory $longRoot
    Protect-Path $longRoot -Container
    $deep = $longRoot
    while ($deep.Length -lt 280) {
        $deep = Join-Path $deep ('segment-' + ('x' * 20))
        [void][IO.Directory]::CreateDirectory('\\?\' + $deep)
    }
    [IO.File]::WriteAllText('\\?\' + (Join-Path $deep 'sentinel.txt'),
        'owned', [Text.UTF8Encoding]::new($false))
    Remove-OwnedTree $longRoot $longRoot
    if (Test-Path -LiteralPath $longRoot) {
        throw 'Long-path owned cleanup left residue.'
    }
    Write-Output "CLEANUP_LONG_PATH=PASS length=$($deep.Length) residue=0"
}

$buildParentIdentity = Get-PathIdentity $repoBuild
$primaryFailure = $null
try {
    Test-CleanupReparseDefense
    New-OwnedDirectory $build
    Protect-Path $build -Container
    $aclProbe = Join-Path $build 'acl-tamper-probe.txt'
    [IO.File]::WriteAllText($aclProbe, 'probe', [Text.UTF8Encoding]::new($false))
    Protect-Path $aclProbe
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $aclProbe, '/grant', '*S-1-5-11:R')
    $aclRejected = $false
    try { Assert-ProtectedPath $aclProbe } catch { $aclRejected = $true }
    if (-not $aclRejected) { throw 'Build input ACL tamper was accepted.' }
    Protect-Path $aclProbe
    Remove-Item -LiteralPath $aclProbe -Force
    $reparseProbe = Join-Path $build 'reparse-tamper-probe'
    New-Item -ItemType Junction -Path $reparseProbe -Target $taskTemp | Out-Null
    $reparseRejected = $false
    try { Assert-PlainTree $build } catch { $reparseRejected = $true }
    if (-not $reparseRejected) { throw 'Build input reparse tamper was accepted.' }
    Remove-Item -LiteralPath $reparseProbe -Force
    Assert-ProtectedPath $build
    Assert-PlainTree $build
    Write-Output 'BUILD_INPUT_TAMPER_DEFENSE=PASS acl=rejected reparse=rejected'
    Invoke-Checked $CMake @('-S', $repo, '-B', $build,
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_PREFIX_PATH=$QtRoot", "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        '-DBUILD_TESTING=OFF', '-DQ_BROWSER_BUILD_WEBENGINE=ON')
    Invoke-Checked $CMake @('--build', $build, '--config', 'Release', '--parallel', '2')
    Assert-ProtectedPath $build
    Assert-PlainTree $build
    $releaseHost = Join-Path $build 'apps\host\Release\qbrowser-host.exe'
    $hostAcl = Get-Acl -LiteralPath $releaseHost
    if (@($hostAcl.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
            $_.AccessControlType -eq 'Allow' -and
            $_.IdentityReference.Value -notin @(
                [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
                'S-1-5-18') }).Count -ne 0) {
        throw 'Generated Release Host is writable/readable by an untrusted principal.'
    }
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

    $privateScanRoots = @($build, $packageOutput, $staging)
    if ($RunAcceptance) { $privateScanRoots += Join-Path $repoBuild 'release-acceptance' }
    foreach ($file in $privateScanRoots | ForEach-Object {
            Get-ChildItem -LiteralPath $_ -File -Recurse -Force }) {
        if ($file.Length -gt 0 -and (Test-PrivatePem $file.FullName)) {
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
    foreach ($name in $loaderEnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousLoaderEnvironment[$name], 'Process')
    }
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
catch {
    $primaryFailure = $_
    throw
}
finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:PATH = $previousPath
    [Environment]::SetEnvironmentVariable(
        'SOURCE_DATE_EPOCH', $previousSourceDateEpoch, 'Process')
    foreach ($name in $loaderEnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousLoaderEnvironment[$name], 'Process')
    }
    $cleanupFailures = @()
    try {
        if (Test-Path -LiteralPath $staging) {
            if (Test-Path -LiteralPath (Join-Path $staging '.qbrowser-release-root')) {
                Remove-OwnedTree $staging $staging '.qbrowser-release-root' $releaseMarkerText
            }
            else { Remove-OwnedTree $staging $staging }
        }
    }
    catch { $cleanupFailures += "staging cleanup: $($_.Exception.Message)" }
    try {
        if (Test-Path -LiteralPath $taskTemp) {
            Remove-OwnedTree $taskTemp $taskTemp
        }
    }
    catch { $cleanupFailures += "temporary cleanup: $($_.Exception.Message)" }
    if ($cleanupFailures.Count -ne 0) {
        $cleanupMessage = $cleanupFailures -join '; '
        if ($null -ne $primaryFailure) {
            Write-Error "Secondary cleanup failure after primary '$($primaryFailure.Exception.Message)': $cleanupMessage" -ErrorAction Continue
        }
        else { throw "Release cleanup failed: $cleanupMessage" }
    }
}
