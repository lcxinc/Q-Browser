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
target_compile_definitions(tst_host_capability_runtime
  PRIVATE
    Q_BROWSER_BROKER_TESTING)
q_browser_enable_strict_warnings(tst_host_capability_runtime)
q_browser_add_qt_test(NAME host_capability_runtime TARGET tst_host_capability_runtime)

add_executable(tst_browser_tab_model tst_browser_tab_model.cpp)
target_link_libraries(tst_browser_tab_model
  PRIVATE
    q_browser_host
    Qt6::Test)
q_browser_enable_strict_warnings(tst_browser_tab_model)
q_browser_add_qt_test(NAME browser_tab_model TARGET tst_browser_tab_model)

add_executable(tst_browser_window_geometry tst_browser_window_geometry.cpp)
target_link_libraries(tst_browser_window_geometry
  PRIVATE
    q_browser_host
    Qt6::Test)
q_browser_enable_strict_warnings(tst_browser_window_geometry)
q_browser_add_qt_test(
  NAME browser_window_geometry
  TARGET tst_browser_window_geometry)

add_executable(tst_browser_session_store tst_browser_session_store.cpp)
target_link_libraries(tst_browser_session_store
  PRIVATE
    q_browser_host
    Qt6::Test)
if(WIN32)
  target_link_libraries(tst_browser_session_store PRIVATE Advapi32)
endif()
q_browser_enable_strict_warnings(tst_browser_session_store)
q_browser_add_qt_test(NAME browser_session_store TARGET tst_browser_session_store)

add_executable(tst_new_tab_page tst_new_tab_page.cpp)
target_link_libraries(tst_new_tab_page
  PRIVATE
    q_browser_host
    Qt6::Test
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::Widgets)
q_browser_enable_strict_warnings(tst_new_tab_page)
q_browser_add_qt_test(NAME new_tab_page TARGET tst_new_tab_page)

add_executable(tst_browser_chrome tst_browser_chrome.cpp)
target_link_libraries(tst_browser_chrome
  PRIVATE
    q_browser_host
    Qt6::Test
    Qt6::Widgets)
q_browser_enable_strict_warnings(tst_browser_chrome)
q_browser_add_qt_test(NAME browser_chrome TARGET tst_browser_chrome)
