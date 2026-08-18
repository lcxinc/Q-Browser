add_executable(qbrowser-package
  main.cpp)

target_link_libraries(qbrowser-package
  PRIVATE
    q_browser_package
    OpenSSL::Crypto
    Qt6::Core)

if(WIN32)
  target_link_libraries(qbrowser-package PRIVATE Advapi32)
endif()

q_browser_enable_strict_warnings(qbrowser-package)
