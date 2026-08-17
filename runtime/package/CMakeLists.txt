add_library(q_browser_package STATIC
  JsonPreflight.cpp
  JsonPreflight.h
  Manifest.cpp
  Manifest.h
  ManifestError.h
  ManifestResourceLimits.h)

target_include_directories(q_browser_package
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_package
  PUBLIC
    Qt6::Core
  PRIVATE
    q_browser_router)

q_browser_enable_strict_warnings(q_browser_package)
