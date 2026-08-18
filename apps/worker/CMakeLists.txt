add_executable(qbrowser-worker WIN32
  main.cpp
  RuntimeFacade.cpp
  RuntimeFacade.h
  WorkerApplication.cpp
  WorkerApplication.h
  WorkerWindow.cpp
  WorkerWindow.h)

target_include_directories(qbrowser-worker
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(qbrowser-worker
  PRIVATE
    q_browser_ipc
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick)

q_browser_enable_strict_warnings(qbrowser-worker)
