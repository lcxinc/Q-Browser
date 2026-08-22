find_program(Q_BROWSER_NODE_EXECUTABLE NAMES node REQUIRED)

set(q_browser_e2e_support
  TestEnvironment.cpp
  TestEnvironment.h
  ../integration/worker/WorkerTestEnvironment.cpp
  ../integration/worker/WorkerTestEnvironment.h)

foreach(q_browser_e2e_test IN ITEMS
    host_routes
    web_fallback
    package_update
    host_survives_worker_crash
    pilot_capabilities)
  add_executable(tst_e2e_${q_browser_e2e_test}
    tst_${q_browser_e2e_test}.cpp
    ${q_browser_e2e_support})
  target_include_directories(tst_e2e_${q_browser_e2e_test} PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/integration/worker")
  target_link_libraries(tst_e2e_${q_browser_e2e_test} PRIVATE
    q_browser_host
    q_browser_package
    q_browser_sandbox
    q_browser_worker_app
    Qt6::Test
    Qt6::Widgets
    Userenv)
  target_compile_definitions(tst_e2e_${q_browser_e2e_test} PRIVATE
    Q_BROWSER_ARCHIVE_TESTING
    Q_BROWSER_NODE_PATH="${Q_BROWSER_NODE_EXECUTABLE}"
    Q_BROWSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
    Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
    Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")
  add_dependencies(tst_e2e_${q_browser_e2e_test} qbrowser-host qbrowser-worker)
  q_browser_enable_strict_warnings(tst_e2e_${q_browser_e2e_test})
  q_browser_add_qt_test(
    NAME e2e_${q_browser_e2e_test}
    TARGET tst_e2e_${q_browser_e2e_test})
  set_tests_properties(e2e_${q_browser_e2e_test} PROPERTIES TIMEOUT 240)
endforeach()
