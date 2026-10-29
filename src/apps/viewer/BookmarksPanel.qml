import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var editor
    objectName: "bookmarksPanel"
    Accessible.name: "场景相机"
    visible: editor.bookmarksOpen && !editor.perfOpen
    width: editor.width > 960 ? 270 : 248
    height: Math.min(content.implicitHeight + 36, editor.height - y - 82 - editor.timelineHeight)
    anchors.right: parent.right
    anchors.rightMargin: 28
    y: 110
    color: "#f5111820"
    border.color: "#30a1b4c6"
    radius: 5
    ScrollView {
        anchors.fill: parent
        anchors.margins: 18
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            id: content
            width: panel.width - 36
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                UiButton {
                    glyph: "X"; width: 23; height: 23; glyphSize: 18
                    Accessible.name: "关闭场景相机"
                    onClicked: editor.bookmarksOpen = false
                }
            }
            UiButton {
                glyph: "BookmarkSimple"; text: "保存当前视角"; primary: true
                objectName: "saveBookmarkButton"
                Layout.fillWidth: true; Layout.topMargin: 8; Layout.preferredHeight: 34
                enabled: appModel.hasScene && cameraTools.bookmarks.length < 16
                onClicked: { editor.clearMovement(); cameraTools.saveBookmark() }
            }
            Image {
                visible: !cameraTools.bookmarks.length
                source: "assets/icons/Camera.png"
                Layout.preferredWidth: 24; Layout.preferredHeight: 24
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 22; Layout.bottomMargin: 12
            }
            Repeater {
                model: cameraTools.bookmarks
                RowLayout {
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    Layout.topMargin: index === 0 ? 8 : 0
                    UiButton {
                        objectName: index === 0 ? "restoreBookmarkButton" : "restoreBookmark-" + index
                        glyph: "Camera"; text: modelData; alignLeft: true
                        Layout.fillWidth: true; Layout.preferredHeight: 42
                        onClicked: {
                            editor.clearMovement()
                            editor.autoOrbit = false
                            cameraTools.restoreBookmark(index)
                        }
                    }
                    UiButton {
                        objectName: index === 0 ? "removeBookmarkButton" : "removeBookmark-" + index
                        glyph: "Trash"; Accessible.name: "删除 " + modelData; width: 28
                        onClicked: cameraTools.removeBookmark(index)
                    }
                }
            }
        }
    }
}
