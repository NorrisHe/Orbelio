import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel
    required property var editor
    readonly property var info: appModel.inspector
    readonly property bool rangeEditable: appModel.attributeView === 1 || appModel.attributeView === 3
    property bool invalidRange: false
    spacing: 16
    objectName: "inspectorPanel"
    function vector(value) { return value ? value.map(x => Number(x).toPrecision(5)).join("  ") : "不可用" }
    function applyRange() {
        if (!rangeEditable) { invalidRange = false; return }
        if (!rangeMinimum.text.trim() || !rangeMaximum.text.trim()) { invalidRange = true; return }
        invalidRange = !appModel.setAttributeRange(Number(rangeMinimum.text), Number(rangeMaximum.text))
        if (!invalidRange) syncRange()
    }
    function syncRange() {
        rangeMinimum.text = Number(appModel.attributeMinimum).toPrecision(5)
        rangeMaximum.text = Number(appModel.attributeMaximum).toPrecision(5)
        invalidRange = false
    }
    Component.onCompleted: syncRange()
    Connections { target: appModel; function onRenderSettingsChanged() { panel.syncRange() } }
    Repeater {
        model: [["名称", panel.info.name], ["格式", panel.info.format], ["表示", panel.info.representation],
                ["Gaussian", panel.info.gaussians], ["SH 阶数", panel.info.shDegree]]
        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            Text { text: modelData[0]; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
            Text {
                text: modelData[1] ?? "不可用"
                Layout.preferredWidth: 166; Layout.minimumWidth: 166; Layout.maximumWidth: 166
                horizontalAlignment: Text.AlignRight; elide: Text.ElideMiddle
                color: editor.ink; font.pixelSize: 11; font.family: "Consolas"
                Accessible.name: modelData[0] + " " + text
            }
        }
    }
    Text {
        objectName: "inspectorPath"
        Layout.fillWidth: true
        text: panel.info.path ?? "不可用"; elide: Text.ElideMiddle
        color: editor.muted; font.pixelSize: 11
        Accessible.name: text
        HoverHandler { id: pathHover }
        ToolTip.visible: pathHover.hovered
        ToolTip.text: text
    }
    ColumnLayout {
        visible: panel.info.frameIndex != null
        Layout.fillWidth: true; spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { text: "已确认帧"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
            Text {
                objectName: "inspectorFrame"
                Layout.preferredWidth: 166; Layout.minimumWidth: 166; Layout.maximumWidth: 166
                text: (panel.info.frameIndex ?? "—") + " · " + Number(panel.info.frameTime ?? 0).toFixed(3) + " s"
                horizontalAlignment: Text.AlignRight; color: editor.ink; font.pixelSize: 11; font.family: "Consolas"
            }
        }
        Text {
            Layout.fillWidth: true; text: panel.info.framePath ?? ""; elide: Text.ElideMiddle
            color: editor.muted; font.pixelSize: 11; Accessible.name: text
        }
    }
    Rectangle { Layout.fillWidth: true; height: 1; color: editor.rule }
    Repeater {
        model: [["中心最小", panel.info.boundsMinimum], ["中心最大", panel.info.boundsMaximum]]
        ColumnLayout {
            required property var modelData
            Layout.fillWidth: true; spacing: 7
            Text { text: modelData[0]; color: editor.muted; font.pixelSize: 11 }
            Text {
                Layout.fillWidth: true; text: panel.vector(modelData[1]); horizontalAlignment: Text.AlignRight
                color: editor.ink; font.pixelSize: 11; font.family: "Consolas"
                Accessible.name: modelData[0] + " " + text + (modelData[1] ? "" : " " + (panel.info.boundsUnavailableReason ?? ""))
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Text { text: "跨帧 ID"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
        Text { text: "不支持"; color: editor.ink; font.pixelSize: 11 }
    }
    Rectangle { Layout.fillWidth: true; height: 1; color: editor.rule }
    ComboBox {
        objectName: "attributeViewCombo"
        Accessible.name: "属性可视化"
        Layout.fillWidth: true; implicitHeight: 34
        enabled: panel.info.name != null
        model: ["正常", "深度", "不透明度", "尺度"]
        currentIndex: appModel.attributeView
        onActivated: appModel.attributeView = currentIndex
        palette.base: "#111820"; palette.text: editor.ink; palette.button: "#1a2430"; palette.buttonText: editor.ink
    }
    RowLayout {
        objectName: "attributeRangeRow"
        Layout.fillWidth: true; spacing: 8
        Repeater {
            model: ["最小", "最大"]
            Text { required property string modelData; text: modelData; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
        }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        TextField {
            id: rangeMinimum; objectName: "attributeMinimumInput"; Accessible.name: "可视化范围最小值"
            Layout.preferredWidth: 135; Layout.minimumWidth: 135; Layout.maximumWidth: 135; implicitHeight: 32
            enabled: panel.rangeEditable; selectByMouse: true; color: editor.ink; font.family: "Consolas"; font.pixelSize: 11
            opacity: enabled ? 1 : 0.4
            horizontalAlignment: Text.AlignRight; onEditingFinished: panel.applyRange()
            background: Rectangle { color: "#111820"; border.color: panel.invalidRange ? "#dc746a" : editor.rule; radius: 2 }
        }
        TextField {
            id: rangeMaximum; objectName: "attributeMaximumInput"; Accessible.name: "可视化范围最大值"
            Layout.preferredWidth: 135; Layout.minimumWidth: 135; Layout.maximumWidth: 135; implicitHeight: 32
            enabled: panel.rangeEditable; selectByMouse: true; color: editor.ink; font.family: "Consolas"; font.pixelSize: 11
            opacity: enabled ? 1 : 0.4
            horizontalAlignment: Text.AlignRight; onEditingFinished: panel.applyRange()
            background: Rectangle { color: "#111820"; border.color: panel.invalidRange ? "#dc746a" : editor.rule; radius: 2 }
        }
    }
    Text {
        visible: panel.invalidRange; Layout.fillWidth: true
        text: appModel.attributeView === 3 ? "范围须大于零，且最小值小于最大值" : "最小值须小于最大值"
        color: "#dc746a"; font.pixelSize: 11; wrapMode: Text.WordWrap
    }
}
