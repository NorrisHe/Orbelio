import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var editor
    property var timeline: appModel.timeline
    objectName: "sequenceTimeline"
    visible: timeline.active
    height: 100
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    color: "#f5111820"
    function displayTime(seconds) {
        return Math.abs(seconds) < 10000000 ? seconds.toFixed(6) : seconds.toExponential(6)
    }
    Rectangle { width: parent.width; height: 1; color: editor.rule }
    Item {
        objectName: "sequenceControlRow"
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.leftMargin: 26; anchors.rightMargin: 26; anchors.topMargin: 12
        height: 32
        Row {
            objectName: "sequenceTransport"
            anchors.left: parent.left
            height: 32; spacing: 10
            UiButton {
                objectName: "sequencePreviousButton"
                width: 30; height: 32
                glyph: "SkipBack"; Accessible.name: "上一帧"
                enabled: timeline.frameCount > 0; onClicked: timeline.step(-1)
            }
            UiButton {
                objectName: "sequencePlayButton"
                width: 30; height: 32
                glyph: timeline.playing ? "Pause" : "Play"
                Accessible.name: timeline.playing ? "暂停" : "播放"
                selected: timeline.playing
                onClicked: timeline.playing = !timeline.playing
            }
            UiButton {
                objectName: "sequenceNextButton"
                width: 30; height: 32
                glyph: "SkipForward"; Accessible.name: "下一帧"
                enabled: timeline.frameCount > 0; onClicked: timeline.step(1)
            }
            UiButton {
                objectName: "sequenceLoopButton"
                width: 30; height: 32
                glyph: "ArrowClockwise"; Accessible.name: "循环播放"
                checkable: true; checked: timeline.looping; selected: checked
                onToggled: timeline.looping = checked
            }
        }
        Row {
            objectName: "sequenceReadout"
            anchors.right: parent.right
            width: 522; height: 32; spacing: 10
            TextField {
                id: requestedField
                objectName: "sequenceTimeField"
                Accessible.name: "请求时间（秒）"
                width: 184; height: 30; anchors.verticalCenter: parent.verticalCenter
                property bool userEdited: false
                color: editor.ink; font.pixelSize: 12; font.family: "Consolas"
                horizontalAlignment: TextInput.AlignRight
                selectByMouse: true
                validator: DoubleValidator { bottom: bar.timeline.startTime; top: bar.timeline.endTime; notation: DoubleValidator.StandardNotation; locale: "C" }
                background: Rectangle { color: "#111820"; border.color: requestedField.activeFocus ? "#ddb263" : "#41505d"; radius: 3 }
                Binding {
                    target: requestedField; property: "text"
                    value: requestedField.activeFocus ? String(bar.timeline.requestedTime) : bar.displayTime(bar.timeline.requestedTime)
                    when: !requestedField.userEdited
                    restoreMode: Binding.RestoreNone
                }
                onTextEdited: userEdited = true
                onEditingFinished: {
                    const value = Number(text)
                    const commit = userEdited && text.trim() && Number.isFinite(value)
                    userEdited = false
                    if (commit) bar.timeline.seek(value)
                    focus = false
                }
            }
            Text { width: 8; anchors.verticalCenter: parent.verticalCenter; text: "s"; color: editor.muted; font.pixelSize: 12 }
            Item {
                objectName: "sequenceBusySlot"
                width: 22; height: 22; anchors.verticalCenter: parent.verticalCenter
                BusyIndicator { objectName: "sequenceBusyIndicator"; anchors.fill: parent; running: timeline.busy; visible: running }
            }
            Row {
                objectName: "sequenceDisplayedFrame"
                width: 278; height: 32; spacing: 6
                property string text: "→ " + String(timeline.frameTime) + " s · " + (timeline.frameIndex + 1) + " / " + timeline.frameCount
                Accessible.name: "实际帧 " + (timeline.frameIndex + 1) + "，时间 " + timeline.frameTime + " 秒"
                Text { width: 10; anchors.verticalCenter: parent.verticalCenter; text: "→"; color: editor.muted; font.pixelSize: 12 }
                Text {
                    objectName: "sequenceActualTime"
                    width: 112; anchors.verticalCenter: parent.verticalCenter
                    text: bar.displayTime(timeline.frameTime)
                    horizontalAlignment: Text.AlignRight
                    color: editor.muted; font.pixelSize: 12; font.family: "Consolas"
                }
                Text { width: 8; anchors.verticalCenter: parent.verticalCenter; text: "s"; color: editor.muted; font.pixelSize: 12 }
                Text { width: 8; anchors.verticalCenter: parent.verticalCenter; text: "·"; color: editor.muted; font.pixelSize: 12 }
                Text {
                    objectName: "sequenceActualIndex"
                    width: 48; anchors.verticalCenter: parent.verticalCenter
                    text: timeline.frameIndex + 1; horizontalAlignment: Text.AlignRight
                    color: editor.muted; font.pixelSize: 12; font.family: "Consolas"
                }
                Text { width: 8; anchors.verticalCenter: parent.verticalCenter; text: "/"; color: editor.muted; font.pixelSize: 12 }
                Text {
                    objectName: "sequenceFrameCount"
                    width: 48; anchors.verticalCenter: parent.verticalCenter
                    text: timeline.frameCount; horizontalAlignment: Text.AlignRight
                    color: editor.muted; font.pixelSize: 12; font.family: "Consolas"
                }
            }
        }
    }
    Slider {
            id: seekSlider
            objectName: "sequenceSeekSlider"
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 26; anchors.rightMargin: 26; anchors.bottomMargin: 12
            height: 26
            Accessible.name: "序列时间"
            from: timeline.startTime; to: timeline.endTime
            value: timeline.requestedTime
            onMoved: timeline.seek(value)
            background: Rectangle {
                x: seekSlider.leftPadding; y: seekSlider.topPadding + seekSlider.availableHeight / 2 - 1
                width: seekSlider.availableWidth; height: 2; color: "#41505d"
                Rectangle {
                    x: (bar.timeline.frameTime - seekSlider.from) / Math.max(0.000001, seekSlider.to - seekSlider.from) * parent.width - 1
                    y: -5; width: 2; height: 12; color: "#e2e8ee"
                }
            }
            handle: Rectangle {
                x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                width: 9; height: 18; radius: 2; color: "#ddb263"
            }
    }
    Rectangle {
        visible: bar.timeline.error !== ""
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.top; anchors.margins: 12
        height: 34; radius: 3; color: "#f519212b"; border.color: "#41505d"
        Text {
            anchors.fill: parent; anchors.margins: 8
            text: bar.timeline.error; color: "#ec9d91"; elide: Text.ElideMiddle; font.pixelSize: 12
        }
    }
}
