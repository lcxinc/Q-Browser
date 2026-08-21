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
  file(GLOB_RECURSE inventory_files
    LIST_DIRECTORIES false
    RELATIVE "${Q_BROWSER_DEPLOY_DIR}"
    "${Q_BROWSER_DEPLOY_DIR}/*")
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
  foreach(required_variable IN ITEMS
      Q_BROWSER_REPO_ROOT
      Q_BROWSER_QT_ROOT
      Q_BROWSER_OPENSSL_ROOT
      Q_BROWSER_PACKAGE_FILE
      Q_BROWSER_PUBLIC_KEY)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
      q_browser_deploy_fail("${required_variable} is required for assembly")
    endif()
  endforeach()
  foreach(path_variable IN ITEMS
      Q_BROWSER_REPO_ROOT
      Q_BROWSER_QT_ROOT
      Q_BROWSER_OPENSSL_ROOT
      Q_BROWSER_PACKAGE_FILE
      Q_BROWSER_PUBLIC_KEY)
    cmake_path(ABSOLUTE_PATH ${path_variable} NORMALIZE)
  endforeach()
  if(NOT IS_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}")
    q_browser_deploy_fail("CMake install directory does not exist: ${Q_BROWSER_DEPLOY_DIR}")
  endif()
  set(host_directory "${Q_BROWSER_DEPLOY_DIR}/host")
  set(runtime_directory "${Q_BROWSER_DEPLOY_DIR}/runtime")
  foreach(installed_executable IN ITEMS
      qbrowser-host.exe qbrowser-worker.exe qbrowser-package.exe)
    q_browser_require_file("host/${installed_executable}")
  endforeach()
  file(MAKE_DIRECTORY "${runtime_directory}")
  file(COPY_FILE "${host_directory}/qbrowser-worker.exe"
    "${runtime_directory}/qbrowser-worker.exe" ONLY_IF_DIFFERENT)

  set(windeployqt "${Q_BROWSER_QT_ROOT}/bin/windeployqt.exe")
  if(NOT EXISTS "${windeployqt}")
    q_browser_deploy_fail("windeployqt is unavailable: ${windeployqt}")
  endif()
  execute_process(
    COMMAND "${windeployqt}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error)
  if(NOT help_result EQUAL 0)
    q_browser_deploy_fail("windeployqt --help failed: ${help_error}")
  endif()
  set(webengine_arguments)
  if(help_output MATCHES "(^|[\r\n ]+)--webengine([\r\n ]+|$)")
    list(APPEND webengine_arguments --webengine)
    message(STATUS "Using windeployqt --webengine")
  else()
    # Qt 6.11 removed the legacy --webengine umbrella switch. These explicit
    # module switches are its supported equivalent; the verifier still
    # requires the complete helper/resource/locales closure.
    list(APPEND webengine_arguments
      -webenginecore -webenginequick -webenginewidgets)
    message(STATUS
      "windeployqt has no --webengine switch; using explicit WebEngine modules")
  endif()
  foreach(deploy_pair IN ITEMS
      "host|${host_directory}/qbrowser-host.exe"
      "runtime|${runtime_directory}/qbrowser-worker.exe")
    string(REPLACE "|" ";" deploy_parts "${deploy_pair}")
    list(GET deploy_parts 0 deploy_subdirectory)
    list(GET deploy_parts 1 deploy_executable)
    execute_process(
      COMMAND "${windeployqt}"
        --release
        --force
        --compiler-runtime
        --force-openssl
        --skip-plugin-types qmltooling
        --exclude-plugins qtposition_nmea
        --translations en
        --qmldir "${Q_BROWSER_REPO_ROOT}/qml"
        ${webengine_arguments}
        --dir "${Q_BROWSER_DEPLOY_DIR}/${deploy_subdirectory}"
        "${deploy_executable}"
      RESULT_VARIABLE deploy_result
      OUTPUT_VARIABLE deploy_output
      ERROR_VARIABLE deploy_error)
    if(NOT deploy_result EQUAL 0)
      q_browser_deploy_fail(
        "windeployqt failed for ${deploy_subdirectory} (${deploy_result}): ${deploy_output}\n${deploy_error}")
    endif()
  endforeach()
  file(REMOVE "${host_directory}/qbrowser-worker.exe")

  file(MAKE_DIRECTORY
    "${Q_BROWSER_DEPLOY_DIR}/packages"
    "${Q_BROWSER_DEPLOY_DIR}/trust")
  file(COPY_FILE "${Q_BROWSER_PACKAGE_FILE}"
    "${Q_BROWSER_DEPLOY_DIR}/packages/com.qbrowser.pilot-1.0.0.qapkg"
    ONLY_IF_DIFFERENT)
  file(COPY_FILE "${Q_BROWSER_PUBLIC_KEY}"
    "${Q_BROWSER_DEPLOY_DIR}/trust/dev-public.pem"
    ONLY_IF_DIFFERENT)
  if(EXISTS "${Q_BROWSER_OPENSSL_ROOT}/bin/libcrypto-3-x64.dll")
    file(COPY_FILE "${Q_BROWSER_OPENSSL_ROOT}/bin/libcrypto-3-x64.dll"
      "${host_directory}/libcrypto-3-x64.dll" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${Q_BROWSER_OPENSSL_ROOT}/bin/libcrypto-3-x64.dll"
      "${runtime_directory}/libcrypto-3-x64.dll" ONLY_IF_DIFFERENT)
  endif()
  if(EXISTS "${Q_BROWSER_OPENSSL_ROOT}/bin/libssl-3-x64.dll")
    file(COPY_FILE "${Q_BROWSER_OPENSSL_ROOT}/bin/libssl-3-x64.dll"
      "${host_directory}/libssl-3-x64.dll" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${Q_BROWSER_OPENSSL_ROOT}/bin/libssl-3-x64.dll"
      "${runtime_directory}/libssl-3-x64.dll" ONLY_IF_DIFFERENT)
  endif()

  foreach(document IN ITEMS
      architecture/runtime.md
      package-spec/qapkg-v1.md
      security/threat-model.md
      security/windows-sandbox.md
      development/getting-started.md
      development/migrator.md
      operations/update-rollback.md
      operations/diagnostics.md)
    cmake_path(GET document PARENT_PATH document_parent)
    file(MAKE_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}/docs/${document_parent}")
    file(COPY_FILE "${Q_BROWSER_REPO_ROOT}/docs/${document}"
      "${Q_BROWSER_DEPLOY_DIR}/docs/${document}" ONLY_IF_DIFFERENT)
  endforeach()

  q_browser_canonical_inventory(generated_inventory)
  file(WRITE "${Q_BROWSER_DEPLOY_DIR}/SHA-256SUMS" "${generated_inventory}")
  set(Q_BROWSER_PACKAGE_CLI "${host_directory}/qbrowser-package.exe")
  set(Q_BROWSER_PUBLIC_KEY "${Q_BROWSER_DEPLOY_DIR}/trust/dev-public.pem")
  set(Q_BROWSER_DEPLOY_MODE VERIFY)
elseif(NOT Q_BROWSER_DEPLOY_MODE STREQUAL "VERIFY")
  q_browser_deploy_fail("unknown mode: ${Q_BROWSER_DEPLOY_MODE}")
endif()

if(NOT IS_DIRECTORY "${Q_BROWSER_DEPLOY_DIR}")
  q_browser_deploy_fail("deployment directory does not exist: ${Q_BROWSER_DEPLOY_DIR}")
endif()

foreach(required_file IN ITEMS
    host/qbrowser-host.exe
    host/qbrowser-package.exe
    runtime/qbrowser-worker.exe
    packages/com.qbrowser.pilot-1.0.0.qapkg
    trust/dev-public.pem
    host/platforms/qwindows.dll
    host/resources/icudtl.dat
    host/resources/qtwebengine_resources.pak
    host/resources/qtwebengine_resources_100p.pak
    host/resources/qtwebengine_resources_200p.pak
    host/resources/qtwebengine_devtools_resources.pak
    host/resources/v8_context_snapshot.bin
    host/translations/qtwebengine_locales/en-US.pak
    host/qml/QtQuick/qtquick2plugin.dll
    host/tls/qopensslbackend.dll
    host/tls/qschannelbackend.dll
    runtime/platforms/qwindows.dll
    runtime/resources/icudtl.dat
    runtime/resources/qtwebengine_resources.pak
    runtime/resources/qtwebengine_resources_100p.pak
    runtime/resources/qtwebengine_resources_200p.pak
    runtime/resources/qtwebengine_devtools_resources.pak
    runtime/resources/v8_context_snapshot.bin
    runtime/translations/qtwebengine_locales/en-US.pak
    runtime/qml/QtQuick/qtquick2plugin.dll
    runtime/tls/qopensslbackend.dll
    runtime/tls/qschannelbackend.dll
    docs/architecture/runtime.md
    docs/package-spec/qapkg-v1.md
    docs/security/threat-model.md
    docs/security/windows-sandbox.md
    docs/development/getting-started.md
    docs/development/migrator.md
    docs/operations/update-rollback.md
    docs/operations/diagnostics.md
    SHA-256SUMS)
  q_browser_require_file("${required_file}")
endforeach()

foreach(runtime_subdirectory IN ITEMS host runtime)
  q_browser_require_glob("Qt Core runtime" "${runtime_subdirectory}/Qt6Core.dll")
  q_browser_require_glob("Qt Quick runtime" "${runtime_subdirectory}/Qt6Quick.dll")
  q_browser_require_glob("Qt WebEngine runtime" "${runtime_subdirectory}/Qt6WebEngineCore.dll")
  q_browser_require_glob("Qt WebEngine helper" "${runtime_subdirectory}/QtWebEngineProcess.exe")
  q_browser_require_glob("OpenSSL Crypto runtime" "${runtime_subdirectory}/libcrypto-3*.dll")
  q_browser_require_glob("OpenSSL TLS runtime" "${runtime_subdirectory}/libssl-3*.dll")
endforeach()

file(GLOB_RECURSE deployed_files
  LIST_DIRECTORIES false
  RELATIVE "${Q_BROWSER_DEPLOY_DIR}"
  "${Q_BROWSER_DEPLOY_DIR}/*")
foreach(relative_path IN LISTS deployed_files)
  string(REPLACE "\\" "/" normalized_path "${relative_path}")
  string(TOLOWER "${normalized_path}" lower_path)
  if(lower_path MATCHES "(^|/)(tests?|fixtures?)(/|$)"
      OR lower_path MATCHES "(^|/)(tst_[^/]*|q_browser_build_smoke)\\.exe$"
      OR lower_path MATCHES "(^|/).*(private|secret).*\\.(pem|key)$"
      OR lower_path MATCHES "\\.(cpp|cxx|cc|h|hpp|pdb|ilk|obj|lib|exp)$")
    q_browser_deploy_fail("forbidden test, source, symbol, or private-key asset: ${normalized_path}")
  endif()
  file(READ "${Q_BROWSER_DEPLOY_DIR}/${relative_path}" prefix LIMIT 256)
  if(prefix MATCHES "-----BEGIN (ENCRYPTED )?PRIVATE KEY-----")
    q_browser_deploy_fail("private key material found in ${normalized_path}")
  endif()
endforeach()

file(READ "${Q_BROWSER_DEPLOY_DIR}/SHA-256SUMS" recorded_inventory)
q_browser_canonical_inventory(expected_inventory)
if(NOT recorded_inventory STREQUAL expected_inventory)
  q_browser_deploy_fail("SHA-256SUMS does not match the canonical sorted inventory")
endif()

if(DEFINED Q_BROWSER_PACKAGE_CLI AND DEFINED Q_BROWSER_PUBLIC_KEY)
  execute_process(
    COMMAND "${Q_BROWSER_PACKAGE_CLI}" inspect
      --package "${Q_BROWSER_DEPLOY_DIR}/packages/com.qbrowser.pilot-1.0.0.qapkg"
      --public-key "${Q_BROWSER_PUBLIC_KEY}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE inspect_output
    ERROR_VARIABLE inspect_error)
  if(NOT inspect_result EQUAL 0
      OR NOT inspect_output MATCHES "\"verified\":true"
      OR NOT inspect_output MATCHES "\"appId\":\"com.qbrowser.pilot\"")
    q_browser_deploy_fail("Pilot signature inspection failed: ${inspect_error}")
  endif()
endif()

message(STATUS "Q-Browser deployment verified: ${Q_BROWSER_DEPLOY_DIR}")
