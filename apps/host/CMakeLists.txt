if(NOT Q_BROWSER_BUILD_WEBENGINE)
  return()
endif()

add_library(q_browser_host STATIC
  HostApplication.cpp
  HostApplication.h
  HostWorkerSessionController.cpp
  HostWorkerSessionController.h
  HostWorkerSessionIo.cpp
  HostWorkerSessionIo.h
  MainWindow.cpp
  MainWindow.h
  NavigationBar.cpp
  NavigationBar.h)

target_include_directories(q_browser_host
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_host
  PUBLIC
    q_browser_ipc
    q_browser_router
    q_browser_webengine
    q_browser_worker_runtime
    Qt6::Core
    Qt6::Widgets)

q_browser_enable_strict_warnings(q_browser_host)

add_executable(qbrowser-host WIN32 main.cpp)
target_link_libraries(qbrowser-host PRIVATE q_browser_host)
q_browser_enable_strict_warnings(qbrowser-host)
