if(NOT WIN32)
  return()
endif()

add_library(q_browser_sandbox STATIC
  AclGrant.cpp
  AclGrant.h
  AppContainerProfile.cpp
  AppContainerProfile.h
  JobLimits.cpp
  JobLimits.h
  SandboxLauncher.cpp
  SandboxLauncher.h
  SandboxError.h
  SandboxTrustBoundary.cpp
  SandboxTrustBoundary.h)

target_include_directories(q_browser_sandbox
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_sandbox
  PUBLIC
    q_browser_ipc
    Qt6::Core
  PRIVATE
    Advapi32
    OneCoreUAP
    Userenv)

q_browser_enable_strict_warnings(q_browser_sandbox)
