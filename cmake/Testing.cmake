include_guard(GLOBAL)

function(q_browser_add_qt_test)
  cmake_parse_arguments(PARSE_ARGV 0 q_browser_test "" "NAME;TARGET" "")

  if(NOT q_browser_test_NAME OR NOT q_browser_test_TARGET)
    message(FATAL_ERROR "q_browser_add_qt_test requires NAME and TARGET")
  endif()
  if(q_browser_test_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "q_browser_add_qt_test received unexpected arguments: ${q_browser_test_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT TARGET "${q_browser_test_TARGET}")
    message(FATAL_ERROR
      "q_browser_add_qt_test target does not exist: ${q_browser_test_TARGET}")
  endif()

  add_test(
    NAME "${q_browser_test_NAME}"
    COMMAND "$<TARGET_FILE:${q_browser_test_TARGET}>")
  set_property(
    TEST "${q_browser_test_NAME}"
    PROPERTY ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endfunction()
