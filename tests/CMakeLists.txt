if(BUILD_TESTING)
  add_subdirectory(integration/ipc)
  add_subdirectory(integration/package)
  add_subdirectory(unit/ipc)
  add_subdirectory(unit/package)
  add_subdirectory(unit/router)

  add_executable(q_browser_build_smoke
    smoke/tst_build_smoke.cpp)

  target_link_libraries(q_browser_build_smoke
    PRIVATE
      Qt6::Core
      Qt6::Test)

  q_browser_enable_strict_warnings(q_browser_build_smoke)

  q_browser_add_qt_test(
    NAME build_smoke
    TARGET q_browser_build_smoke)
endif()
