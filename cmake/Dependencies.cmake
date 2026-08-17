include(FetchContent)

set(q_browser_qt_components
  Core
  Gui
  Widgets
  Quick
  Qml
  QuickControls2
  Network
  Test
  QuickTest)

if(Q_BROWSER_BUILD_WEBENGINE)
  list(APPEND q_browser_qt_components WebEngineCore WebEngineWidgets)
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS ${q_browser_qt_components})
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 174573d60290f447c13a2b1b3405de2b96e27d6c
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniz)
