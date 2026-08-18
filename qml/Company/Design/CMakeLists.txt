set_source_files_properties(
  Theme.qml
  Spacing.qml
  Typography.qml
  PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(q_browser_design
  STATIC
  NO_PLUGIN
  URI Company.Design
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  QML_FILES
    AppButton.qml
    AppCard.qml
    AppDialog.qml
    AppNavigation.qml
    AppStatusBadge.qml
    AppTable.qml
    AppTextField.qml
    AppToast.qml
    Spacing.qml
    StateView.qml
    Theme.qml
    Typography.qml)

target_link_libraries(q_browser_design
  PRIVATE
    Qt6::Quick
    Qt6::QuickControls2)
