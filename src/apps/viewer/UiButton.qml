import QtQuick
import QtQuick.Controls

Button {
    id: control
    property string glyph: ""
    property bool selected: false
    property bool primary: false
    property bool outlined: false
    property bool underlined: false
    property bool circle: false
    property bool alignLeft: false
    property bool glyphAfter: false
    property int textSize: 13
    property int glyphSize: 20
    implicitWidth: Math.max(30, (glyph ? glyphSize : 0) + textLabel.implicitWidth + (text && glyph ? 9 : 0) + (primary || outlined ? 28 : 8))
    implicitHeight: 32
    padding: 4
    hoverEnabled: true
    opacity: enabled ? 1 : 0.32
    Accessible.name: text || objectName
    background: Rectangle {
        color: control.primary ? "#d8ae63" : control.hovered || control.down ? "#14d3e1ed" : "transparent"
        border.width: control.circle || control.outlined || control.visualFocus ? 1 : 0
        border.color: control.circle || control.visualFocus ? "#bc9b62" : "#41505d"
        radius: control.circle ? width / 2 : 3
        Rectangle {
            visible: control.underlined && control.selected
            anchors.bottom: parent.bottom
            width: parent.width
            height: 2
            color: "#ddb263"
        }
    }
    contentItem: Item {
    Row {
        id: contents
        layoutDirection: control.glyphAfter ? Qt.RightToLeft : Qt.LeftToRight
        spacing: control.text && control.glyph ? 9 : 0
        anchors.verticalCenter: parent.verticalCenter
        x: control.alignLeft ? 0 : (parent.width - width) / 2
        Image {
            visible: control.glyph !== ""
            width: visible ? control.glyphSize : 0
            height: control.glyphSize
            anchors.verticalCenter: parent.verticalCenter
            source: control.glyph ? "assets/icons/" + control.glyph + (control.primary ? "-dark" : control.selected ? "-amber" : "") + ".png" : ""
            sourceSize: Qt.size(control.glyphSize * 2, control.glyphSize * 2)
        }
        Text {
            id: textLabel
            text: control.text
            width: Math.max(0, Math.min(implicitWidth, control.width - (control.primary || control.outlined ? 28 : 8) - (control.glyph ? control.glyphSize + (control.text ? 9 : 0) : 0)))
            elide: Text.ElideRight
            color: control.primary ? "#1c1913" : control.selected ? "#ddb263" : "#cbd3dd"
            font.family: "Microsoft YaHei UI"
            font.pixelSize: control.textSize
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    }
    ToolTip {
        visible: control.hovered && !control.text && control.Accessible.name !== ""
        text: control.Accessible.name
        delay: 500
        contentItem: Text { text: control.Accessible.name; color: "#e2e8ee"; font.pixelSize: 12 }
        background: Rectangle { color: "#19212b"; border.color: "#41505d"; radius: 3 }
    }
}
