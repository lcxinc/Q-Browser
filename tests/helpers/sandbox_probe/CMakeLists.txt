add_executable(q_browser_sandbox_probe
  main.cpp)

if(MSVC)
  set_property(TARGET q_browser_sandbox_probe PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(q_browser_sandbox_probe
  PRIVATE
    Shell32
    Ws2_32)

q_browser_enable_strict_warnings(q_browser_sandbox_probe)
