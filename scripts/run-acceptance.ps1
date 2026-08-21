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

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo "build\acceptance-$($Configuration.ToLowerInvariant())"
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
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

Invoke-Checked $cmake @(
    '-S', $repo, '-B', $build,
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
    '-DBUILD_TESTING=ON',
    '-DQ_BROWSER_BUILD_WEBENGINE=ON'
)
Invoke-Checked $cmake @('--build', $build, '--config', $Configuration, '--parallel', '2')

$inventory = & $ctest --test-dir $build -C $Configuration -N
if ($LASTEXITCODE -ne 0) { throw 'CTest inventory failed.' }
$inventoryText = $inventory -join "`n"
$requiredTests = @(
    'build_smoke', 'sandbox_launcher', 'pilot_routes', 'quick_design', 'malicious_package',
    'capability_escape', 'worker_api_surface', 'e2e_host_routes',
    'e2e_web_fallback', 'e2e_package_update', 'e2e_host_survives_worker_crash'
)
foreach ($required in $requiredTests) {
    if ($inventoryText -notmatch "(?m):\s+$([regex]::Escape($required))\s*$") {
        throw "Required acceptance test is missing: $required"
    }
}
Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration,
    '--output-on-failure', '--no-tests=error', '--repeat', 'until-pass:2')

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
Invoke-Checked $packageExe @('keygen', '--private-key', $privateKey, '--public-key', $publicKey)
Invoke-Checked $packageExe @('pack', '--source', (Join-Path $repo 'packages\pilot'), '--output', $unsigned)
Invoke-Checked $packageExe @('sign', '--package', $unsigned, '--private-key', $privateKey, '--output', $signed)
$inspection = & $packageExe inspect --package $signed --public-key $publicKey
if ($LASTEXITCODE -ne 0) { throw 'Signed Pilot inspection failed.' }
$inspectionObject = $inspection | ConvertFrom-Json
if (-not $inspectionObject.verified -or $inspectionObject.appId -ne 'com.qbrowser.pilot') {
    throw 'Signed Pilot inspection did not produce the expected verified identity.'
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

Write-Output "Q-Browser acceptance passed: $Configuration"
Write-Output "Deployment: $deploy"
