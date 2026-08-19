add_library(q_browser_worker_app STATIC
  PendingCapabilityQueue.cpp
  PendingCapabilityQueue.h
  RuntimeFacade.cpp
  RuntimeFacade.h
  WorkerApplication.cpp
  WorkerApplication.h
  WorkerNetworkAccess.cpp
  WorkerNetworkAccess.h
  WorkerWindow.cpp
  WorkerWindow.h)

target_include_directories(q_browser_worker_app
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_worker_app
  PUBLIC
    q_browser_ipc
    q_browser_router
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
  PRIVATE
    q_browser_design)

q_browser_enable_strict_warnings(q_browser_worker_app)

add_executable(qbrowser-worker WIN32 main.cpp)
target_link_libraries(qbrowser-worker PRIVATE q_browser_worker_app)
q_browser_enable_strict_warnings(qbrowser-worker)
