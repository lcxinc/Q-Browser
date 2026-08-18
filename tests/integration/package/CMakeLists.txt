add_executable(tst_package_cli
  tst_package_cli.cpp)

target_link_libraries(tst_package_cli
  PRIVATE
    q_browser_package
    Qt6::Test)

if(WIN32)
  target_link_libraries(tst_package_cli PRIVATE Advapi32)
endif()

target_compile_definitions(tst_package_cli
  PRIVATE
    Q_BROWSER_PACKAGE_EXE="$<TARGET_FILE:qbrowser-package>"
    Q_BROWSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_dependencies(tst_package_cli qbrowser-package)

q_browser_enable_strict_warnings(tst_package_cli)

q_browser_add_qt_test(
  NAME package_cli
  TARGET tst_package_cli)

add_executable(tst_package_installer
  tst_package_installer.cpp)

target_link_libraries(tst_package_installer
  PRIVATE
    q_browser_package
    Qt6::Test)

target_compile_definitions(tst_package_installer PRIVATE
  Q_BROWSER_ARCHIVE_TESTING
  Q_BROWSER_PACKAGE_INSTALLER_TESTING)

q_browser_enable_strict_warnings(tst_package_installer)

q_browser_add_qt_test(
  NAME package_installer
  TARGET tst_package_installer)
