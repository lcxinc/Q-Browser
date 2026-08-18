add_library(q_browser_worker_runtime STATIC
  WorkerSupervisor.cpp
  WorkerSupervisor.h
  WorkerSurface.cpp
  WorkerSurface.h)

target_include_directories(q_browser_worker_runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_worker_runtime
  PUBLIC
    Qt6::Gui
    Qt6::Widgets)

q_browser_enable_strict_warnings(q_browser_worker_runtime)
