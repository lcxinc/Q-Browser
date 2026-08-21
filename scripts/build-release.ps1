[CmdletBinding()]
param(
    [string]$QtRoot = 'E:\DevEnv\qt\6.11.1\msvc2022_64',
    [string]$OpenSslRoot = 'E:\DevEnv\qt\Tools\OpenSSLv3\Win_x64',
    [string]$CMake = 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe',
    [string]$BuildDirectory = '',
    [string]$DeploymentDirectory = '',
    [switch]$Clean,
    [bool]$RunAcceptance = $true
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoBuild = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoBuild 'release'
}
if ([string]::IsNullOrWhiteSpace($DeploymentDirectory)) {
    $DeploymentDirectory = Join-Path $repoBuild 'release-deploy'
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
$deployment = [IO.Path]::GetFullPath($DeploymentDirectory)
$packageOutput = Join-Path $repoBuild 'release-package'

function Assert-ChildPath([string]$Path, [string]$Parent, [string]$Label) {
    $parentPrefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $candidate = [IO.Path]::GetFullPath($Path)
    if (-not $candidate.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must remain below $Parent"
    }
}

Assert-ChildPath $build $repoBuild 'BuildDirectory'
Assert-ChildPath $deployment $repoBuild 'DeploymentDirectory'
if ($build.Equals($deployment, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and DeploymentDirectory must be distinct.'
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

function Protect-DevelopmentTrustKey([string]$Path) {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $Path,
        '/inheritance:r',
        '/grant:r',
        "${currentName}:F",
        '*S-1-5-18:F')
    $effective = Get-Acl -LiteralPath $Path
    $unexpected = @($effective.Access | Where-Object {
        $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
        $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin @(
            $currentUser.Value, $system.Value)
    })
    if (-not $effective.AreAccessRulesProtected -or $unexpected.Count -ne 0) {
        throw "Development public trust key ACL is not restricted: $Path"
    }
}

function Protect-RuntimeDirectory([string]$Path) {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $currentName = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    Invoke-Checked "$env:SystemRoot\System32\icacls.exe" @(
        $Path,
        '/inheritance:r',
        '/grant:r',
        "${currentName}:(OI)(CI)F",
        '*S-1-5-18:(OI)(CI)F')
    $effective = Get-Acl -LiteralPath $Path
    $unexpected = @($effective.Access | Where-Object {
        $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
        $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin @(
            $currentUser.Value, $system.Value)
    })
    if (-not $effective.AreAccessRulesProtected -or $unexpected.Count -ne 0) {
        throw "Runtime directory ACL is not restricted: $Path"
    }
}

$deployScript = Join-Path $repo 'cmake\Deploy.cmake'
$deployedPackage = Join-Path $deployment 'packages\com.qbrowser.pilot-1.0.0.qapkg'
$deployedPublicKey = Join-Path $deployment 'trust\dev-public.pem'
$deployedHost = Join-Path $deployment 'host'
$deployedRuntime = Join-Path $deployment 'runtime'
if ((Test-Path -LiteralPath $deployment) -and -not $Clean) {
    Protect-RuntimeDirectory $deployedHost
    Protect-RuntimeDirectory $deployedRuntime
    Protect-DevelopmentTrustKey $deployedPublicKey
    Invoke-Checked $CMake @(
        '-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment",
        "-DQ_BROWSER_PACKAGE_CLI=$(Join-Path $deployedHost 'qbrowser-package.exe')",
        "-DQ_BROWSER_PUBLIC_KEY=$deployedPublicKey",
        '-P', $deployScript)
    Write-Output "Verified existing deployment without overwriting it: $deployment"
    return
}

if ($Clean) {
    foreach ($cleanTarget in @($build, $deployment, $packageOutput)) {
        Assert-ChildPath $cleanTarget $repoBuild 'Clean target'
        if (Test-Path -LiteralPath $cleanTarget) {
            Remove-Item -LiteralPath $cleanTarget -Recurse -Force
        }
    }
}
elseif (Test-Path -LiteralPath $build) {
    throw "Build output already exists; use -Clean after validating the path: $build"
}

$runId = [Guid]::NewGuid().ToString('N')
$taskTemp = Join-Path $repoBuild ".task18-release-temp-$runId"
$staging = Join-Path $repoBuild ".task18-release-deploy-$runId"
Assert-ChildPath $taskTemp $repoBuild 'Temporary directory'
Assert-ChildPath $staging $repoBuild 'Deployment staging directory'
New-Item -ItemType Directory -Path $taskTemp | Out-Null
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousPath = $env:PATH
$env:TEMP = $taskTemp
$env:TMP = $taskTemp
$env:QTEST_FUNCTION_TIMEOUT = '900000'
try {
    Invoke-Checked $CMake @(
        '-S', $repo, '-B', $build,
        '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        '-DBUILD_TESTING=OFF',
        '-DQ_BROWSER_BUILD_WEBENGINE=ON')
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

    & (Join-Path $repo 'scripts\create-dev-package.ps1') `
        -Configuration Release -BuildDirectory $build `
        -OutputDirectory $packageOutput -Clean:$Clean
    if ($LASTEXITCODE -ne 0) { throw 'Development Pilot package creation failed.' }

    New-Item -ItemType Directory -Path $staging | Out-Null
    $stagingHost = Join-Path $staging 'host'
    $stagingRuntime = Join-Path $staging 'runtime'
    New-Item -ItemType Directory -Path $stagingHost, $stagingRuntime | Out-Null
    Protect-RuntimeDirectory $stagingHost
    Protect-RuntimeDirectory $stagingRuntime
    Invoke-Checked $CMake @(
        '--install', $build, '--config', 'Release',
        '--prefix', $stagingHost,
        '--component', 'Runtime')
    Invoke-Checked $CMake @(
        '-DQ_BROWSER_DEPLOY_MODE=ASSEMBLE',
        "-DQ_BROWSER_DEPLOY_DIR=$staging",
        "-DQ_BROWSER_REPO_ROOT=$repo",
        "-DQ_BROWSER_QT_ROOT=$QtRoot",
        "-DQ_BROWSER_OPENSSL_ROOT=$OpenSslRoot",
        "-DQ_BROWSER_PACKAGE_FILE=$(Join-Path $packageOutput 'com.qbrowser.pilot-1.0.0.qapkg')",
        "-DQ_BROWSER_PUBLIC_KEY=$(Join-Path $packageOutput 'dev-public.pem')",
        '-P', $deployScript)
    Protect-DevelopmentTrustKey (Join-Path $staging 'trust\dev-public.pem')

    $forbiddenSymbols = @(
        'qbrowser_host_testing', 'qbrowser_archive_testing',
        'forceLifecycleQueueFullForTesting', 'retryWorkerCleanupForTesting')
    foreach ($binary in Get-ChildItem -LiteralPath $staging -Recurse -File -Filter '*.exe') {
        foreach ($symbol in $forbiddenSymbols) {
            & "$env:SystemRoot\System32\findstr.exe" /P /M /C:$symbol $binary.FullName | Out-Null
            if ($LASTEXITCODE -eq 0) {
                throw "Production binary contains test-hook surface '$symbol': $($binary.Name)"
            }
        }
    }

    if (Test-Path -LiteralPath $deployment) {
        throw "Deployment appeared during staging; refusing to overwrite: $deployment"
    }
    Move-Item -LiteralPath $staging -Destination $deployment

    $deployedHost = Join-Path $deployment 'host'
    $deployedRuntime = Join-Path $deployment 'runtime'
    $minimalPath = "$deployedHost;$env:SystemRoot\System32;$env:SystemRoot"
    $env:PATH = $minimalPath
    Invoke-Checked $CMake @(
        '-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment",
        "-DQ_BROWSER_PACKAGE_CLI=$(Join-Path $deployedHost 'qbrowser-package.exe')",
        "-DQ_BROWSER_PUBLIC_KEY=$(Join-Path $deployment 'trust\dev-public.pem')",
        '-P', $deployScript)
    $inspect = & (Join-Path $deployedHost 'qbrowser-package.exe') inspect `
        --package $deployedPackage --public-key $deployedPublicKey
    if ($LASTEXITCODE -ne 0 -or -not (($inspect | ConvertFrom-Json).verified)) {
        throw 'Deployment-only package verification failed under the minimal PATH.'
    }
    $env:PATH = "C:\polluted-does-not-exist;$minimalPath"
    Invoke-Checked $CMake @(
        '-DQ_BROWSER_DEPLOY_MODE=VERIFY',
        "-DQ_BROWSER_DEPLOY_DIR=$deployment",
        "-DQ_BROWSER_PACKAGE_CLI=$(Join-Path $deployedHost 'qbrowser-package.exe')",
        "-DQ_BROWSER_PUBLIC_KEY=$(Join-Path $deployment 'trust\dev-public.pem')",
        '-P', $deployScript)

    if ($RunAcceptance) {
        $env:PATH = $previousPath
        & (Join-Path $repo 'scripts\run-acceptance.ps1') `
            -Configuration Release `
            -BuildDirectory (Join-Path $repoBuild 'release-acceptance') `
            -QtRoot $QtRoot -OpenSslRoot $OpenSslRoot
        if ($LASTEXITCODE -ne 0) { throw 'Release acceptance failed.' }
    }

    Write-Output "Q-Browser Release deployment created and verified: $deployment"
}
finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:PATH = $previousPath
    foreach ($temporary in @($taskTemp, $staging)) {
        Assert-ChildPath $temporary $repoBuild 'Temporary cleanup target'
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Recurse -Force
        }
    }
}
