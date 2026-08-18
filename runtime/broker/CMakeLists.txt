add_library(q_browser_broker STATIC
  CapabilityBroker.cpp
  CapabilityBroker.h
  ClipboardBroker.cpp
  ClipboardBroker.h
  FileBroker.cpp
  FileBroker.h
  FileDialogTestHooks.cpp
  FileDialogTestHooks.h
  NetworkBroker.cpp
  NetworkBroker.h
  StorageBroker.cpp
  StorageBroker.h
  UserGestureGrantStore.cpp
  UserGestureGrantStore.h)

target_include_directories(q_browser_broker
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_broker
  PUBLIC
    q_browser_policy
    q_browser_ipc
    Qt6::Core
  PRIVATE
    Qt6::Gui
    Qt6::Network
    Qt6::Widgets)

if(WIN32)
  target_link_libraries(q_browser_broker PRIVATE Ole32 Shell32)
endif()

if(BUILD_TESTING)
  target_compile_definitions(q_browser_broker PRIVATE Q_BROWSER_BROKER_TESTING)
endif()

q_browser_enable_strict_warnings(q_browser_broker)
