function(q_browser_add_update_test target source test_name)
  add_executable(${target}
    ${source}
    UpdateTestSupport.h)
  target_link_libraries(${target}
    PRIVATE
      q_browser_package
      q_browser_telemetry
      q_browser_worker_runtime
      Qt6::Test)
  target_compile_definitions(${target}
    PRIVATE
      Q_BROWSER_ARCHIVE_TESTING
      Q_BROWSER_PACKAGE_INSTALLER_TESTING)
  q_browser_enable_strict_warnings(${target})
  q_browser_add_qt_test(
    NAME ${test_name}
    TARGET ${target})
endfunction()

q_browser_add_update_test(
  tst_update_lifecycle tst_update_lifecycle.cpp update_lifecycle)
q_browser_add_update_test(
  tst_crash_rollback tst_crash_rollback.cpp crash_rollback)
q_browser_add_update_test(
  tst_offline_lkg tst_offline_lkg.cpp offline_lkg)

if(WIN32)
  target_sources(tst_crash_rollback PRIVATE
    ../worker/WorkerTestEnvironment.cpp
    ../worker/WorkerTestEnvironment.h)
  target_link_libraries(tst_crash_rollback
    PRIVATE
      q_browser_ipc
      q_browser_sandbox
      q_browser_worker_app
      Qt6::Gui
      Qt6::Quick
      Qt6::Widgets
      Advapi32
      Userenv)
  target_include_directories(tst_crash_rollback
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/integration/worker")
  target_compile_definitions(tst_crash_rollback PRIVATE
    Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
    Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
    Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")
  add_dependencies(tst_crash_rollback qbrowser-worker)
endif()

if(WIN32 AND Q_BROWSER_BUILD_WEBENGINE)
  add_executable(tst_production_update_runtime
    tst_production_update_runtime.cpp
    ../worker/WorkerTestEnvironment.cpp
    ../worker/WorkerTestEnvironment.h
    UpdateTestSupport.h)
  target_link_libraries(tst_production_update_runtime
    PRIVATE
      q_browser_host
      q_browser_package
      q_browser_sandbox
      q_browser_worker_app
      Qt6::Test
      Qt6::Widgets
      Userenv)
  target_include_directories(tst_production_update_runtime
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/integration/worker")
  target_compile_definitions(tst_production_update_runtime PRIVATE
    Q_BROWSER_ARCHIVE_TESTING
    Q_BROWSER_PACKAGE_INSTALLER_TESTING
    Q_BROWSER_HOST_PATH="$<TARGET_FILE:qbrowser-host>"
    Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
    Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
    Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")
  add_dependencies(tst_production_update_runtime qbrowser-host qbrowser-worker)
  q_browser_enable_strict_warnings(tst_production_update_runtime)
  q_browser_add_qt_test(
    NAME production_update_runtime
    TARGET tst_production_update_runtime)
  set_tests_properties(production_update_runtime PROPERTIES
    TIMEOUT 900
    ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=900000")
endif()
