add_library(q_browser_telemetry STATIC
  EventRecorder.cpp
  EventRecorder.h
  SafeEvent.cpp
  SafeEvent.h)

target_include_directories(q_browser_telemetry
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_telemetry PUBLIC Qt6::Core)
q_browser_enable_strict_warnings(q_browser_telemetry)
