add_library(q_browser_broker STATIC
  CapabilityBroker.cpp
  CapabilityBroker.h
  ClipboardBroker.cpp
  ClipboardBroker.h
  FileBroker.cpp
  FileBroker.h
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

q_browser_enable_strict_warnings(q_browser_broker)
