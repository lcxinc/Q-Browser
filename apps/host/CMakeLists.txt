if(NOT Q_BROWSER_BUILD_WEBENGINE)
  return()
endif()

add_library(q_browser_host STATIC
  BrowserTabModel.cpp
  BrowserTabModel.h
  HostApplication.cpp
  HostApplication.h
  HostCapabilityRuntime.cpp
  HostCapabilityRuntime.h
  HostRuntimeConfig.cpp
  HostRuntimeConfig.h
  InstalledPackageWorkerLauncher.cpp
  InstalledPackageWorkerLauncher.h
  InstalledPackageWorkerLauncherTestHooks.cpp
  InstalledPackageWorkerLauncherTestHooks.h
  RuntimePackageAuthority.cpp
  RuntimePackageAuthority.h
  WorkerRetirementManager.cpp
  WorkerRetirementManager.h
  HostWorkerSessionController.cpp
  HostWorkerSessionController.h
  HostWorkerSessionIo.cpp
  HostWorkerSessionIo.h
  HostWorkerSessionTestHooks.cpp
  HostWorkerSessionTestHooks.h
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
    q_browser_broker
    q_browser_package
    q_browser_router
    q_browser_sandbox
    q_browser_webengine
    q_browser_worker_runtime
    Qt6::Core
    Qt6::Widgets)

if(WIN32)
  target_link_libraries(q_browser_host PRIVATE bcrypt)
endif()

q_browser_enable_strict_warnings(q_browser_host)

if(BUILD_TESTING)
  target_compile_definitions(q_browser_host PUBLIC Q_BROWSER_HOST_TESTING)
endif()

add_executable(qbrowser-host WIN32 main.cpp)
target_link_libraries(qbrowser-host PRIVATE q_browser_host)
q_browser_enable_strict_warnings(qbrowser-host)
