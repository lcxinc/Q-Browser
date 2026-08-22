cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED Q_BROWSER_DEPLOY_MODE)
  set(Q_BROWSER_DEPLOY_MODE VERIFY)
endif()

function(q_browser_deploy_fail message_text)
  message(FATAL_ERROR "Q-Browser deployment verification failed: ${message_text}")
endfunction()

function(q_browser_require_file relative_path)
  if(NOT EXISTS "${Q_BROWSER_DEPLOY_DIR}/${relative_path}"
      OR IS_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}/${relative_path}")
    q_browser_deploy_fail("missing ${relative_path}")
  endif()
endfunction()

function(q_browser_require_glob description pattern)
  file(GLOB matches LIST_DIRECTORIES false "${Q_BROWSER_DEPLOY_DIR}/${pattern}")
  if(NOT matches)
    q_browser_deploy_fail("missing ${description} (${pattern})")
  endif()
endfunction()

function(q_browser_canonical_inventory output_variable)
  file(GLOB_RECURSE inventory_files LIST_DIRECTORIES false
    RELATIVE "${Q_BROWSER_DEPLOY_DIR}" "${Q_BROWSER_DEPLOY_DIR}/*")
  list(REMOVE_ITEM inventory_files "SHA-256SUMS")
  list(SORT inventory_files)
  set(inventory "")
  foreach(relative_path IN LISTS inventory_files)
    string(REPLACE "\\" "/" normalized_path "${relative_path}")
    file(SHA256 "${Q_BROWSER_DEPLOY_DIR}/${relative_path}" digest)
    string(TOLOWER "${digest}" digest)
    string(APPEND inventory "${digest}  ${normalized_path}\n")
  endforeach()
  set(${output_variable} "${inventory}" PARENT_SCOPE)
endfunction()

if(NOT DEFINED Q_BROWSER_DEPLOY_DIR OR Q_BROWSER_DEPLOY_DIR STREQUAL "")
  q_browser_deploy_fail("Q_BROWSER_DEPLOY_DIR is required")
endif()
cmake_path(ABSOLUTE_PATH Q_BROWSER_DEPLOY_DIR NORMALIZE)

if(Q_BROWSER_DEPLOY_MODE STREQUAL "ASSEMBLE")
  foreach(required_variable IN ITEMS Q_BROWSER_REPO_ROOT Q_BROWSER_QT_ROOT
      Q_BROWSER_OPENSSL_ROOT Q_BROWSER_PACKAGE_FILE Q_BROWSER_PUBLIC_KEY)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
      q_browser_deploy_fail("${required_variable} is required for assembly")
    endif()
  endforeach()
  foreach(path_variable IN ITEMS Q_BROWSER_REPO_ROOT Q_BROWSER_QT_ROOT
      Q_BROWSER_OPENSSL_ROOT Q_BROWSER_PACKAGE_FILE Q_BROWSER_PUBLIC_KEY)
    cmake_path(ABSOLUTE_PATH ${path_variable} NORMALIZE)
  endforeach()
  if(NOT IS_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}")
    q_browser_deploy_fail("CMake install directory does not exist: ${Q_BROWSER_DEPLOY_DIR}")
  endif()
  set(host_directory "${Q_BROWSER_DEPLOY_DIR}/host")
  set(runtime_directory "${Q_BROWSER_DEPLOY_DIR}/runtime")
  foreach(installed_executable IN ITEMS qbrowser-host.exe qbrowser-worker.exe
      qbrowser-package.exe)
    q_browser_require_file("host/${installed_executable}")
  endforeach()
  file(MAKE_DIRECTORY "${runtime_directory}")
  file(COPY_FILE "${host_directory}/qbrowser-worker.exe"
    "${runtime_directory}/qbrowser-worker.exe" ONLY_IF_DIFFERENT)

  set(windeployqt "${Q_BROWSER_QT_ROOT}/bin/windeployqt.exe")
  if(NOT EXISTS "${windeployqt}")
    q_browser_deploy_fail("windeployqt is unavailable: ${windeployqt}")
  endif()
  execute_process(COMMAND "${windeployqt}" --help RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output ERROR_VARIABLE help_error)
  if(NOT help_result EQUAL 0)
    q_browser_deploy_fail("windeployqt --help failed: ${help_error}")
  endif()
  set(host_webengine_arguments)
  if(help_output MATCHES "(^|[\r\n ]+)--webengine([\r\n ]+|$)")
    list(APPEND host_webengine_arguments --webengine)
  else()
    list(APPEND host_webengine_arguments
      -webenginecore -webenginequick -webenginewidgets)
  endif()
  execute_process(COMMAND "${windeployqt}" --release --force
    --compiler-runtime --force-openssl --skip-plugin-types qmltooling
    --exclude-plugins qtposition_nmea --translations en
    --qmldir "${Q_BROWSER_REPO_ROOT}/qml" ${host_webengine_arguments}
    --dir "${host_directory}" "${host_directory}/qbrowser-host.exe"
    RESULT_VARIABLE host_deploy_result OUTPUT_VARIABLE host_deploy_output
    ERROR_VARIABLE host_deploy_error)
  if(NOT host_deploy_result EQUAL 0)
    q_browser_deploy_fail("windeployqt host failure: ${host_deploy_output}\n${host_deploy_error}")
  endif()
  # Qt 6.11's windeployqt WebEngine traversal omits this direct import of
  # Qt6WebEngineQuick.dll. Keep the host PE closure complete explicitly.
  foreach(host_qt_runtime IN ITEMS Qt6WebChannelQuick.dll)
    if(NOT EXISTS "${Q_BROWSER_QT_ROOT}/bin/${host_qt_runtime}")
      q_browser_deploy_fail("missing Qt host runtime: ${host_qt_runtime}")
    endif()
    file(COPY_FILE "${Q_BROWSER_QT_ROOT}/bin/${host_qt_runtime}"
      "${host_directory}/${host_qt_runtime}" ONLY_IF_DIFFERENT)
  endforeach()
  execute_process(COMMAND "${windeployqt}" --release --force
    --compiler-runtime --force-openssl --skip-plugin-types qmltooling
    --exclude-plugins qtposition_nmea --translations en
    --qmldir "${Q_BROWSER_REPO_ROOT}/qml"
    --no-webenginecore --no-webenginequick --no-webenginewidgets
    --dir "${runtime_directory}" "${runtime_directory}/qbrowser-worker.exe"
    RESULT_VARIABLE worker_deploy_result OUTPUT_VARIABLE worker_deploy_output
    ERROR_VARIABLE worker_deploy_error)
  if(NOT worker_deploy_result EQUAL 0)
    q_browser_deploy_fail("windeployqt runtime failure: ${worker_deploy_output}\n${worker_deploy_error}")
  endif()
  file(REMOVE "${host_directory}/qbrowser-worker.exe")

  file(GLOB msvc_runtime_files LIST_DIRECTORIES false
    "${host_directory}/vcruntime140*.dll" "${host_directory}/msvcp140*.dll"
    "${host_directory}/concrt140*.dll")
  foreach(msvc_runtime IN LISTS msvc_runtime_files)
    cmake_path(GET msvc_runtime FILENAME runtime_name)
    file(COPY_FILE "${msvc_runtime}" "${runtime_directory}/${runtime_name}"
      ONLY_IF_DIFFERENT)
  endforeach()

  file(MAKE_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}/packages"
    "${Q_BROWSER_DEPLOY_DIR}/trust")
  file(COPY_FILE "${Q_BROWSER_PACKAGE_FILE}"
    "${Q_BROWSER_DEPLOY_DIR}/packages/com.qbrowser.pilot-1.0.0.qapkg"
    ONLY_IF_DIFFERENT)
  file(COPY_FILE "${Q_BROWSER_PUBLIC_KEY}"
    "${Q_BROWSER_DEPLOY_DIR}/trust/dev-public.pem" ONLY_IF_DIFFERENT)
  foreach(openssl_dll IN ITEMS libcrypto-3-x64.dll libssl-3-x64.dll)
    if(NOT EXISTS "${Q_BROWSER_OPENSSL_ROOT}/bin/${openssl_dll}")
      q_browser_deploy_fail("missing OpenSSL runtime: ${openssl_dll}")
    endif()
    foreach(runtime_root IN ITEMS "${host_directory}" "${runtime_directory}")
      file(COPY_FILE "${Q_BROWSER_OPENSSL_ROOT}/bin/${openssl_dll}"
        "${runtime_root}/${openssl_dll}" ONLY_IF_DIFFERENT)
    endforeach()
  endforeach()
  foreach(document IN ITEMS architecture/runtime.md package-spec/qapkg-v1.md
      security/threat-model.md security/windows-sandbox.md
      development/getting-started.md development/migrator.md
      operations/update-rollback.md operations/diagnostics.md)
    cmake_path(GET document PARENT_PATH document_parent)
    file(MAKE_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}/docs/${document_parent}")
    file(COPY_FILE "${Q_BROWSER_REPO_ROOT}/docs/${document}"
      "${Q_BROWSER_DEPLOY_DIR}/docs/${document}" ONLY_IF_DIFFERENT)
  endforeach()
  file(WRITE "${Q_BROWSER_DEPLOY_DIR}/.qbrowser-release-root"
    "Q-BROWSER TASK18 RELEASE v1\n")
  q_browser_canonical_inventory(generated_inventory)
  file(WRITE "${Q_BROWSER_DEPLOY_DIR}/SHA-256SUMS" "${generated_inventory}")
  message(STATUS "Q-Browser deployment assembled: ${Q_BROWSER_DEPLOY_DIR}")
  return()
elseif(Q_BROWSER_DEPLOY_MODE STREQUAL "SEAL")
  q_browser_require_file("release-attestation.json")
  q_browser_canonical_inventory(generated_inventory)
  file(WRITE "${Q_BROWSER_DEPLOY_DIR}/SHA-256SUMS" "${generated_inventory}")
  set(require_attestation TRUE)
elseif(Q_BROWSER_DEPLOY_MODE STREQUAL "VERIFY")
  set(require_attestation TRUE)
elseif(Q_BROWSER_DEPLOY_MODE STREQUAL "PREVERIFY")
  set(require_attestation FALSE)
else()
  q_browser_deploy_fail("unknown mode: ${Q_BROWSER_DEPLOY_MODE}")
endif()

if(NOT IS_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}")
  q_browser_deploy_fail("deployment directory does not exist: ${Q_BROWSER_DEPLOY_DIR}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "Q_BROWSER_DEPLOY_VERIFY_ROOT=${Q_BROWSER_DEPLOY_DIR}"
  "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules"
  "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
  -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command [=[
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($env:Q_BROWSER_DEPLOY_VERIFY_ROOT)
function Assert-Plain([string]$path) {
  $item = Get-Item -LiteralPath $path -Force
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "reparse point rejected: $path"
  }
}
$cursor = Get-Item -LiteralPath $root -Force
while ($null -ne $cursor) { Assert-Plain $cursor.FullName; $cursor = $cursor.Parent }
foreach ($item in Get-ChildItem -LiteralPath $root -Force -Recurse) {
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "reparse point rejected: $($item.FullName)"
  }
}
$trusted = @([Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
  'S-1-5-18', 'S-1-5-32-544')
try {
  $trusted += ([Security.Principal.NTAccount]'NT SERVICE\TrustedInstaller').Translate(
    [Security.Principal.SecurityIdentifier]).Value
} catch {}
$writeMask = [long]([Security.AccessControl.FileSystemRights]::WriteData) -bor
  [long]([Security.AccessControl.FileSystemRights]::AppendData) -bor
  [long]([Security.AccessControl.FileSystemRights]::CreateFiles) -bor
  [long]([Security.AccessControl.FileSystemRights]::CreateDirectories) -bor
  [long]([Security.AccessControl.FileSystemRights]::WriteAttributes) -bor
  [long]([Security.AccessControl.FileSystemRights]::WriteExtendedAttributes) -bor
  [long]([Security.AccessControl.FileSystemRights]::Delete) -bor
  [long]([Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles) -bor
  [long]([Security.AccessControl.FileSystemRights]::ChangePermissions) -bor
  [long]([Security.AccessControl.FileSystemRights]::TakeOwnership)
$replaceMask = [long]([Security.AccessControl.FileSystemRights]::Delete) -bor
  [long]([Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles) -bor
  [long]([Security.AccessControl.FileSystemRights]::ChangePermissions) -bor
  [long]([Security.AccessControl.FileSystemRights]::TakeOwnership)
$cursor = Get-Item -LiteralPath $root -Force
while ($null -ne $cursor) {
  $ancestorAcl = Get-Acl -LiteralPath $cursor.FullName
  $ancestorOwner = ([Security.Principal.NTAccount]$ancestorAcl.Owner).Translate(
    [Security.Principal.SecurityIdentifier]).Value
  if ($ancestorOwner -notin $trusted) {
    throw "untrusted ancestor owner: $($cursor.FullName)"
  }
  foreach ($rule in $ancestorAcl.GetAccessRules($true, $true,
      [Security.Principal.SecurityIdentifier])) {
    if ($rule.AccessControlType -eq
          [Security.AccessControl.AccessControlType]::Allow -and
        $rule.IdentityReference.Value -notin $trusted -and
        ($rule.PropagationFlags -band
          [Security.AccessControl.PropagationFlags]::InheritOnly) -eq 0 -and
        (([long]$rule.FileSystemRights -band $replaceMask) -ne 0)) {
      throw "untrusted ancestor replacement ACE: $($cursor.FullName)"
    }
  }
  $cursor = $cursor.Parent
}
$sensitive = @($root, (Join-Path $root 'host'), (Join-Path $root 'runtime'),
  (Join-Path $root 'packages'), (Join-Path $root 'trust'),
  (Join-Path $root 'packages\com.qbrowser.pilot-1.0.0.qapkg'),
  (Join-Path $root 'trust\dev-public.pem'), (Join-Path $root 'SHA-256SUMS'))
if (Test-Path -LiteralPath (Join-Path $root 'release-attestation.json')) {
  $sensitive += Join-Path $root 'release-attestation.json'
}
foreach ($path in $sensitive) {
  $acl = Get-Acl -LiteralPath $path
  $owner = ([Security.Principal.NTAccount]$acl.Owner).Translate(
    [Security.Principal.SecurityIdentifier]).Value
  if ($owner -notin $trusted -or -not $acl.AreAccessRulesProtected) {
    throw "unprotected owner/DACL: $path"
  }
  foreach ($rule in $acl.GetAccessRules($true, $true,
      [Security.Principal.SecurityIdentifier])) {
    if ($rule.AccessControlType -eq
          [Security.AccessControl.AccessControlType]::Allow -and
        $rule.IdentityReference.Value -notin $trusted -and
        (([long]$rule.FileSystemRights -band $writeMask) -ne 0)) {
      throw "untrusted writable ACE: $path"
    }
  }
}
]=]
  RESULT_VARIABLE security_result OUTPUT_VARIABLE security_output
  ERROR_VARIABLE security_error)
if(NOT security_result EQUAL 0)
  q_browser_deploy_fail("path/ACL boundary rejected: ${security_output}${security_error}")
endif()

foreach(required_file IN ITEMS .qbrowser-release-root host/qbrowser-host.exe
    host/qbrowser-package.exe runtime/qbrowser-worker.exe
    packages/com.qbrowser.pilot-1.0.0.qapkg trust/dev-public.pem
    host/platforms/qwindows.dll host/resources/icudtl.dat
    host/resources/qtwebengine_resources.pak
    host/resources/qtwebengine_resources_100p.pak
    host/resources/qtwebengine_resources_200p.pak
    host/resources/qtwebengine_devtools_resources.pak
    host/resources/v8_context_snapshot.bin
    host/translations/qtwebengine_locales/en-US.pak
    host/qml/QtQuick/qtquick2plugin.dll host/tls/qopensslbackend.dll
    host/tls/qschannelbackend.dll runtime/platforms/qwindows.dll
    runtime/qml/QtQuick/qtquick2plugin.dll runtime/tls/qopensslbackend.dll
    runtime/tls/qschannelbackend.dll docs/architecture/runtime.md
    docs/package-spec/qapkg-v1.md docs/security/threat-model.md
    docs/security/windows-sandbox.md docs/development/getting-started.md
    docs/development/migrator.md docs/operations/update-rollback.md
    docs/operations/diagnostics.md SHA-256SUMS)
  q_browser_require_file("${required_file}")
endforeach()
if(require_attestation)
  q_browser_require_file("release-attestation.json")
  file(READ "${Q_BROWSER_DEPLOY_DIR}/release-attestation.json" attestation)
  set(expected_attestation
    "{\"schema\":1,\"deploymentOnlyE2E\":true,\"routeCount\":10,\"webEngine\":\"deployed\",\"signedUpdate\":\"1.1.0\",\"rollback\":\"1.0.0\"}\n")
  if(NOT attestation STREQUAL expected_attestation)
    q_browser_deploy_fail("release acceptance attestation is missing or invalid")
  endif()
endif()
file(READ "${Q_BROWSER_DEPLOY_DIR}/.qbrowser-release-root" ownership_marker)
if(NOT ownership_marker STREQUAL "Q-BROWSER TASK18 RELEASE v1\n")
  q_browser_deploy_fail("release ownership marker is invalid")
endif()

foreach(runtime_subdirectory IN ITEMS host runtime)
  foreach(qt_module IN ITEMS Core Gui Quick Network)
    q_browser_require_file("${runtime_subdirectory}/Qt6${qt_module}.dll")
  endforeach()
  q_browser_require_glob("OpenSSL Crypto runtime" "${runtime_subdirectory}/libcrypto-3*.dll")
  q_browser_require_glob("OpenSSL TLS runtime" "${runtime_subdirectory}/libssl-3*.dll")
  foreach(msvc_runtime IN ITEMS vcruntime140.dll vcruntime140_1.dll
      msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_atomic_wait.dll
      msvcp140_codecvt_ids.dll concrt140.dll)
    q_browser_require_file("${runtime_subdirectory}/${msvc_runtime}")
  endforeach()
endforeach()
q_browser_require_file("host/Qt6WebEngineCore.dll")
q_browser_require_file("host/Qt6WebChannelQuick.dll")
q_browser_require_file("host/QtWebEngineProcess.exe")
file(GLOB_RECURSE forbidden_worker_webengine LIST_DIRECTORIES false
  "${Q_BROWSER_DEPLOY_DIR}/runtime/*WebEngine*"
  "${Q_BROWSER_DEPLOY_DIR}/runtime/*webengine*")
if(forbidden_worker_webengine)
  q_browser_deploy_fail("Worker closure contains forbidden WebEngine assets")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "Q_BROWSER_DEPLOY_VERIFY_ROOT=${Q_BROWSER_DEPLOY_DIR}"
  "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules"
  "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
  -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command [=[
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($env:Q_BROWSER_DEPLOY_VERIFY_ROOT)

function Get-U16([byte[]]$bytes, [int]$offset) {
  if ($offset -lt 0 -or $offset + 2 -gt $bytes.Length) {
    throw "truncated PE uint16 at $offset"
  }
  return [BitConverter]::ToUInt16($bytes, $offset)
}
function Get-U32([byte[]]$bytes, [int]$offset) {
  if ($offset -lt 0 -or $offset + 4 -gt $bytes.Length) {
    throw "truncated PE uint32 at $offset"
  }
  return [BitConverter]::ToUInt32($bytes, $offset)
}
function Get-AsciiZ([byte[]]$bytes, [int]$offset) {
  if ($offset -lt 0 -or $offset -ge $bytes.Length) {
    throw "invalid PE string offset $offset"
  }
  $end = $offset
  while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { ++$end }
  if ($end -eq $bytes.Length) { throw "unterminated PE import string" }
  return [Text.Encoding]::ASCII.GetString($bytes, $offset, $end - $offset)
}
function Get-PeImports([string]$path) {
  $bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -lt 64 -or (Get-U16 $bytes 0) -ne 0x5a4d) {
    throw "invalid PE image: $path"
  }
  $pe = [int](Get-U32 $bytes 0x3c)
  if ($pe + 24 -gt $bytes.Length -or (Get-U32 $bytes $pe) -ne 0x00004550) {
    throw "invalid PE signature: $path"
  }
  $sectionCount = [int](Get-U16 $bytes ($pe + 6))
  $optionalSize = [int](Get-U16 $bytes ($pe + 20))
  $optional = $pe + 24
  $magic = Get-U16 $bytes $optional
  if ($magic -eq 0x20b) { $directory = $optional + 112 }
  elseif ($magic -eq 0x10b) { $directory = $optional + 96 }
  else { throw "unsupported PE optional header: $path" }
  if ($optional + $optionalSize -gt $bytes.Length -or
      $directory + (14 * 8) -gt $optional + $optionalSize) {
    throw "truncated PE optional header: $path"
  }
  $sections = @()
  $sectionTable = $optional + $optionalSize
  for ($index = 0; $index -lt $sectionCount; ++$index) {
    $entry = $sectionTable + ($index * 40)
    $sections += [pscustomobject]@{
      VirtualSize = [uint32](Get-U32 $bytes ($entry + 8))
      VirtualAddress = [uint32](Get-U32 $bytes ($entry + 12))
      RawSize = [uint32](Get-U32 $bytes ($entry + 16))
      RawAddress = [uint32](Get-U32 $bytes ($entry + 20))
    }
  }
  $toOffset = {
    param([uint32]$rva)
    foreach ($section in $sections) {
      $span = [Math]::Max([uint64]$section.VirtualSize,
        [uint64]$section.RawSize)
      if ([uint64]$rva -ge [uint64]$section.VirtualAddress -and
          [uint64]$rva -lt [uint64]$section.VirtualAddress + $span) {
        $raw = [uint64]$section.RawAddress +
          ([uint64]$rva - [uint64]$section.VirtualAddress)
        if ($raw -ge [uint64]$bytes.Length) {
          throw "PE RVA maps outside image: $path"
        }
        return [int]$raw
      }
    }
    if ([uint64]$rva -lt [uint64]$optional) { return [int]$rva }
    throw "unmapped PE RVA $rva in $path"
  }
  $imports = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
  $importRva = [uint32](Get-U32 $bytes ($directory + 8))
  if ($importRva -ne 0) {
    $descriptor = & $toOffset $importRva
    while ($true) {
      $originalThunk = Get-U32 $bytes $descriptor
      $nameRva = [uint32](Get-U32 $bytes ($descriptor + 12))
      $firstThunk = Get-U32 $bytes ($descriptor + 16)
      if ($originalThunk -eq 0 -and $nameRva -eq 0 -and $firstThunk -eq 0) {
        break
      }
      if ($nameRva -eq 0) { throw "PE import without a name: $path" }
      [void]$imports.Add((Get-AsciiZ $bytes (& $toOffset $nameRva)))
      $descriptor += 20
    }
  }
  $delayRva = [uint32](Get-U32 $bytes ($directory + (13 * 8)))
  if ($delayRva -ne 0) {
    $descriptor = & $toOffset $delayRva
    while ($true) {
      $attributes = Get-U32 $bytes $descriptor
      $nameRva = [uint32](Get-U32 $bytes ($descriptor + 4))
      $iat = Get-U32 $bytes ($descriptor + 12)
      if ($attributes -eq 0 -and $nameRva -eq 0 -and $iat -eq 0) { break }
      if (($attributes -band 1) -eq 0) {
        throw "non-RVA delay import is unsupported: $path"
      }
      if ($nameRva -eq 0) { throw "PE delay import without a name: $path" }
      [void]$imports.Add((Get-AsciiZ $bytes (& $toOffset $nameRva)))
      $descriptor += 32
    }
  }
  return @($imports)
}

$missing = [Collections.Generic.List[string]]::new()
$osDlls = [Collections.Generic.HashSet[string]]::new(
  [StringComparer]::OrdinalIgnoreCase)
foreach ($name in @('advapi32.dll','authz.dll','bcrypt.dll','bthprops.cpl',
  'cfgmgr32.dll','comctl32.dll','comdlg32.dll','crypt32.dll','cryptbase.dll',
  'd3d9.dll','d3d11.dll','d3d12.dll','dcomp.dll','dbghelp.dll','dhcpcsvc.dll',
  'dnsapi.dll','dwrite.dll','dwmapi.dll','dxgi.dll','fontsub.dll','gdi32.dll',
  'hid.dll','icuuc.dll','imagehlp.dll','imm32.dll','iphlpapi.dll',
  'kernel32.dll','mpr.dll','msasn1.dll','ncrypt.dll','netapi32.dll','normaliz.dll',
  'ntdll.dll','ole32.dll','oleaut32.dll','pdh.dll','powrprof.dll','propsys.dll','psapi.dll',
  'rpcrt4.dll','secur32.dll','setupapi.dll','shell32.dll','shlwapi.dll',
  'mf.dll','mfplat.dll','mfreadwrite.dll','mmdevapi.dll','msvcrt.dll','urlmon.dll',
  'user32.dll','userenv.dll','uiautomationcore.dll','uxtheme.dll','version.dll',
  'winhttp.dll','winmm.dll','winspool.drv','wintrust.dll','winusb.dll','wlanapi.dll',
  'ws2_32.dll','wtsapi32.dll')) {
  [void]$osDlls.Add($name)
}
foreach ($closureName in @('host', 'runtime')) {
  $closureRoot = Join-Path $root $closureName
  $images = @(Get-ChildItem -LiteralPath $closureRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') })
  foreach ($image in $images) {
    foreach ($import in @(Get-PeImports $image.FullName)) {
      $besideImporter = Join-Path $image.DirectoryName $import
      $closureDirect = Join-Path $closureRoot $import
      if ((Test-Path -LiteralPath $besideImporter -PathType Leaf) -or
          (Test-Path -LiteralPath $closureDirect -PathType Leaf)) { continue }
      if ($import.StartsWith('api-ms-win-', [StringComparison]::OrdinalIgnoreCase) -or
          $import.StartsWith('ext-ms-win-', [StringComparison]::OrdinalIgnoreCase) -or
          $osDlls.Contains($import)) {
        continue
      }
      $missing.Add("$closureName/$($image.Name) -> $import")
    }
  }
}
if ($missing.Count -ne 0) {
  throw "missing PE dependencies: $($missing -join '; ')"
}
]=]
  RESULT_VARIABLE pe_result OUTPUT_VARIABLE pe_output ERROR_VARIABLE pe_error)
if(NOT pe_result EQUAL 0)
  q_browser_deploy_fail("PE dependency closure rejected: ${pe_output}${pe_error}")
endif()

file(GLOB_RECURSE deployed_files LIST_DIRECTORIES false
  RELATIVE "${Q_BROWSER_DEPLOY_DIR}" "${Q_BROWSER_DEPLOY_DIR}/*")
foreach(relative_path IN LISTS deployed_files)
  string(REPLACE "\\" "/" normalized_path "${relative_path}")
  string(TOLOWER "${normalized_path}" lower_path)
  if(lower_path MATCHES "(^|/)(tests?|fixtures?)(/|$)"
      OR lower_path MATCHES "(^|/)(tst_[^/]*|q_browser_build_smoke)\\.exe$"
      OR lower_path MATCHES "(^|/).*(private|secret).*\\.(pem|key)$"
      OR lower_path MATCHES "\\.(cpp|cxx|cc|h|hpp|pdb|ilk|obj|lib|exp)$")
    q_browser_deploy_fail("forbidden test, source, symbol, or private-key asset: ${normalized_path}")
  endif()
  file(SIZE "${Q_BROWSER_DEPLOY_DIR}/${relative_path}" deployed_size)
  if(deployed_size GREATER 536870912)
    q_browser_deploy_fail("deployment file exceeds 512 MiB scan policy: ${normalized_path}")
  endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "Q_BROWSER_DEPLOY_VERIFY_ROOT=${Q_BROWSER_DEPLOY_DIR}"
  "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
  -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command [=[
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($env:Q_BROWSER_DEPLOY_VERIFY_ROOT)
$pattern = [regex]::new('(?m)^[ \t]*-----BEGIN ([A-Z0-9 ]+)-----',
  [Text.RegularExpressions.RegexOptions]::CultureInvariant)
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Force -File) {
  $reader = [IO.StreamReader]::new($file.FullName, [Text.Encoding]::ASCII,
    $false, 65536)
  try {
    $buffer = [char[]]::new(65536)
    $carry = ''
    while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
      $text = $carry + [string]::new($buffer, 0, $count)
      foreach ($match in $pattern.Matches($text)) {
        $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
        if ($relative -ne 'trust/dev-public.pem' -or
            $match.Groups[1].Value -ne 'PUBLIC KEY') {
          throw "forbidden PEM material found in $relative"
        }
      }
      $carry = if ($text.Length -gt 128) { $text.Substring($text.Length - 128) }
        else { $text }
    }
  }
  finally { $reader.Dispose() }
}
]=]
  RESULT_VARIABLE pem_result OUTPUT_VARIABLE pem_output ERROR_VARIABLE pem_error)
if(NOT pem_result EQUAL 0)
  q_browser_deploy_fail("complete PEM scan rejected: ${pem_output}${pem_error}")
endif()

file(READ "${Q_BROWSER_DEPLOY_DIR}/SHA-256SUMS" recorded_inventory)
q_browser_canonical_inventory(expected_inventory)
if(NOT recorded_inventory STREQUAL expected_inventory)
  q_browser_deploy_fail("SHA-256SUMS does not match the canonical sorted inventory")
endif()
execute_process(COMMAND "${Q_BROWSER_DEPLOY_DIR}/host/qbrowser-package.exe" inspect
  --package "${Q_BROWSER_DEPLOY_DIR}/packages/com.qbrowser.pilot-1.0.0.qapkg"
  --public-key "${Q_BROWSER_DEPLOY_DIR}/trust/dev-public.pem"
  RESULT_VARIABLE inspect_result OUTPUT_VARIABLE inspect_output
  ERROR_VARIABLE inspect_error)
if(NOT inspect_result EQUAL 0 OR NOT inspect_output MATCHES "\"verified\":true"
    OR NOT inspect_output MATCHES "\"appId\":\"com.qbrowser.pilot\""
    OR NOT inspect_output MATCHES "\"version\":\"1.0.0\"")
  q_browser_deploy_fail("Pilot signature/identity inspection failed: ${inspect_error}")
endif()
message(STATUS "Q-Browser deployment verified: ${Q_BROWSER_DEPLOY_DIR}")
