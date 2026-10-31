import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Orbelio.Native 1.0

Rectangle {
    id: root
    width: 1200; height: 760
    color: "#0b1118"
    property bool playing: true
    property real sceneTime: 0
    property bool autoOrbit: false
    property real orbitTime: 0
    property real previousOrbitTime: 0
    readonly property real orbitDegreesPerSecond: 20
    readonly property real orbitDegreesPerPixel: 0.3
    readonly property real wheelUnitsPerStep: 120
    property double lastTick: Date.now()
    property var heldKeys: ({})
    readonly property color ink: "#e2e8ee"
    readonly property color muted: "#a7b2bf"
    readonly property color rule: "#33b2c4d5"
    property bool perfOpen: false
    property bool inspectorOpen: false
    property bool bookmarksOpen: false
    property string outputStatus: ""
    property bool outputPending: false
    readonly property int timelineHeight: appModel.timeline.active ? sequenceTimeline.height : 0
    readonly property bool moving: !!(heldKeys[Qt.Key_W] || heldKeys[Qt.Key_A] || heldKeys[Qt.Key_S] || heldKeys[Qt.Key_D] || heldKeys[Qt.Key_Q] || heldKeys[Qt.Key_E])
    onMovingChanged: lastTick = Date.now()

    function clearMovement() { heldKeys = ({}) }

    function movementKey(event, pressed) {
        const movement = [Qt.Key_W, Qt.Key_A, Qt.Key_S, Qt.Key_D, Qt.Key_Q, Qt.Key_E, Qt.Key_Shift]
        if (!viewportBridge.freeCamera || movement.indexOf(event.key) < 0)
            return false
        if (pressed && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            return false
        if (!event.isAutoRepeat) {
            const next = Object.assign({}, heldKeys)
            next[event.key] = pressed
            heldKeys = next
        }
        event.accepted = true
        root.autoOrbit = false
        return true
    }

    function switchCameraMode(free) {
        clearMovement()
        autoOrbit = false
        viewportBridge.freeCamera = free
        viewport.forceActiveFocus()
    }

    function moveCamera(seconds) {
        if (!moving || !viewport.activeFocus || !viewportBridge.freeCamera)
            return
        viewportBridge.moveCamera(Number(!!heldKeys[Qt.Key_D]) - Number(!!heldKeys[Qt.Key_A]),
                                  Number(!!heldKeys[Qt.Key_E]) - Number(!!heldKeys[Qt.Key_Q]),
                                  Number(!!heldKeys[Qt.Key_W]) - Number(!!heldKeys[Qt.Key_S]),
                                  seconds, !!heldKeys[Qt.Key_Shift])
    }

    Connections {
        target: root.Window.window
        function onActiveChanged() { if (!root.Window.window.active) root.clearMovement() }
    }
    Connections {
        target: viewportBridge
        function onCameraModeChanged() { root.clearMovement() }
    }

    onPlayingChanged: lastTick = Date.now()
    onAutoOrbitChanged: lastTick = Date.now()
    onOrbitTimeChanged: {
        viewportBridge.orbitCamera((orbitTime - previousOrbitTime) * orbitDegreesPerSecond, 0)
        previousOrbitTime = orbitTime
    }

    function fitCamera() {
        clearMovement()
        autoOrbit = false
        viewportBridge.fitCamera()
    }

    function resetCamera() {
        clearMovement()
        autoOrbit = false
        viewportBridge.resetCamera()
    }

    FrameAnimation {
        running: (root.playing && !appModel.timeline.active) || root.autoOrbit || root.moving
        onTriggered: {
            const now = Date.now()
            const seconds = (now - root.lastTick) / 1000
            if (root.playing && !appModel.timeline.active)
                root.sceneTime += seconds
            if (root.autoOrbit)
                root.orbitTime += seconds
            root.moveCamera(seconds)
            root.lastTick = now
        }
    }

    FileDialog {
        id: modelFileDialog
        objectName: "modelFileDialog"
        title: "打开 Gaussian 模型"
        nameFilters: ["Gaussian models (*.ply *.sog *.spz *.splat *.ksplat *.json *.gsseq *.fdu4d)", "FDU 4DGS (*.fdu4d)", "PLY Sequence (*.gsseq *.json)", "PLY (*.ply)", "SOG (*.sog *.json)", "SPZ (*.spz)", "SPLAT / KSPLAT (*.splat *.ksplat)", "All files (*)"]
        onVisibleChanged: if (visible) root.clearMovement()
        onAccepted: appModel.openFiles([selectedFile])
    }

    FileDialog {
        id: cameraExportDialog
        objectName: "cameraExportDialog"
        title: "导出相机参数"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["Camera parameters (*.json)"]
        onVisibleChanged: if (visible) root.clearMovement()
        onAccepted: { outputTimer.stop(); root.outputPending = true; cameraTools.exportCamera(selectedFile) }
    }
    FileDialog {
        id: screenshotDialog
        objectName: "screenshotDialog"
        title: "保存视口 PNG"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG image (*.png)"]
        onVisibleChanged: if (visible) root.clearMovement()
        onAccepted: { outputTimer.stop(); root.outputPending = true; cameraTools.saveScreenshot(selectedFile) }
    }

    Shortcut {
        sequences: [StandardKey.Open]
        enabled: !exportDialog.visible && !cameraExportDialog.visible && !screenshotDialog.visible && !modelFileDialog.visible
        onActivated: modelFileDialog.open()
    }
    Connections {
        target: cameraTools
        function onChanged() {
            if (!root.outputPending) return
            root.outputStatus = cameraTools.status
            if (!cameraTools.busy) {
                root.outputPending = false
                outputTimer.restart()
            }
        }
    }
    Timer { id: outputTimer; interval: 8000; onTriggered: root.outputStatus = "" }

    Viewport {
        id: viewport
        objectName: "viewport"
        anchors.fill: parent
        anchors.bottomMargin: root.timelineHeight
        sceneTime: root.sceneTime
        bridge: viewportBridge
        onActiveFocusChanged: if (!activeFocus) root.clearMovement()
        Keys.onReleased: event => root.movementKey(event, false)
        Keys.onPressed: event => {
            if (root.movementKey(event, true)) return
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return
            if (event.isAutoRepeat) { event.accepted = true; return }
            if (event.key === Qt.Key_V) root.switchCameraMode(!viewportBridge.freeCamera)
            else if (event.key === Qt.Key_Escape) { root.clearMovement(); focus = false }
            else if (event.key === Qt.Key_F) root.fitCamera()
            else if (event.key === Qt.Key_R) root.resetCamera()
            else if (appModel.timeline.active && event.key === Qt.Key_Space) appModel.timeline.playing = !appModel.timeline.playing
            else if (appModel.timeline.active && event.key === Qt.Key_Left) appModel.timeline.step(-1)
            else if (appModel.timeline.active && event.key === Qt.Key_Right) appModel.timeline.step(1)
            else return
            event.accepted = true
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            property point lastPosition
            property bool panning: false
            onPressed: mouse => {
                viewport.forceActiveFocus()
                root.autoOrbit = false
                lastPosition = Qt.point(mouse.x, mouse.y)
                panning = mouse.button === Qt.MiddleButton
                    || (mouse.button === Qt.RightButton && !viewportBridge.freeCamera)
                    || (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier) !== 0)
            }
            onPositionChanged: mouse => {
                if (!pressed) return
                const dx = mouse.x - lastPosition.x
                const dy = mouse.y - lastPosition.y
                lastPosition = Qt.point(mouse.x, mouse.y)
                if (panning) {
                    const extent = Math.max(1, Math.min(width, height))
                    viewportBridge.panCamera(dx / extent, dy / extent)
                } else viewportBridge.orbitCamera(-dx * root.orbitDegreesPerPixel, dy * root.orbitDegreesPerPixel)
            }
            onDoubleClicked: root.fitCamera()
            onWheel: wheel => {
                viewport.forceActiveFocus()
                root.autoOrbit = false
                const delta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y
                viewportBridge.zoomCamera(delta / root.wheelUnitsPerStep)
                wheel.accepted = true
            }
        }
    }
    Column {
        visible: !appModel.hasScene
        anchors.centerIn: parent
        spacing: 12
        BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: appModel.loading; visible: running }
        UiButton {
            visible: !appModel.loading
            text: "打开模型…"; glyph: "Cube"; outlined: true; height: 42
            onClicked: modelFileDialog.open()
        }
    }
    Rectangle {
        width: parent.width; height: 48; color: "#b0080e14"
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 26; anchors.rightMargin: 26; spacing: 20
            Text { text: "ORBELIO"; color: root.ink; font.pixelSize: 20; font.letterSpacing: 1.3 }
            Rectangle { width: 1; height: 17; color: "#59cddbe8" }
            UiButton {
                objectName: "openModelButton"
                text: "打开…"; glyph: "Cube"
                onClicked: modelFileDialog.open()
            }
            BusyIndicator { running: appModel.loading; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
            Item { Layout.fillWidth: true }
            UiButton {
                objectName: "exportButton"
                glyph: "ArrowSquareOut"; text: "导出"
                enabled: appModel.hasScene && !cameraTools.busy
                onClicked: { root.clearMovement(); appModel.timeline.playing = false; exportDialog.open() }
            }
            UiButton {
                objectName: "performanceButton"
                glyph: "Pulse"; width: 34; height: 34; circle: true
                selected: root.perfOpen; Accessible.name: "性能检查"
                onClicked: {
                    root.clearMovement()
                    root.perfOpen = !root.perfOpen || root.inspectorOpen
                    root.inspectorOpen = false
                }
            }
        }
    }
    Column {
        x: 26; y: 78; width: 190; spacing: 10
        RowLayout {
            width: parent.width
            UiButton {
                objectName: "sceneMenuButton"
                Layout.fillWidth: true
                text: appModel.modelName; glyph: "Cube"; alignLeft: true
                Accessible.name: "当前模型 " + appModel.modelName
                onClicked: sceneMenu.open()
                Menu {
                    id: sceneMenu
                    y: parent.height; width: 220
                    palette.window: "#19212b"; palette.base: "#19212b"
                    palette.text: "#e2e8ee"; palette.windowText: "#e2e8ee"
                    palette.buttonText: "#e2e8ee"; palette.highlight: "#394132"
                    palette.highlightedText: "#ddb263"
                    background: Rectangle { color: "#19212b"; border.color: "#41505d"; radius: 3 }
                    MenuItem { text: "打开模型…"; onTriggered: modelFileDialog.open() }
                    MenuItem {
                        objectName: "inspectModelButton"
                        text: "模型属性"; enabled: appModel.hasScene
                        onTriggered: { root.clearMovement(); root.inspectorOpen = true; root.perfOpen = true }
                    }
                    MenuItem {
                        objectName: "unloadModelButton"
                        text: "卸载模型"; enabled: appModel.hasScene || appModel.loading
                        onTriggered: { root.clearMovement(); appModel.unloadModel() }
                    }
                }
            }
        }
        Rectangle { width: parent.width; height: 1; color: root.rule }
        UiButton {
            objectName: "bookmarksButton"
            glyph: "Camera"; text: "场景相机"; alignLeft: true; height: 37; width: parent.width
            selected: root.bookmarksOpen && !root.perfOpen
            onClicked: {
                root.clearMovement()
                root.bookmarksOpen = !root.bookmarksOpen
                root.perfOpen = false
            }
        }
    }
    BookmarksPanel { editor: root }
    PerformancePanel { editor: root }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: root.perfOpen ? 345 : root.bookmarksOpen ? (root.width > 960 ? 325 : 303) : 26
        anchors.bottom: parent.bottom; anchors.bottomMargin: 24 + root.timelineHeight
        spacing: 12
        UiButton {
            objectName: "fitCameraButton"
            glyph: "FrameCorners"; Accessible.name: "适配场景"
            enabled: appModel.hasScene; onClicked: root.fitCamera()
        }
        UiButton {
            objectName: "resetCameraButton"
            glyph: "ArrowCounterClockwise"; Accessible.name: "重置相机"
            enabled: appModel.hasScene; onClicked: root.resetCamera()
        }
        ComboBox {
            id: cameraModeSelector
            objectName: "cameraModeSelector"
            width: 100; height: 32
            Accessible.name: "相机模式"
            model: ["环绕", "自由"]
            currentIndex: viewportBridge.freeCamera ? 1 : 0
            onActivated: root.switchCameraMode(currentIndex === 1)
            contentItem: Text {
                text: cameraModeSelector.displayText; color: "#ddb263"; font.pixelSize: 13
                leftPadding: 10; verticalAlignment: Text.AlignVCenter
            }
            indicator: Image {
                source: "assets/icons/CaretDown.png"; width: 16; height: 16
                x: parent.width - width - 8; y: (parent.height - height) / 2
            }
            background: Rectangle { color: "#b0111820"; radius: 3; border.color: cameraModeSelector.visualFocus ? "#ddb263" : "#41505d" }
            delegate: ItemDelegate {
                required property string modelData
                required property int index
                width: cameraModeSelector.width
                contentItem: Text { text: modelData; color: "#e2e8ee"; font.pixelSize: 13 }
                background: Rectangle { color: parent.highlighted ? "#394132" : "#19212b" }
                highlighted: cameraModeSelector.highlightedIndex === index
            }
            popup: Popup {
                y: -implicitHeight - 4; width: cameraModeSelector.width; padding: 1
                implicitHeight: contentItem.implicitHeight + 2
                contentItem: ListView {
                    clip: true; implicitHeight: contentHeight
                    model: cameraModeSelector.popup.visible ? cameraModeSelector.delegateModel : null
                    currentIndex: cameraModeSelector.highlightedIndex
                }
                background: Rectangle { color: "#19212b"; border.color: "#41505d"; radius: 3 }
            }
        }
        UiButton {
            objectName: "autoOrbitToggle"
            glyph: "ArrowClockwise"; Accessible.name: "自动环绕"
            selected: root.autoOrbit; checkable: true; checked: root.autoOrbit
            enabled: appModel.hasScene && !viewportBridge.freeCamera
            onToggled: root.autoOrbit = checked
        }
    }
    Rectangle {
        visible: appModel.loadState === "Error" || root.outputStatus !== ""
        anchors.left: parent.left; anchors.bottom: parent.bottom
        anchors.margins: 26
        anchors.bottomMargin: 26 + root.timelineHeight
        width: Math.min(statusText.implicitWidth + 28, root.width - 52)
        height: statusText.implicitHeight + 24
        radius: 4; color: "#ed19212b"; border.color: "#41505d"
        RowLayout {
            anchors.fill: parent; anchors.margins: 12
            Text {
                id: statusText
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                text: appModel.loadState === "Error" ? appModel.ioStatus : root.outputStatus
                color: root.ink; font.pixelSize: 12
            }
            UiButton {
                visible: appModel.loadState === "Error"
                text: "打开…"; onClicked: modelFileDialog.open()
            }
        }
    }
    ExportDialog {
        id: exportDialog
        editor: root
        onCameraRequested: cameraExportDialog.open()
        onScreenshotRequested: screenshotDialog.open()
    }
    SequenceTimeline { id: sequenceTimeline; editor: root }
    DropArea {
        id: modelDrop
        objectName: "modelDropArea"
        anchors.fill: parent
        onEntered: drag => { drag.accepted = drag.hasUrls }
        onDropped: drop => {
            if (drop.hasUrls) { root.clearMovement(); appModel.openFiles(drop.urls); drop.acceptProposedAction() }
        }
    }
    Rectangle {
        anchors.fill: parent; visible: modelDrop.containsDrag
        color: "#b00c1119"; border.color: "#ddb263"; border.width: 2
        Text { anchors.centerIn: parent; text: "打开模型"; color: root.ink; font.pixelSize: 22 }
    }
}
