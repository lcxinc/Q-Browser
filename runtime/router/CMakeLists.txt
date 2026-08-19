add_library(q_browser_router STATIC
  AppUrl.cpp
  AppUrl.h
  NormalizedPath.cpp
  NormalizedPath.h
  PilotRoutes.cpp
  PilotRoutes.h
  RouteRecord.h
  RouteRegistry.cpp
  RouteRegistry.h)

target_include_directories(q_browser_router
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_router
  PUBLIC
    Qt6::Core)

q_browser_enable_strict_warnings(q_browser_router)
