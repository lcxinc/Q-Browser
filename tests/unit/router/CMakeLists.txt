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
