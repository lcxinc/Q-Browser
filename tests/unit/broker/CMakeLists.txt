add_executable(tst_capability_broker
  tst_capability_broker.cpp)

target_link_libraries(tst_capability_broker
  PRIVATE
    q_browser_broker
    Qt6::Test)

target_compile_definitions(tst_capability_broker
  PRIVATE
    Q_BROWSER_BROKER_TESTING
    Q_BROWSER_FILE_BROKER_SOURCE_FILE="${PROJECT_SOURCE_DIR}/runtime/broker/FileBroker.cpp")

q_browser_enable_strict_warnings(tst_capability_broker)

q_browser_add_qt_test(
  NAME capability_broker
  TARGET tst_capability_broker)
