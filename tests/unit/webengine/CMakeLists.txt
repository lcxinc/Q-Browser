add_executable(tst_request_interceptor
  tst_request_interceptor.cpp)

target_link_libraries(tst_request_interceptor
  PRIVATE
    q_browser_webengine
    Qt6::Network
    Qt6::Test
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::Widgets)

q_browser_enable_strict_warnings(tst_request_interceptor)
q_browser_add_qt_test(
  NAME request_interceptor
  TARGET tst_request_interceptor)

set_property(
  TEST request_interceptor
  APPEND PROPERTY ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen")

add_executable(tst_webengine_file_selection
  tst_webengine_file_selection.cpp)

target_link_libraries(tst_webengine_file_selection
  PRIVATE
    q_browser_webengine
    Qt6::Network
    Qt6::Test
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::Widgets)

q_browser_enable_strict_warnings(tst_webengine_file_selection)
q_browser_add_qt_test(
  NAME webengine_file_selection
  TARGET tst_webengine_file_selection)
