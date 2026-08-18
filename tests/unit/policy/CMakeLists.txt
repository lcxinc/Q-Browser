add_executable(tst_policy_engine
  tst_policy_engine.cpp)

target_link_libraries(tst_policy_engine
  PRIVATE
    q_browser_policy
    q_browser_package
    Qt6::Test)

q_browser_enable_strict_warnings(tst_policy_engine)

q_browser_add_qt_test(
  NAME policy_engine
  TARGET tst_policy_engine)
