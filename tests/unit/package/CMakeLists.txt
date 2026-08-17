add_executable(tst_manifest
  tst_manifest.cpp)

target_link_libraries(tst_manifest
  PRIVATE
    q_browser_package
    Qt6::Test)

target_compile_definitions(tst_manifest
  PRIVATE
    Q_BROWSER_MANIFEST_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/manifests"
    Q_BROWSER_MANIFEST_SCHEMA_FILE="${PROJECT_SOURCE_DIR}/schemas/qapkg-manifest-v1.schema.json")

q_browser_enable_strict_warnings(tst_manifest)

q_browser_add_qt_test(
  NAME manifest
  TARGET tst_manifest)
