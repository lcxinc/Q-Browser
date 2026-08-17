cmake_minimum_required(VERSION 3.30)

project(QBrowser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(CTest)

option(Q_BROWSER_BUILD_WEBENGINE "Build the WebEngine fallback surface" ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

foreach(q_browser_subdirectory IN ITEMS apps runtime qml tests)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${q_browser_subdirectory}/CMakeLists.txt")
    add_subdirectory("${q_browser_subdirectory}")
  endif()
endforeach()
