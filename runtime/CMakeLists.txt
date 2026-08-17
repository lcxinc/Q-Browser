set(q_browser_runtime_components
  router
  package
  policy
  ipc
  telemetry
  broker
  sandbox/windows
  worker
  webengine)

foreach(q_browser_runtime_component IN LISTS q_browser_runtime_components)
  if(EXISTS
      "${CMAKE_CURRENT_SOURCE_DIR}/${q_browser_runtime_component}/CMakeLists.txt")
    add_subdirectory("${q_browser_runtime_component}")
  endif()
endforeach()
