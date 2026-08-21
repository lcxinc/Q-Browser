cmake_minimum_required(VERSION 3.30)

project(QBrowser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# The release inventory is itself a signed operational control.  Make MSVC
# derive PE metadata from the linked content instead of the wall clock so two
# clean builds of the same revision have identical deployment hashes.
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Brepro>)
  add_link_options($<$<CONFIG:Release>:/Brepro>)
endif()

include(CTest)
include(InstallRequiredSystemLibraries)

option(Q_BROWSER_BUILD_WEBENGINE "Build the WebEngine fallback surface" ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Testing.cmake)

add_subdirectory(runtime)
add_subdirectory(qml)
add_subdirectory(apps)
add_subdirectory(tools/package-cli)

install(TARGETS
    qbrowser-worker
    qbrowser-package
  RUNTIME DESTINATION .
  COMPONENT Runtime)
if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
    DESTINATION .
    COMPONENT Runtime)
endif()
if(TARGET qbrowser-host)
  install(TARGETS qbrowser-host
    RUNTIME DESTINATION .
    COMPONENT Runtime)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
