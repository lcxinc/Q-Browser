add_library(q_browser_policy STATIC
  EffectivePolicy.h
  HostPolicy.cpp
  HostPolicy.h
  PolicyEngine.cpp
  PolicyEngine.h)

target_include_directories(q_browser_policy
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_policy
  PUBLIC
    q_browser_package
    Qt6::Core)

q_browser_enable_strict_warnings(q_browser_policy)
