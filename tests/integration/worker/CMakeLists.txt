set(q_browser_worker_test_support
  WorkerTestEnvironment.cpp
  WorkerTestEnvironment.h)

foreach(q_browser_worker_test IN ITEMS handshake surface crash)
  add_executable(tst_worker_${q_browser_worker_test}
    tst_worker_${q_browser_worker_test}.cpp
    ${q_browser_worker_test_support})
  target_link_libraries(tst_worker_${q_browser_worker_test}
    PRIVATE
      q_browser_ipc
      q_browser_sandbox
      q_browser_worker_runtime
      Qt6::Gui
      Qt6::Quick
      Qt6::Test
      Qt6::Widgets
      Userenv)
  target_compile_definitions(tst_worker_${q_browser_worker_test}
    PRIVATE
      Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
      Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}")
  add_dependencies(tst_worker_${q_browser_worker_test} qbrowser-worker)
  q_browser_enable_strict_warnings(tst_worker_${q_browser_worker_test})
  q_browser_add_qt_test(
    NAME worker_${q_browser_worker_test}
    TARGET tst_worker_${q_browser_worker_test})
endforeach()
