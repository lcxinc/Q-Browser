param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("file", "directory")]
    [string] $Mode,

    [Parameter(Mandatory = $true)]
    [string] $Path
)

$ErrorActionPreference = "Stop"
$lockStream = $null

function Convert-ToExtendedPath([string] $Value) {
    $fullPath = [System.IO.Path]::GetFullPath($Value)
    if ($fullPath.StartsWith("\\?\", [System.StringComparison]::Ordinal)) {
        return $fullPath
    }
    if ($fullPath.StartsWith("\\", [System.StringComparison]::Ordinal)) {
        return "\\?\UNC\" + $fullPath.Substring(2)
    }
    return "\\?\" + $fullPath
}

try {
    if ($Mode -eq "file") {
        # FileShare.Read permits the migrator's two read handles but denies all
        # writers and delete/rename operations for this helper's lifetime.
        $nativePath = Convert-ToExtendedPath $Path
        $lockStream = New-Object System.IO.FileStream(
            $nativePath,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read
        )
    }
    else {
        # CreateProcess establishes cwd before the script starts. Windows keeps
        # that directory open without delete sharing, so this process is the
        # non-modifying directory lease. Validate that cwd is the requested path.
        $expected = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
        $actual = [System.IO.Path]::GetFullPath((Get-Location).ProviderPath).TrimEnd('\')
        if (-not [System.String]::Equals($expected, $actual, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "directory cwd mismatch"
        }
    }

    [Console]::Out.WriteLine("READY")
    [Console]::Out.Flush()
    $command = [Console]::In.ReadLine()
    if ($command -ne "DONE") {
        throw "invalid lock protocol"
    }
    [Console]::Out.WriteLine("DONE")
    [Console]::Out.Flush()
}
catch {
    [Console]::Error.WriteLine("LOCK_ERROR")
    exit 73
}
finally {
    if ($null -ne $lockStream) {
        $lockStream.Dispose()
    }
}
