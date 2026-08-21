if(BUILD_TESTING)
  if(WIN32)
    add_subdirectory(helpers/sandbox_probe)
    add_subdirectory(helpers/qt_sandbox_probe)
    add_subdirectory(security)
  endif()

  add_subdirectory(integration/broker)
  add_subdirectory(integration/ipc)
  add_subdirectory(integration/package)
  add_subdirectory(integration/pilot)
  add_subdirectory(integration/worker)
  add_subdirectory(integration/update)
  add_subdirectory(quick)
  if(Q_BROWSER_BUILD_WEBENGINE)
    add_subdirectory(integration/host)
    add_subdirectory(e2e)
  endif()
  add_subdirectory(unit/broker)
  if(Q_BROWSER_BUILD_WEBENGINE)
    add_subdirectory(unit/host)
  endif()
  add_subdirectory(unit/ipc)
  add_subdirectory(unit/package)
  add_subdirectory(unit/policy)
  add_subdirectory(unit/router)
  add_subdirectory(unit/telemetry)

  if(Q_BROWSER_BUILD_WEBENGINE)
    add_subdirectory(unit/webengine)
  endif()

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
