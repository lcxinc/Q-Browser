if(NOT Q_BROWSER_BUILD_WEBENGINE)
  return()
endif()

add_library(q_browser_webengine STATIC
  PilotRequestInterceptor.cpp
  PilotRequestInterceptor.h
  WebSessionProfile.cpp
  WebSessionProfile.h
  WebSurface.cpp
  WebSurface.h
  "${PROJECT_SOURCE_DIR}/resources/host.qrc")

target_include_directories(q_browser_webengine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_webengine
  PUBLIC
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::Widgets
  PRIVATE
    Qt6::Network)

if(BUILD_TESTING)
  target_compile_definitions(q_browser_webengine
    PUBLIC Q_BROWSER_WEBENGINE_TESTING)
endif()

q_browser_enable_strict_warnings(q_browser_webengine)
