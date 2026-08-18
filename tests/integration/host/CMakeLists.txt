add_executable(tst_unified_navigation
  tst_unified_navigation.cpp
  ../worker/WorkerTestEnvironment.cpp
  ../worker/WorkerTestEnvironment.h)

target_include_directories(tst_unified_navigation
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../worker")

target_link_libraries(tst_unified_navigation
  PRIVATE
    q_browser_host
    q_browser_ipc
    q_browser_sandbox
    q_browser_worker_app
    q_browser_worker_runtime
    Qt6::Network
    Qt6::Quick
    Qt6::Test
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::Widgets
    Userenv)

target_compile_definitions(tst_unified_navigation
  PRIVATE
    Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
    Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
    Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")

add_dependencies(tst_unified_navigation qbrowser-worker)
q_browser_enable_strict_warnings(tst_unified_navigation)
q_browser_add_qt_test(
  NAME unified_navigation
  TARGET tst_unified_navigation)
