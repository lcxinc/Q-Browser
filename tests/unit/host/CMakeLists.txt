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

add_executable(tst_host_capability_runtime tst_host_capability_runtime.cpp)
target_link_libraries(tst_host_capability_runtime
  PRIVATE
    q_browser_host
    Qt6::Network
    Qt6::Test
    Qt6::Widgets)
q_browser_enable_strict_warnings(tst_host_capability_runtime)
q_browser_add_qt_test(NAME host_capability_runtime TARGET tst_host_capability_runtime)

add_executable(tst_browser_tab_model tst_browser_tab_model.cpp)
target_link_libraries(tst_browser_tab_model
  PRIVATE
    q_browser_host
    Qt6::Test)
q_browser_enable_strict_warnings(tst_browser_tab_model)
q_browser_add_qt_test(NAME browser_tab_model TARGET tst_browser_tab_model)
