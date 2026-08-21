[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [string]$KeyDirectory = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo 'build\release'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repo 'build\release-package'
}
if ([string]::IsNullOrWhiteSpace($KeyDirectory)) {
    $KeyDirectory = Join-Path $repo '.qbrowser-dev\signing'
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$keys = [IO.Path]::GetFullPath($KeyDirectory)

function Assert-ChildPath([string]$Path, [string]$Parent, [string]$Label) {
    $parentPrefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath($Path)
    if (-not $candidate.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must remain below $Parent"
    }
}

Assert-ChildPath $build (Join-Path $repo 'build') 'BuildDirectory'
Assert-ChildPath $output (Join-Path $repo 'build') 'OutputDirectory'
Assert-ChildPath $keys (Join-Path $repo '.qbrowser-dev') 'KeyDirectory'

$packageCli = Join-Path $build "tools\package-cli\$Configuration\qbrowser-package.exe"
if (-not (Test-Path -LiteralPath $packageCli -PathType Leaf)) {
    throw "Package CLI is unavailable: $packageCli"
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

Write-Warning 'DEVELOPMENT ONLY: this command uses a local disposable signing key. Never use this trust root for production or copy its private key into a deployment.'
New-Item -ItemType Directory -Force $keys | Out-Null
$privateKey = Join-Path $keys 'private.pem'
$publicKey = Join-Path $keys 'public.pem'
if ((Test-Path -LiteralPath $privateKey) -xor (Test-Path -LiteralPath $publicKey)) {
    throw 'Development signing key pair is incomplete; remove it manually after reviewing the paths.'
}
if (-not (Test-Path -LiteralPath $privateKey)) {
    Invoke-Checked $packageCli @(
        'keygen', '--private-key', $privateKey, '--public-key', $publicKey)
}

$temporaryRoot = Join-Path (Join-Path $repo 'build') (
    '.task18-package-' + [Guid]::NewGuid().ToString('N'))
Assert-ChildPath $temporaryRoot (Join-Path $repo 'build') 'Temporary package directory'
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    $source = Join-Path $repo 'packages\pilot'
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
    Invoke-Checked $packageCli @(
        'sign', '--package', $unsignedOne, '--private-key', $privateKey,
        '--output', $signedOne)
    Invoke-Checked $packageCli @(
        'sign', '--package', $unsignedTwo, '--private-key', $privateKey,
        '--output', $signedTwo)
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $signedOne).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $signedTwo).Hash) {
        throw 'Deterministic sign verification failed.'
    }
    $inspectionText = & $packageCli inspect --package $signedOne --public-key $publicKey
    if ($LASTEXITCODE -ne 0) { throw 'Signed Pilot inspection failed.' }
    $inspection = $inspectionText | ConvertFrom-Json
    if (-not $inspection.verified -or
        $inspection.appId -ne 'com.qbrowser.pilot' -or
        $inspection.version -ne '1.0.0') {
        throw 'Signed Pilot inspection returned an unexpected identity.'
    }

    $publishedPackage = Join-Path $output 'com.qbrowser.pilot-1.0.0.qapkg'
    $publishedPublicKey = Join-Path $output 'dev-public.pem'
    if (Test-Path -LiteralPath $output) {
        if ($Clean) {
            Remove-Item -LiteralPath $output -Recurse -Force
        }
        elseif ((Test-Path -LiteralPath $publishedPackage -PathType Leaf) -and
                (Test-Path -LiteralPath $publishedPublicKey -PathType Leaf) -and
                (Get-FileHash -Algorithm SHA256 -LiteralPath $publishedPackage).Hash -eq
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $signedOne).Hash -and
                (Get-FileHash -Algorithm SHA256 -LiteralPath $publishedPublicKey).Hash -eq
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $publicKey).Hash -and
                @(Get-ChildItem -LiteralPath $output -File).Count -eq 2) {
            Write-Output "Development package is already current: $publishedPackage"
            return
        }
        else {
            throw "Output exists and differs; rerun with -Clean after validating: $output"
        }
    }
    New-Item -ItemType Directory -Path $output | Out-Null
    Copy-Item -LiteralPath $signedOne -Destination $publishedPackage
    Copy-Item -LiteralPath $publicKey -Destination $publishedPublicKey
    if (Test-Path -LiteralPath (Join-Path $output 'private.pem')) {
        throw 'Private key publication is forbidden.'
    }
    Write-Output "Development package created: $publishedPackage"
    Write-Output "Development public key: $publishedPublicKey"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
