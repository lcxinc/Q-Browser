add_executable(tst_manifest
  tst_manifest.cpp)

find_program(Q_BROWSER_NODE_EXECUTABLE NAMES node REQUIRED)

target_link_libraries(tst_manifest
  PRIVATE
    q_browser_package
    Qt6::Test)

target_compile_definitions(tst_manifest
  PRIVATE
    Q_BROWSER_MANIFEST_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/manifests"
    Q_BROWSER_MANIFEST_SCHEMA_FILE="${PROJECT_SOURCE_DIR}/schemas/qapkg-manifest-v1.schema.json"
    Q_BROWSER_NODE_EXECUTABLE="${Q_BROWSER_NODE_EXECUTABLE}")

q_browser_enable_strict_warnings(tst_manifest)

q_browser_add_qt_test(
  NAME manifest
  TARGET tst_manifest)

add_executable(tst_archive
  tst_archive.cpp)

target_link_libraries(tst_archive
  PRIVATE
    q_browser_package
    Qt6::Test)

target_compile_definitions(tst_archive
  PRIVATE
    Q_BROWSER_ARCHIVE_TESTING
    Q_BROWSER_ARCHIVE_SOURCE_FILE="${PROJECT_SOURCE_DIR}/runtime/package/Archive.cpp")

q_browser_enable_strict_warnings(tst_archive)

q_browser_add_qt_test(
  NAME archive
  TARGET tst_archive)
