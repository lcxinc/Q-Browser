add_executable(tst_pilot_routes
  tst_pilot_routes.cpp
  ../worker/WorkerTestEnvironment.cpp
  ../worker/WorkerTestEnvironment.h)

target_include_directories(tst_pilot_routes PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../worker")

target_link_libraries(tst_pilot_routes
  PRIVATE
    q_browser_package
    q_browser_policy
    q_browser_router
    q_browser_ipc
    q_browser_sandbox
    q_browser_worker_app
    q_browser_worker_runtime
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
    Userenv)

target_compile_definitions(tst_pilot_routes PRIVATE
  Q_BROWSER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  Q_BROWSER_WORKER_PATH="$<TARGET_FILE:qbrowser-worker>"
  Q_BROWSER_WINDEPLOYQT_PATH="${WINDEPLOYQT_EXECUTABLE}"
  Q_BROWSER_DESIGN_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml")

add_dependencies(tst_pilot_routes qbrowser-worker)

q_browser_enable_strict_warnings(tst_pilot_routes)
q_browser_add_qt_test(NAME pilot_routes TARGET tst_pilot_routes)

set(q_browser_pilot_qml
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/Main.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/PilotRouter.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/models/RuntimeModels.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/LoginPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/DashboardPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/OrdersPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/OrderDetailPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/OrderEditPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/CustomersPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/CustomerDetailPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/FilesPage.qml"
  "${PROJECT_SOURCE_DIR}/packages/pilot/qml/pages/SettingsPage.qml")

add_test(
  NAME pilot_qmllint
  COMMAND "$<TARGET_FILE:Qt6::qmllint>"
    --ignore-settings
    -I "${CMAKE_BINARY_DIR}/qml"
    ${q_browser_pilot_qml})
set_property(
  TEST pilot_qmllint
  PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
