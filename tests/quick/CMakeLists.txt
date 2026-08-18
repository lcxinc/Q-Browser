qt_add_executable(qbrowser_quick_tests
  main.cpp)

target_sources(qbrowser_quick_tests
  PRIVATE
    tst_AppButton.qml
    tst_AppDialog.qml
    tst_AppNavigation.qml
    tst_AppTable.qml
    tst_AppTextField.qml
    tst_StateView.qml)

target_link_libraries(qbrowser_quick_tests
  PRIVATE
    q_browser_design
    Qt6::QuickTest)

target_compile_definitions(qbrowser_quick_tests
  PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

q_browser_enable_strict_warnings(qbrowser_quick_tests)

set(q_browser_quick_test_temp "${CMAKE_CURRENT_BINARY_DIR}/temp")
file(MAKE_DIRECTORY "${q_browser_quick_test_temp}")

add_test(
  NAME quick_design
  COMMAND "$<TARGET_FILE:qbrowser_quick_tests>"
    -platform windows)

set_property(
  TEST quick_design
  PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    "QT_QUICK_BACKEND=set:software"
    "TEMP=set:${q_browser_quick_test_temp}"
    "TMP=set:${q_browser_quick_test_temp}")
