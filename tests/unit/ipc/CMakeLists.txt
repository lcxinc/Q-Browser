add_executable(tst_frame_codec
  tst_frame_codec.cpp)

target_link_libraries(tst_frame_codec
  PRIVATE
    q_browser_ipc
    Qt6::Test)

q_browser_enable_strict_warnings(tst_frame_codec)

q_browser_add_qt_test(
  NAME frame_codec
  TARGET tst_frame_codec)

add_executable(tst_protocol_message
  tst_protocol_message.cpp)

target_link_libraries(tst_protocol_message
  PRIVATE
    q_browser_ipc
    Qt6::Test)

q_browser_enable_strict_warnings(tst_protocol_message)

q_browser_add_qt_test(
  NAME protocol_message
  TARGET tst_protocol_message)
