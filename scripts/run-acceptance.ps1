[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$BuildDirectory = '',
    [string]$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64',
    [string]$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('QBrowser.Task18.FileIdentity' -as [type]) -and
    -not ('QBrowser.Task18.ReparseDirectory' -as [type])) {
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
  public static class ReparseDirectory {
    const uint MountPointTag = 0xA0000003;
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
      IntPtr input, uint inputLength, byte[] output, uint outputLength,
      out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool RemoveDirectoryW(string path);
    static string Native(string path) {
      string full = System.IO.Path.GetFullPath(path);
      return full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                     : @"\\?\" + full;
    }
    public static uint ReadTag(string path) {
      string full = System.IO.Path.GetFullPath(path);
      using (var handle = CreateFileW(Native(full), 0, 7, IntPtr.Zero, 3,
                                     0x00200000 | 0x02000000, IntPtr.Zero)) {
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        byte[] buffer = new byte[16384]; uint returned;
        if (!DeviceIoControl(handle, 0x000900A8, IntPtr.Zero, 0,
                             buffer, (uint)buffer.Length, out returned, IntPtr.Zero))
          throw new Win32Exception(Marshal.GetLastWin32Error(), full);
        if (returned < 8) throw new InvalidOperationException("Invalid reparse buffer: " + full);
        return BitConverter.ToUInt32(buffer, 0);
      }
    }
    public static void RemoveVerifiedMountPoint(string path) {
      string full = System.IO.Path.GetFullPath(path);
      if (ReadTag(full) != MountPointTag)
        throw new InvalidOperationException("Not a mount-point reparse entry: " + full);
      if (!RemoveDirectoryW(Native(full)))
        throw new Win32Exception(Marshal.GetLastWin32Error(), full);
    }
  }
}
'@
}
elseif (-not ('QBrowser.Task18.FileIdentity' -as [type]) -or
        -not ('QBrowser.Task18.ReparseDirectory' -as [type])) {
    throw 'Task18 native path helpers are only partially loaded.'
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path ([Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData)) `
        "QBrowserTask18\build\acceptance-$($Configuration.ToLowerInvariant())"
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
$cmake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe'
$ctest = 'E:\DevEnv\qt\Tools\CMake_64\bin\ctest.exe'
$windeployqt = Join-Path $QtRoot 'bin\windeployqt.exe'
$runId = [Guid]::NewGuid().ToString('N')
$taskTemp = Join-Path $build 'task-temp'
$npmCache = Join-Path $build 'npm-cache'
$deploy = Join-Path $build "deployment-$Configuration-$runId"
New-Item -ItemType Directory -Force $taskTemp, $npmCache, $deploy | Out-Null
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$env:QTEST_FUNCTION_TIMEOUT = '900000'
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
$previousMsBuildNodeReuse = [Environment]::GetEnvironmentVariable(
    'MSBUILDDISABLENODEREUSE', 'Process')
$env:MSBUILDDISABLENODEREUSE = '1'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

function Remove-VerifiedWorkspaceJunction([string]$Name) {
    $link = Join-Path $repo "tools\node_modules\@q-browser\$Name"
    if (-not (Test-Path -LiteralPath $link)) { return }
    $ownedRoot = [IO.Path]::GetFullPath((Join-Path $repo 'tools'))
    $link = [IO.Path]::GetFullPath($link)
    $ownedPrefix = $ownedRoot.TrimEnd('\') + '\'
    if (-not $link.StartsWith($ownedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Workspace junction escaped its owned root: $link"
    }
    $ownedItem = Get-Item -LiteralPath $ownedRoot -Force
    if (($ownedItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Workspace junction owned root is a reparse point: $ownedRoot"
    }
    $ancestor = Get-Item -LiteralPath (Split-Path -Parent $link) -Force
    while ($null -ne $ancestor -and
           $ancestor.FullName.StartsWith($ownedPrefix,
               [StringComparison]::OrdinalIgnoreCase)) {
        if (($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Workspace junction has a reparse ancestor: $($ancestor.FullName)"
        }
        $ancestor = $ancestor.Parent
    }
    $item = Get-Item -LiteralPath $link -Force
    $expected = [IO.Path]::GetFullPath((Join-Path $repo "tools\$Name"))
    $sentinel = Join-Path $expected 'package.json'
    $targets = @($item.Target | ForEach-Object {
        (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($_)) -ErrorAction Stop).Path
    })
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -or
        $item.LinkType -ne 'Junction' -or $targets.Count -ne 1 -or
        -not $targets[0].Equals($expected, [StringComparison]::OrdinalIgnoreCase) -or
        [QBrowser.Task18.ReparseDirectory]::ReadTag($link) -ne
            [uint32]2684354563) {
        throw "Refusing to remove unexpected npm workspace link: $link"
    }
    $ownedIdentity = [QBrowser.Task18.FileIdentity]::Read($ownedRoot)
    $targetIdentity = [QBrowser.Task18.FileIdentity]::Read($expected)
    $sentinelIdentity = [QBrowser.Task18.FileIdentity]::Read($sentinel)
    $sentinelHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinel).Hash
    [QBrowser.Task18.ReparseDirectory]::RemoveVerifiedMountPoint($link)
    if ((Test-Path -LiteralPath $link -ErrorAction SilentlyContinue) -or
        [QBrowser.Task18.FileIdentity]::Read($ownedRoot) -ne $ownedIdentity -or
        [QBrowser.Task18.FileIdentity]::Read($expected) -ne $targetIdentity -or
        [QBrowser.Task18.FileIdentity]::Read($sentinel) -ne $sentinelIdentity -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sentinel).Hash -ne $sentinelHash) {
        throw "Workspace junction target changed during link-only removal: $link"
    }
}

try {
Invoke-Checked $cmake @(
    '-S', $repo, '-B', $build,
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
    '-DBUILD_TESTING=ON',
    '-DQ_BROWSER_BUILD_WEBENGINE=ON'
)
Invoke-Checked $cmake @('--build', $build, '--config', $Configuration, '--parallel', '2',
    '--', '/nr:false')

$inventory = & $ctest --test-dir $build -C $Configuration -N
if ($LASTEXITCODE -ne 0) { throw 'CTest inventory failed.' }
$inventoryText = $inventory -join "`n"
$requiredTests = @(
    'build_smoke', 'sandbox_launcher', 'pilot_routes', 'quick_design', 'malicious_package',
    'capability_escape', 'worker_api_surface', 'e2e_host_routes',
    'e2e_web_fallback', 'e2e_package_update', 'e2e_host_survives_worker_crash',
    'production_update_runtime'
)
foreach ($required in $requiredTests) {
    if ($inventoryText -notmatch "(?m):\s+$([regex]::Escape($required))\s*$") {
        throw "Required acceptance test is missing: $required"
    }
}
Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration,
    '--output-on-failure', '--no-tests=error')

$requiredExecutables = [ordered]@{
    build_smoke = "tests\$Configuration\q_browser_build_smoke.exe"
    sandbox_launcher = "tests\security\$Configuration\tst_sandbox_launcher.exe"
    pilot_routes = "tests\integration\pilot\$Configuration\tst_pilot_routes.exe"
    quick_design = "tests\quick\$Configuration\qbrowser_quick_tests.exe"
    malicious_package = "tests\security\$Configuration\tst_malicious_package.exe"
    capability_escape = "tests\security\$Configuration\tst_capability_escape.exe"
    worker_api_surface = "tests\security\$Configuration\tst_worker_api_surface.exe"
    e2e_host_routes = "tests\e2e\$Configuration\tst_e2e_host_routes.exe"
    e2e_web_fallback = "tests\e2e\$Configuration\tst_e2e_web_fallback.exe"
    e2e_package_update = "tests\e2e\$Configuration\tst_e2e_package_update.exe"
    e2e_host_survives_worker_crash = "tests\e2e\$Configuration\tst_e2e_host_survives_worker_crash.exe"
    production_update_runtime = "tests\integration\update\$Configuration\tst_production_update_runtime.exe"
}
$requiredResults = Join-Path $build "required-results-$Configuration-$runId"
New-Item -ItemType Directory -Force $requiredResults | Out-Null
foreach ($entry in $requiredExecutables.GetEnumerator()) {
    $executable = Join-Path $build $entry.Value
    if (-not (Test-Path -LiteralPath $executable)) {
        throw "Required acceptance executable is missing: $($entry.Key)"
    }
    $junit = Join-Path $requiredResults "$($entry.Key).xml"
    Invoke-Checked $executable @('-o', "$junit,junitxml")
    [xml]$result = Get-Content -LiteralPath $junit -Raw
    $suite = $result.testsuite
    if ($null -eq $suite -or
        [int]$suite.skipped -ne 0 -or
        [int]$suite.failures -ne 0 -or
        [int]$suite.errors -ne 0) {
        throw "Required acceptance test did not pass without skips: $($entry.Key)"
    }
}

Push-Location (Join-Path $repo 'tools')
try {
    Invoke-Checked 'npm.cmd' @('ci', '--ignore-scripts', '--cache', $npmCache)
    Invoke-Checked 'npm.cmd' @('test', '--ignore-scripts')
}
finally {
    Pop-Location
    Remove-VerifiedWorkspaceJunction 'migrator'
    Remove-VerifiedWorkspaceJunction 'mock-api'
}

$packageExe = Join-Path $build "tools\package-cli\$Configuration\qbrowser-package.exe"
$hostExe = Join-Path $build "apps\host\$Configuration\qbrowser-host.exe"
$workerExe = Join-Path $build "apps\worker\$Configuration\qbrowser-worker.exe"
$privateKey = Join-Path $build 'acceptance-private.pem'
$publicKey = Join-Path $build 'acceptance-public.pem'
$unsigned = Join-Path $build 'pilot-unsigned.qapkg'
$signed = Join-Path $build 'pilot-signed.qapkg'
foreach ($path in @($privateKey, $publicKey, $unsigned, $signed)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
try {
    Invoke-Checked $packageExe @('keygen', '--private-key', $privateKey, '--public-key', $publicKey)
    Invoke-Checked $packageExe @('pack', '--source', (Join-Path $repo 'packages\pilot'), '--output', $unsigned)
    Invoke-Checked $packageExe @('sign', '--package', $unsigned, '--private-key', $privateKey, '--output', $signed)
    $inspection = & $packageExe inspect --package $signed --public-key $publicKey
    if ($LASTEXITCODE -ne 0) { throw 'Signed Pilot inspection failed.' }
    $inspectionObject = $inspection | ConvertFrom-Json
    if (-not $inspectionObject.verified -or $inspectionObject.appId -ne 'com.qbrowser.pilot') {
        throw 'Signed Pilot inspection did not produce the expected verified identity.'
    }
}
finally {
    if (Test-Path -LiteralPath $privateKey) {
        Remove-Item -LiteralPath $privateKey -Force
    }
}
if (Test-Path -LiteralPath $privateKey) {
    throw 'Acceptance private signing key cleanup failed.'
}

Copy-Item -LiteralPath $hostExe, $workerExe, $packageExe, $signed, $publicKey -Destination $deploy -Force
Copy-Item -LiteralPath (Join-Path $OpenSslRoot 'bin\libcrypto-3-x64.dll') -Destination $deploy -Force
Invoke-Checked $windeployqt @('--no-translations', '--qmldir', (Join-Path $repo 'qml'),
    '--dir', $deploy, (Join-Path $deploy 'qbrowser-host.exe'))
Invoke-Checked $windeployqt @('--no-translations', '--qmldir', (Join-Path $repo 'qml'),
    '--dir', $deploy, (Join-Path $deploy 'qbrowser-worker.exe'))

$qtSuffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
$webResourceSuffix = if ($Configuration -eq 'Debug') { '.debug' } else { '' }
$requiredDeployment = @(
    'qbrowser-host.exe', 'qbrowser-worker.exe', 'qbrowser-package.exe',
    'pilot-signed.qapkg', 'acceptance-public.pem', "Qt6Core$qtSuffix.dll",
    "Qt6Quick$qtSuffix.dll", "Qt6WebEngineCore$qtSuffix.dll",
    'libcrypto-3-x64.dll',
    "QtWebEngineProcess$qtSuffix.exe",
    "resources\qtwebengine_resources$webResourceSuffix.pak",
    'resources\icudtl.dat', 'translations\qtwebengine_locales\en-US.pak',
    "platforms\qwindows$qtSuffix.dll"
)
foreach ($relative in $requiredDeployment) {
    if (-not (Test-Path -LiteralPath (Join-Path $deploy $relative))) {
        throw "Deployment verification failed; missing $relative"
    }
}
if (Test-Path -LiteralPath (Join-Path $deploy 'acceptance-private.pem')) {
    throw 'Deployment verification failed; private signing key was deployed.'
}
$privateKeyFiles = @(Get-ChildItem -LiteralPath $build -File -Recurse -Filter '*private*.pem')
if ($privateKeyFiles.Count -ne 0) {
    throw "Acceptance output contains a private key file: $($privateKeyFiles[0].FullName)"
}
foreach ($file in Get-ChildItem -LiteralPath $build -File -Recurse) {
    if ($file.Length -lt 27) { continue }
    $reader = [IO.StreamReader]::new($file.FullName, [Text.Encoding]::ASCII,
        $false, 65536)
    try {
        $buffer = [char[]]::new(65536); $carry = ''; $found = $false
        while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $text = $carry + [string]::new($buffer, 0, $count)
            if ($text -match '(?m)^[ \t]*-----BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----') {
                $found = $true; break
            }
            $carry = if ($text.Length -gt 128) {
                $text.Substring($text.Length - 128)
            } else { $text }
        }
    }
    finally { $reader.Dispose() }
    if ($found) {
        throw "Acceptance output contains PEM private key material: $($file.FullName)"
    }
}

Write-Output "Q-Browser acceptance passed: $Configuration"
Write-Output "Deployment: $deploy"
}
finally {
    [Environment]::SetEnvironmentVariable(
        'MSBUILDDISABLENODEREUSE', $previousMsBuildNodeReuse, 'Process')
}
