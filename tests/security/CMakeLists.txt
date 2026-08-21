add_executable(tst_sandbox_launcher
  tst_sandbox_launcher.cpp)

target_link_libraries(tst_sandbox_launcher
  PRIVATE
    q_browser_sandbox
    q_browser_ipc
    Qt6::Network
    Qt6::Test)

target_compile_definitions(tst_sandbox_launcher
  PRIVATE
    Q_BROWSER_SANDBOX_TESTING
    Q_BROWSER_SANDBOX_PROBE_PATH="$<TARGET_FILE:q_browser_sandbox_probe>"
    Q_BROWSER_QT_SANDBOX_PROBE_PATH="$<TARGET_FILE:q_browser_qt_sandbox_probe>"
    Q_BROWSER_QT_CORE_DLL_PATH="$<TARGET_FILE:Qt6::Core>")

add_dependencies(tst_sandbox_launcher
  q_browser_qt_sandbox_probe
  q_browser_sandbox_probe)

q_browser_enable_strict_warnings(tst_sandbox_launcher)

q_browser_add_qt_test(
  NAME sandbox_launcher
  TARGET tst_sandbox_launcher)

add_executable(tst_malicious_package
  tst_malicious_package.cpp)
target_link_libraries(tst_malicious_package PRIVATE
  q_browser_package
  Qt6::Test)
q_browser_enable_strict_warnings(tst_malicious_package)
q_browser_add_qt_test(NAME malicious_package TARGET tst_malicious_package)

set(q_browser_security_worker_support
  ../integration/worker/WorkerTestEnvironment.cpp
  ../integration/worker/WorkerTestEnvironment.h)

foreach(q_browser_security_worker_test IN ITEMS capability_escape worker_api_surface)
  add_executable(tst_${q_browser_security_worker_test}
    tst_${q_browser_security_worker_test}.cpp
    ${q_browser_security_worker_support})
  target_include_directories(tst_${q_browser_security_worker_test} PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/integration/worker")
  target_link_libraries(tst_${q_browser_security_worker_test} PRIVATE
    q_browser_broker
    q_browser_ipc
    q_browser_package
    q_browser_sandbox
    q_browser_worker_app
    q_browser_worker_runtime
    Qt6::Gui
    Qt6::Network
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
    Userenv)
  target_compile_definitions(tst_${q_browser_security_worker_test} PRIVATE
    Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
    Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
    Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")
  add_dependencies(tst_${q_browser_security_worker_test} qbrowser-worker)
  q_browser_enable_strict_warnings(tst_${q_browser_security_worker_test})
  q_browser_add_qt_test(
    NAME ${q_browser_security_worker_test}
    TARGET tst_${q_browser_security_worker_test})
endforeach()
