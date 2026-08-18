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
