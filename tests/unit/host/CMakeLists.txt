add_executable(tst_host_runtime_config tst_host_runtime_config.cpp)

target_link_libraries(tst_host_runtime_config
  PRIVATE
    q_browser_host
    q_browser_package
    Qt6::Test)

if(WIN32)
  target_link_libraries(tst_host_runtime_config PRIVATE Advapi32)
endif()

q_browser_enable_strict_warnings(tst_host_runtime_config)
q_browser_add_qt_test(NAME host_runtime_config TARGET tst_host_runtime_config)
