import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: dialog
    required property var editor
    property bool png: false
    signal cameraRequested()
    signal screenshotRequested()
    objectName: "exportDialog"
    width: Math.min(420, editor.width - 48)
    height: content.implicitHeight + 48
    anchors.centerIn: parent
    modal: true
    focus: true
    padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: editor.clearMovement()
    background: Rectangle { color: "#f5111820"; border.color: "#30a1b4c6"; radius: 5 }
    Overlay.modal: Rectangle { color: "#7a000000" }
    ColumnLayout {
        id: content
        Accessible.name: "导出"
        width: parent.width
        spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.bottomMargin: 21
            Item { Layout.fillWidth: true }
            UiButton {
                glyph: "X"; Accessible.name: "关闭导出"; width: 23; height: 23; glyphSize: 18
                onClicked: dialog.close()
            }
        }
        Repeater {
            model: [false, true]
            UiButton {
                required property bool modelData
                objectName: modelData ? "selectScreenshotButton" : "selectCameraExportButton"
                Layout.fillWidth: true; Layout.preferredHeight: 48
                Layout.bottomMargin: modelData ? 20 : 9
                text: modelData ? "PNG" : "JSON"
                glyph: modelData ? "Camera" : "FloppyDisk"
                outlined: true; selected: dialog.png === modelData
                Accessible.name: modelData ? "视口 PNG" : "相机参数 JSON"
                onClicked: dialog.png = modelData
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            UiButton { text: "取消"; outlined: true; onClicked: dialog.close() }
            UiButton {
                objectName: "saveExportButton"
                text: "保存…"; primary: true
                enabled: appModel.hasScene && !cameraTools.busy && !appModel.timeline.busy
                onClicked: {
                    dialog.close()
                    if (dialog.png) dialog.screenshotRequested()
                    else dialog.cameraRequested()
                }
            }
        }
    }
}
