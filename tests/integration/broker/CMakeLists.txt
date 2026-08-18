add_executable(tst_network_broker
  tst_network_broker.cpp)

target_link_libraries(tst_network_broker
  PRIVATE
    q_browser_broker
    Qt6::Network
    Qt6::Test)

q_browser_enable_strict_warnings(tst_network_broker)

q_browser_add_qt_test(
  NAME network_broker
  TARGET tst_network_broker)

add_executable(tst_storage_broker
  tst_storage_broker.cpp)

target_link_libraries(tst_storage_broker
  PRIVATE
    q_browser_broker
    Qt6::Test)

target_compile_definitions(tst_storage_broker PRIVATE Q_BROWSER_BROKER_TESTING)

q_browser_enable_strict_warnings(tst_storage_broker)

q_browser_add_qt_test(
  NAME storage_broker
  TARGET tst_storage_broker)
