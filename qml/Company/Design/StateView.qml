import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Control {
    id: root

    enum ViewState {
        Content,
        Loading,
        Empty,
        Error
    }

    property int viewState: StateView.Content
    property string loadingMessage: "Loading"
    property string emptyMessage: "No items"
    property string errorMessage: "Something went wrong"
    property string accessibleName: viewState === StateView.Loading ? loadingMessage
                                    : viewState === StateView.Empty ? emptyMessage
                                    : viewState === StateView.Error ? errorMessage : "Content"
    default property alias contentData: contentContainer.data
    readonly property bool loadingVisible: viewState === StateView.Loading
    readonly property bool emptyVisible: viewState === StateView.Empty
    readonly property bool errorVisible: viewState === StateView.Error

    Accessible.name: accessibleName
    Accessible.description: viewState === StateView.Error ? "Error" : ""
    Accessible.role: Accessible.Pane

    contentItem: Item {
        Item {
            id: contentContainer
            anchors.fill: parent
            visible: root.viewState === StateView.Content
        }

        ColumnLayout {
            id: loadingPanel
            anchors.centerIn: parent
            visible: root.viewState === StateView.Loading
            spacing: Spacing.sm

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: loadingPanel.visible
                Accessible.ignored: true
            }

            Text {
                text: root.loadingMessage
                color: Theme.textSecondary
                font.family: Typography.family
                font.pixelSize: Typography.body
            }
        }

        Text {
            id: emptyPanel
            anchors.centerIn: parent
            visible: root.viewState === StateView.Empty
            text: root.emptyMessage
            color: Theme.textSecondary
            font.family: Typography.family
            font.pixelSize: Typography.body
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Text {
            id: errorPanel
            anchors.centerIn: parent
            visible: root.viewState === StateView.Error
            text: root.errorMessage
            color: Theme.error
            font.family: Typography.family
            font.pixelSize: Typography.body
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
