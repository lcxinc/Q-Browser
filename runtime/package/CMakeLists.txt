add_library(q_browser_package STATIC
  Archive.cpp
  Archive.h
  ArchiveLimits.h
  ArchiveTestHooks.cpp
  ArchiveTestHooks.h
  ContentDigest.cpp
  ContentDigest.h
  JsonPreflight.cpp
  JsonPreflight.h
  Manifest.cpp
  Manifest.h
  ManifestError.h
  ManifestResourceLimits.h
  SignatureVerifier.cpp
  SignatureVerifier.h
  SignatureTestHooks.cpp
  SignatureTestHooks.h
  WindowsStableIo.cpp
  WindowsStableIo.h)

target_include_directories(q_browser_package
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(q_browser_package
  PUBLIC
    Qt6::Core
  PRIVATE
    q_browser_router
    miniz
    OpenSSL::Crypto)

q_browser_enable_strict_warnings(q_browser_package)

if(BUILD_TESTING)
  target_compile_definitions(q_browser_package
    PRIVATE
      Q_BROWSER_ARCHIVE_TESTING
      Q_BROWSER_SIGNATURE_TESTING)
endif()
