[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [string]$KeyDirectory = '',
    [string]$TrustedRoot = '',
    [string]$SourceDirectory = '',
    [switch]$ParentTrustedRootLeaseHeld,
    [switch]$Clean
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
  public sealed class DirectoryLease : IDisposable {
    readonly SafeFileHandle handle;
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
      IntPtr security, uint creation, uint flags, IntPtr template);
    public DirectoryLease(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      // Hold DELETE access without FILE_SHARE_DELETE so the directory cannot be
      // renamed or replaced while security-sensitive files are in use.
      handle = CreateFileW(native, 0x00010000, 3, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
      if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), full);
    }
    public static int ProbeDenyDeleteLease(string path) {
      string full = System.IO.Path.GetFullPath(path);
      string native = full.StartsWith(@"\\") ? @"\\?\UNC\" + full.Substring(2)
                                                : @"\\?\" + full;
      using (var probe = CreateFileW(native, 0x00010000, 3, IntPtr.Zero, 3,
                                     0x02000000, IntPtr.Zero)) {
        return probe.IsInvalid ? Marshal.GetLastWin32Error() : 0;
      }
    }
    public void Dispose() { handle.Dispose(); }
  }
}
'@
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRoot = if ([string]::IsNullOrWhiteSpace($SourceDirectory)) { $repo }
    else { [IO.Path]::GetFullPath($SourceDirectory) }
$repoBuild = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
$localAppData = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData)
if ([string]::IsNullOrWhiteSpace($TrustedRoot)) {
    $TrustedRoot = Join-Path $localAppData 'QBrowserTask18'
}
$trusted = [IO.Path]::GetFullPath($TrustedRoot)
$trustedBuildRoot = Join-Path $trusted 'build'
$trustedWorkRoot = Join-Path $trusted 'work'
$defaultOutput = [IO.Path]::GetFullPath((Join-Path $trusted 'release-package'))
$defaultKeys = [IO.Path]::GetFullPath((Join-Path $trusted 'signing'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $trustedBuildRoot 'release'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = $defaultOutput }
if ([string]::IsNullOrWhiteSpace($KeyDirectory)) { $KeyDirectory = $defaultKeys }
$build = [IO.Path]::GetFullPath($BuildDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$keys = [IO.Path]::GetFullPath($KeyDirectory)
$ownedMarkerName = '.qbrowser-task18-owned'
$ownedMarkerText = "Q-BROWSER TASK18 OWNED v1`n"

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

function Remove-OwnedTree([string]$Path, [string]$ExactAllowedPath) {
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
    $marker = Join-Path $full $ownedMarkerName
    $actualMarker = if (Test-Path -LiteralPath $marker -PathType Leaf) {
        (Get-Content -LiteralPath $marker -Raw) -replace "`r`n", "`n"
    } else { '' }
    if ($actualMarker -ne ($ownedMarkerText -replace "`r`n", "`n")) {
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
        if (($current.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
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
        Remove-Item -LiteralPath $file.FullName -Force
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
        Remove-Item -LiteralPath $directory.FullName -Force
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
        [Security.Principal.SecurityIdentifier]
    ).Value
    if (-not $verified.AreAccessRulesProtected -or
        $ownerSid -ne $current.Value -or
        @($verified.GetAccessRules($true, $true,
            [Security.Principal.SecurityIdentifier]) | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -notin $allowed }).Count -ne 0) {
        throw "ACL sanitization failed: $Path"
    }
}

function Get-TrustedSidValues {
    $values = @(
        [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
        'S-1-5-18', 'S-1-5-32-544')
    try {
        $values += ([Security.Principal.NTAccount]'NT SERVICE\TrustedInstaller').Translate(
            [Security.Principal.SecurityIdentifier]).Value
    } catch {}
    return @($values | Sort-Object -Unique)
}

function Assert-TrustedAncestorChain([string]$Path, [string]$ManagedRoot = '') {
    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not (Test-Path -LiteralPath $candidate)) {
        $parent = [IO.Directory]::GetParent($candidate)
        if ($null -eq $parent) { throw "No existing trusted ancestor for $Path" }
        $candidate = $parent.FullName
    }
    $managed = if ([string]::IsNullOrWhiteSpace($ManagedRoot)) { '' }
        else { [IO.Path]::GetFullPath($ManagedRoot).TrimEnd('\') }
    $trustedSids = @(Get-TrustedSidValues)
    $replaceMask = [long]([Security.AccessControl.FileSystemRights]::Delete) -bor
        [long]([Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles) -bor
        [long]([Security.AccessControl.FileSystemRights]::ChangePermissions) -bor
        [long]([Security.AccessControl.FileSystemRights]::TakeOwnership)
    $writeMask = $replaceMask -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteData) -bor
        [long]([Security.AccessControl.FileSystemRights]::AppendData) -bor
        [long]([Security.AccessControl.FileSystemRights]::CreateFiles) -bor
        [long]([Security.AccessControl.FileSystemRights]::CreateDirectories) -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteAttributes) -bor
        [long]([Security.AccessControl.FileSystemRights]::WriteExtendedAttributes)
    $item = Get-Item -LiteralPath $candidate -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Trusted ancestor is a reparse point: $($item.FullName)"
        }
        $acl = Get-Acl -LiteralPath $item.FullName
        $owner = ([Security.Principal.NTAccount]$acl.Owner).Translate(
            [Security.Principal.SecurityIdentifier]).Value
        if ($owner -notin $trustedSids) {
            throw "Trusted ancestor has an untrusted owner: $($item.FullName)"
        }
        $insideManaged = -not [string]::IsNullOrEmpty($managed) -and
            ($item.FullName.TrimEnd('\').Equals($managed,
                [StringComparison]::OrdinalIgnoreCase) -or
             $item.FullName.StartsWith($managed + '\',
                [StringComparison]::OrdinalIgnoreCase))
        if ($insideManaged -and -not $acl.AreAccessRulesProtected) {
            throw "Managed ancestor DACL is not protected: $($item.FullName)"
        }
        foreach ($rule in $acl.GetAccessRules($true, $true,
                [Security.Principal.SecurityIdentifier])) {
            if ($rule.AccessControlType -ne 'Allow' -or
                $rule.IdentityReference.Value -in $trustedSids -or
                ($rule.PropagationFlags -band
                    [Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) {
                continue
            }
            $mask = if ($insideManaged) { $writeMask } else { $replaceMask }
            if (([long]$rule.FileSystemRights -band $mask) -ne 0) {
                throw "Untrusted ancestor replacement/write ACE: $($item.FullName)"
            }
        }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}

function Assert-StableTrustedPath([string]$Path, [string]$Identity) {
    Assert-TrustedAncestorChain $Path $trusted
    if ((Get-PathIdentity $Path) -ne $Identity) {
        throw "Trusted path identity changed: $Path"
    }
}

function Add-DirectoryLease([string]$Path) {
    $lease = [QBrowser.Task18.DirectoryLease]::new($Path)
    [void]$script:leases.Add($lease)
    return $lease
}

function Assert-PlainTree([string]$Path) {
    Assert-NoReparseAncestor $Path
    foreach ($entry in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse member is forbidden: $($entry.FullName)"
        }
    }
}

$script:leases = [Collections.Generic.List[IDisposable]]::new()
Assert-TrustedAncestorChain $localAppData
Assert-NoReparseAncestor $trusted
New-Item -ItemType Directory -Path $trusted -Force | Out-Null
foreach ($managedParent in @($trustedBuildRoot, $trustedWorkRoot)) {
    New-Item -ItemType Directory -Path $managedParent -Force | Out-Null
}
$managedLeaseRoots = @($trusted, $trustedBuildRoot, $trustedWorkRoot)
if ($ParentTrustedRootLeaseHeld) {
    # A parent release operation already owns these roots. Probe its
    # deny-delete leases before any ACL mutation, then use the established
    # protected state read-only.
    foreach ($managedRoot in $managedLeaseRoots) {
        if ([QBrowser.Task18.DirectoryLease]::ProbeDenyDeleteLease($managedRoot) -ne 32) {
            throw "Parent deny-delete lease is absent for $managedRoot."
        }
        Assert-ProtectedPath $managedRoot -Container
    }
    Write-Output 'PARENT_TRUSTED_ROOT_LEASE=PASS roots=3 sharingViolation=32'
}
else {
    foreach ($managedRoot in $managedLeaseRoots) {
        [void](Add-DirectoryLease $managedRoot)
        Protect-Path $managedRoot -Container
    }
}
Assert-TrustedAncestorChain $trusted $trusted

Assert-ChildPath $build $trusted 'BuildDirectory'
Assert-ChildPath $output $trusted 'OutputDirectory'
Assert-ChildPath $keys $trusted 'KeyDirectory'
Assert-NoReparseAncestor $build
Assert-NoReparseAncestor $output
Assert-NoReparseAncestor $keys
if ($Clean -and -not $output.Equals($defaultOutput,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "-Clean is restricted to the exact Task18 output: $defaultOutput"
}

$packageCli = Join-Path $build "tools\package-cli\$Configuration\qbrowser-package.exe"
if (-not (Test-Path -LiteralPath $packageCli -PathType Leaf)) {
    throw "Package CLI is unavailable: $packageCli"
}

Write-Warning "DEVELOPMENT ONLY: this command uses local signing authority below $trusted. Never use it for production or copy private signing material into deployment output."
New-Item -ItemType Directory -Force $keys | Out-Null
Protect-Path $keys -Container
Assert-PlainTree $keys
$keyIdentity = Get-PathIdentity $keys
$keyLease = $null
Assert-StableTrustedPath $keys $keyIdentity
$privateKey = Join-Path $keys 'private.pem'
$publicKey = Join-Path $keys 'public.pem'
if ((Test-Path -LiteralPath $privateKey) -xor (Test-Path -LiteralPath $publicKey)) {
    throw 'Development signing key pair is incomplete; inspect the protected directory manually.'
}
if (-not (Test-Path -LiteralPath $privateKey)) {
    Invoke-Checked $packageCli @(
        'keygen', '--private-key', $privateKey, '--public-key', $publicKey)
}
Protect-Path $privateKey
Protect-Path $publicKey
Assert-PlainTree $keys
$keyLease = Add-DirectoryLease $keys

$temporaryRoot = Join-Path $trustedWorkRoot ('.task18-package-' + [Guid]::NewGuid().ToString('N'))
New-OwnedDirectory $temporaryRoot
$publishStage = $null
Protect-Path $temporaryRoot -Container
$temporaryLease = $null
$temporaryIdentity = Get-PathIdentity $temporaryRoot
$buildRootIdentity = Get-PathIdentity $trustedWorkRoot
try {
    $sourceInput = Join-Path $sourceRoot 'packages\pilot'
    Assert-PlainTree $sourceInput
    $source = Join-Path $temporaryRoot 'pilot-source'
    New-Item -ItemType Directory -Path $source | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $sourceInput -Force -Recurse) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Pilot source contains a reparse point: $($entry.FullName)"
        }
        $relative = $entry.FullName.Substring($sourceInput.Length).TrimStart('\')
        $target = Join-Path $source $relative
        if ($entry.PSIsContainer) { [void][IO.Directory]::CreateDirectory($target) }
        else {
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
            Copy-Item -LiteralPath $entry.FullName -Destination $target
        }
    }
    Assert-StableTrustedPath $temporaryRoot $temporaryIdentity
    $unsignedOne = Join-Path $temporaryRoot 'pilot-1.unsigned.qapkg'
    $unsignedTwo = Join-Path $temporaryRoot 'pilot-2.unsigned.qapkg'
    $signedOne = Join-Path $temporaryRoot 'com.qbrowser.pilot-1.0.0.qapkg'
    $signedTwo = Join-Path $temporaryRoot 'pilot-2.signed.qapkg'
    Invoke-Checked $packageCli @('pack', '--source', $source, '--output', $unsignedOne)
    Invoke-Checked $packageCli @('pack', '--source', $source, '--output', $unsignedTwo)
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $unsignedOne).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $unsignedTwo).Hash) {
        throw 'Deterministic pack verification failed.'
    }
    Assert-StableTrustedPath $keys $keyIdentity
    $keyRenameProbe = Join-Path $trusted 'signing-substitute-probe'
    $keyRenameRejected = $false
    try { Move-Item -LiteralPath $keys -Destination $keyRenameProbe -ErrorAction Stop }
    catch { $keyRenameRejected = $true }
    if (-not $keyRenameRejected -or (Get-PathIdentity $keys) -ne $keyIdentity) {
        throw 'Protected signing authority accepted rename/replacement.'
    }
    # qbrowser-package opens the exact signing root with its own stable,
    # deny-delete directory handle. Release our equivalent handle so the two
    # exclusive mutation locks do not conflict; validate identity on both sides.
    $keyLease.Dispose()
    $keyLease = $null
    Assert-StableTrustedPath $keys $keyIdentity
    Invoke-Checked $packageCli @('sign', '--package', $unsignedOne,
        '--private-key', $privateKey, '--output', $signedOne)
    Assert-StableTrustedPath $keys $keyIdentity
    Invoke-Checked $packageCli @('sign', '--package', $unsignedTwo,
        '--private-key', $privateKey, '--output', $signedTwo)
    Assert-StableTrustedPath $keys $keyIdentity
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $signedOne).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $signedTwo).Hash) {
        throw 'Deterministic sign verification failed.'
    }
    $inspectionText = & $packageCli inspect --package $signedOne --public-key $publicKey
    if ($LASTEXITCODE -ne 0) { throw 'Signed Pilot inspection failed.' }
    $inspection = $inspectionText | ConvertFrom-Json
    if (-not $inspection.verified -or $inspection.appId -ne 'com.qbrowser.pilot' -or
        $inspection.version -ne '1.0.0') {
        throw 'Signed Pilot inspection returned an unexpected identity.'
    }
    $keyLease = Add-DirectoryLease $keys
    Assert-StableTrustedPath $keys $keyIdentity

    $publishedPackage = Join-Path $output 'com.qbrowser.pilot-1.0.0.qapkg'
    $publishedPublicKey = Join-Path $output 'dev-public.pem'
    if (Test-Path -LiteralPath $output) {
        if ($Clean) {
            Remove-OwnedTree $output $defaultOutput
        }
        elseif ((Test-Path -LiteralPath $publishedPackage -PathType Leaf) -and
                (Test-Path -LiteralPath $publishedPublicKey -PathType Leaf) -and
                (Test-Path -LiteralPath (Join-Path $output $ownedMarkerName) -PathType Leaf) -and
                (Get-FileHash -Algorithm SHA256 -LiteralPath $publishedPackage).Hash -eq
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $signedOne).Hash -and
                (Get-FileHash -Algorithm SHA256 -LiteralPath $publishedPublicKey).Hash -eq
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $publicKey).Hash -and
                @(Get-ChildItem -LiteralPath $output -File -Force).Count -eq 3) {
            Write-Output "Development package is already current: $publishedPackage"
            return
        }
        else { throw "Output exists and differs; inspect it before using -Clean: $output" }
    }
    $publishStage = Join-Path $trustedWorkRoot ('.task18-package-publish-' + [Guid]::NewGuid().ToString('N'))
    New-OwnedDirectory $publishStage
    Protect-Path $publishStage -Container
    $publishIdentity = Get-PathIdentity $publishStage
    $publishLease = Add-DirectoryLease $publishStage
    Copy-Item -LiteralPath $signedOne -Destination (Join-Path $publishStage (Split-Path $publishedPackage -Leaf))
    Copy-Item -LiteralPath $publicKey -Destination (Join-Path $publishStage (Split-Path $publishedPublicKey -Leaf))
    Protect-Path (Join-Path $publishStage (Split-Path $publishedPackage -Leaf))
    Protect-Path (Join-Path $publishStage (Split-Path $publishedPublicKey -Leaf))
    if ((Get-PathIdentity $trustedWorkRoot) -ne $buildRootIdentity) {
        throw 'Trusted work root identity changed before package publication.'
    }
    Assert-StableTrustedPath $keys $keyIdentity
    Assert-StableTrustedPath $temporaryRoot $temporaryIdentity
    Assert-StableTrustedPath $publishStage $publishIdentity
    Assert-NoReparseAncestor $output
    if (Test-Path -LiteralPath $output) {
        throw "Package output appeared during staging; refusing to overwrite: $output"
    }
    $publishLease.Dispose()
    Move-Item -LiteralPath $publishStage -Destination $output
    $publishStage = $null
    $outputIdentity = Get-PathIdentity $output
    $outputLease = Add-DirectoryLease $output
    Assert-StableTrustedPath $output $outputIdentity
    $outputRenameProbe = Join-Path $trusted 'release-package-substitute-probe'
    $outputRenameRejected = $false
    try { Move-Item -LiteralPath $output -Destination $outputRenameProbe -ErrorAction Stop }
    catch { $outputRenameRejected = $true }
    if (-not $outputRenameRejected -or (Get-PathIdentity $output) -ne $outputIdentity) {
        throw 'Published development package accepted rename/replacement.'
    }
    Write-Output 'SIGNING_PARENT_REPLACEMENT_DEFENSE=PASS key=stable publish=stable'
    Write-Output "Development package created: $publishedPackage"
    Write-Output "Development public key: $publishedPublicKey"
}
finally {
    foreach ($lease in $script:leases) { $lease.Dispose() }
    $script:leases.Clear()
    if ($null -ne $publishStage -and (Test-Path -LiteralPath $publishStage)) {
        Remove-OwnedTree $publishStage $publishStage
    }
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-OwnedTree $temporaryRoot $temporaryRoot
    }
}
