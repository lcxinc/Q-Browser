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

if(BUILD_TESTING)
  target_compile_definitions(qbrowser-package
    PRIVATE
      Q_BROWSER_ARCHIVE_TESTING
      Q_BROWSER_PACKAGE_CLI_TESTING)
endif()
