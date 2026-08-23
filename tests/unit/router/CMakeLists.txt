add_executable(tst_app_url
  tst_app_url.cpp)

target_link_libraries(tst_app_url
  PRIVATE
    q_browser_router
    Qt6::Test)

q_browser_enable_strict_warnings(tst_app_url)

q_browser_add_qt_test(
  NAME app_url
  TARGET tst_app_url)

add_executable(tst_route_registry
  tst_route_registry.cpp)

target_link_libraries(tst_route_registry
  PRIVATE
    q_browser_router
    Qt6::Test)

q_browser_enable_strict_warnings(tst_route_registry)

q_browser_add_qt_test(
  NAME route_registry
  TARGET tst_route_registry)

add_executable(tst_normalized_path
  tst_normalized_path.cpp)

target_link_libraries(tst_normalized_path
  PRIVATE
    q_browser_router
    Qt6::Test)

q_browser_enable_strict_warnings(tst_normalized_path)

q_browser_add_qt_test(
  NAME normalized_path
  TARGET tst_normalized_path)

add_executable(tst_browser_address
  tst_browser_address.cpp)

target_link_libraries(tst_browser_address
  PRIVATE
    q_browser_router
    Qt6::Test)

q_browser_enable_strict_warnings(tst_browser_address)

q_browser_add_qt_test(
  NAME browser_address
  TARGET tst_browser_address)
