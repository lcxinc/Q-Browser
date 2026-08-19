add_executable(tst_safe_event tst_safe_event.cpp)

target_link_libraries(tst_safe_event
  PRIVATE
    q_browser_telemetry
    Qt6::Test)

q_browser_enable_strict_warnings(tst_safe_event)
q_browser_add_qt_test(NAME safe_event TARGET tst_safe_event)
