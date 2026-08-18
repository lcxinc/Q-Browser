add_library(q_browser_ipc STATIC
  FrameCodec.cpp
  FrameCodec.h
  IpcSession.cpp
  IpcSession.h
  ProtocolMessage.cpp
  ProtocolMessage.h
  WinPipeTransport.cpp
  WinPipeTransport.h)

target_include_directories(q_browser_ipc
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_ipc
  PUBLIC
    Qt6::Core)

q_browser_enable_strict_warnings(q_browser_ipc)
