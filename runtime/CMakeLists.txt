set(q_browser_runtime_components
  router
  telemetry
  worker
  package
  policy
  ipc
  broker
  sandbox/windows
  webengine)

foreach(q_browser_runtime_component IN LISTS q_browser_runtime_components)
  if(EXISTS
      "${CMAKE_CURRENT_SOURCE_DIR}/${q_browser_runtime_component}/CMakeLists.txt")
    add_subdirectory("${q_browser_runtime_component}")
  endif()
endforeach()
