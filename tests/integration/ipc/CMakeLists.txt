add_executable(tst_ipc_session
  tst_ipc_session.cpp)

target_link_libraries(tst_ipc_session
  PRIVATE
    q_browser_ipc
    Qt6::Test)

q_browser_enable_strict_warnings(tst_ipc_session)

q_browser_add_qt_test(
  NAME ipc_session
  TARGET tst_ipc_session)
