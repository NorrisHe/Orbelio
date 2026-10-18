import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Orbelio.Native 1.0

Rectangle {
    id: root
    width: 1120; height: 680
    color: "#0c1119"
    property bool playing: true
    property real sceneTime: 0
    property bool autoOrbit: false
    property real orbitTime: 0
    property real previousOrbitTime: 0
    readonly property real orbitDegreesPerSecond: 20
    readonly property real orbitDegreesPerPixel: 0.3
    readonly property real wheelUnitsPerStep: 120
    property double lastTick: Date.now()
    property var perf: appModel.performance

    onPlayingChanged: lastTick = Date.now()
    onAutoOrbitChanged: lastTick = Date.now()
    onOrbitTimeChanged: {
        viewportBridge.orbitCamera((orbitTime - previousOrbitTime) * orbitDegreesPerSecond, 0)
        previousOrbitTime = orbitTime
    }

    function fitCamera() {
        autoOrbit = false
        viewportBridge.fitCamera()
    }

    function resetCamera() {
        autoOrbit = false
        viewportBridge.resetCamera()
    }

    FrameAnimation {
        running: root.playing || root.autoOrbit
        onTriggered: {
            const now = Date.now()
            const seconds = (now - root.lastTick) / 1000
            if (root.playing)
                root.sceneTime += seconds
            if (root.autoOrbit)
                root.orbitTime += seconds
            root.lastTick = now
        }
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 1
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 64; color: "#141c28"
            RowLayout {
                anchors.fill: parent; anchors.margins: 18; spacing: 16
                Text { text: "ORBELIO"; color: "#e8eff9"; font.pixelSize: 21; font.letterSpacing: 3; font.bold: true }
                Rectangle { width: 1; height: 24; color: "#344154" }
                Text { text: "Static GS / Integration Preview"; color: "#9cacc3"; font.pixelSize: 13 }
                Item { Layout.fillWidth: true }
                Text { text: "VULKAN 1.3"; color: "#70d6bc"; font.pixelSize: 11; font.letterSpacing: 1 }
                Text { text: appModel.validation ? "VALIDATION ON" : "VALIDATION OFF"; color: "#9cacc3"; font.pixelSize: 11 }
            }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 1
            Rectangle {
                Layout.preferredWidth: 210; Layout.fillHeight: true; color: "#111925"
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 18; spacing: 16
                    Text { text: "SCENE"; color: "#8c9bb0"; font.pixelSize: 11; font.letterSpacing: 2 }
                    Text { text: root.perf.gaussians > 0 ? "Guitar / 3DGS" : "Loading model..."; color: "#d9e3f0"; font.pixelSize: 14 }
                    Text { Layout.fillWidth: true; text: "PlayCanvas sample · SH0\nLeft drag: orbit\nRight / middle / Shift+left: pan\nWheel: zoom · Double-click / F: fit\nR: reset camera"; wrapMode: Text.WordWrap; color: "#718299"; font.pixelSize: 12 }
                    RowLayout {
                        Button { objectName: "fitCameraButton"; text: "Fit"; onClicked: root.fitCamera() }
                        Button { objectName: "resetCameraButton"; text: "Reset"; onClicked: root.resetCamera() }
                    }
                    CheckBox { objectName: "autoOrbitToggle"; text: "Auto orbit"; checked: root.autoOrbit; onToggled: root.autoOrbit = checked }
                    Rectangle { Layout.fillWidth: true; height: 1; color: "#263346" }
                    Text { text: "EXECUTION"; color: "#8c9bb0"; font.pixelSize: 11; font.letterSpacing: 2 }
                    Repeater {
                        model: ["01  GUI / Main", "02  Qt Render / Engine", "03  IO / Pooled worker"]
                        delegate: Text { required property string modelData; text: modelData; color: "#b0c1d6"; font.pixelSize: 12 }
                    }
                    Text { Layout.fillWidth: true; text: appModel.ioStatus; color: "#70d6bc"; font.pixelSize: 11; wrapMode: Text.WordWrap }
                    Item { Layout.fillHeight: true }
                    Text { Layout.fillWidth: true; text: "Temporary LichtFeld backend\nGPL-3.0-or-later · rewrite TODO"; wrapMode: Text.WordWrap; color: "#657992"; font.pixelSize: 10 }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: "#101b29"; clip: true
                Viewport {
                    id: viewport
                    objectName: "viewport"
                    anchors.fill: parent
                    sceneTime: root.sceneTime
                    bridge: viewportBridge
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_F) {
                            root.fitCamera()
                            event.accepted = true
                        } else if (event.key === Qt.Key_R) {
                            root.resetCamera()
                            event.accepted = true
                        }
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
                            panning = mouse.button !== Qt.LeftButton || (mouse.modifiers & Qt.ShiftModifier) !== 0
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return
                            const dx = mouse.x - lastPosition.x
                            const dy = mouse.y - lastPosition.y
                            lastPosition = Qt.point(mouse.x, mouse.y)
                            if (panning) {
                                const extent = Math.max(1, Math.min(width, height))
                                viewportBridge.panCamera(dx / extent, dy / extent)
                            } else {
                                viewportBridge.orbitCamera(-dx * root.orbitDegreesPerPixel, dy * root.orbitDegreesPerPixel)
                            }
                        }
                        onDoubleClicked: root.fitCamera()
                        onWheel: wheel => {
                            viewport.forceActiveFocus()
                            root.autoOrbit = false
                            const steps = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y / root.wheelUnitsPerStep
                                                                  : wheel.angleDelta.y / root.wheelUnitsPerStep
                            viewportBridge.zoomCamera(steps)
                            wheel.accepted = true
                        }
                    }
                }
                Column {
                    visible: !(root.perf.gaussians > 0)
                    anchors.centerIn: parent; spacing: 14
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "+"; color: "#6bbfae"; font.pixelSize: 44; font.weight: Font.Light }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Native Vulkan viewport"; color: "#c6d8e9"; font.pixelSize: 20 }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Engine VkImage → Qt Quick composition"; color: "#819ab0"; font.pixelSize: 12 }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Preparing model and compute pipelines..."; color: "#59768e"; font.pixelSize: 11 }
                }
                Text { anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 18; text: "VIEWPORT  /  GPU ONLY"; color: "#6d92a6"; font.pixelSize: 10; font.letterSpacing: 1 }
            }
            Rectangle {
                Layout.preferredWidth: 350; Layout.fillHeight: true; color: "#111925"
                ScrollView {
                    anchors.fill: parent; anchors.margins: 14; clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width; spacing: 8
                        Text { text: "PERFORMANCE / A6"; color: "#8c9bb0"; font.pixelSize: 11; font.letterSpacing: 2 }
                        Text { Layout.fillWidth: true; text: appModel.gpuName; color: "#d9e3f0"; font.pixelSize: 13; wrapMode: Text.WordWrap }
                        Text { text: root.perf.fps == null ? "Warming up..." : Number(root.perf.fps).toFixed(1) + " FPS"; color: "#70d6bc"; font.pixelSize: 24 }
                        Text { Layout.fillWidth: true; text: "Last 120 completed frames / " + (root.perf.samples ?? 0) + " samples\n30 scene frames excluded for warmup. p50 / p95 (ms). UI + counters: 4 Hz."; wrapMode: Text.WordWrap; color: "#8294ad"; font.pixelSize: 11 }
                        Repeater {
                            model: [["CPU complete", "completeFrameMs"], ["Completion interval", "completedIntervalMs"],
                                    ["CPU engine (blocking)", "engineBlockingMs"], ["CPU Qt span", "qtCompositionCpuSpanMs"],
                                    ["CPU backend waits", "backendWaitMs"], ["CPU Qt fence wait", "qtCompletionWaitMs"],
                                    ["CPU all GPU waits", "allGpuWaitsMs"], ["CPU query readback", "queryReadbackCpuMs"], ["GPU preprocess", "gpuPreprocessMs"],
                                    ["GPU depth sort", "gpuDepthSortMs"], ["GPU raster + tile sort", "gpuRasterAndTileSortMs"],
                                    ["GPU image compose", "gpuImageComposeMs"], ["GPU GS stage sum", "gpuGsStagesMs"]]
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Text { text: modelData[0]; color: "#8294ad"; font.pixelSize: 10; Layout.fillWidth: true }
                                Text { text: root.perf.timingRows ? (root.perf.timingRows[modelData[1]] ?? "Unavailable") : "Unavailable"; color: "#cfdeed"; font.pixelSize: 11; font.family: "Consolas" }
                            }
                        }
                        Text { Layout.fillWidth: true; text: "CPU: engine includes backend waits; complete frame ends at Qt queue fence. FPS uses completion intervals, including idle. GPU: timestamps; raster includes tile sort + counters. Image compose separate."; wrapMode: Text.WordWrap; color: "#657992"; font.pixelSize: 10 }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#263346" }
                        Repeater {
                            model: ["Total / visible", "Tile references", "Max tile list (all waves)", "Depth waves"]
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                Text { text: modelData; color: "#8294ad"; font.pixelSize: 11; Layout.fillWidth: true }
                                Text { text: modelData === "Total / visible" ? (root.perf.gaussians ?? "Unavailable") + " / " + (root.perf.visible ?? "Unavailable")
                                      : modelData === "Tile references" ? String(root.perf.tileInstances ?? "Unavailable")
                                      : modelData === "Max tile list (all waves)" ? String(root.perf.maxTile ?? "Unavailable")
                                      : String(root.perf.depthWaves ?? "Unavailable"); color: "#cfdeed"; font.pixelSize: 11; font.family: "Consolas" }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#263346" }
                        Repeater {
                            model: ["Model buffers", "Preprocess buffers", "Sort / tile scratch", "Pixel buffers + target",
                                    "Statistics / capture", "GS reserved / growths", "VMA used / reserved", "VMA used peak",
                                    "CPU active / reserved", "CPU peak / allocations", "GS allocations / reuse", "Image creates / growths"]
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                Text { text: modelData; color: "#8294ad"; font.pixelSize: 10; Layout.fillWidth: true }
                                Text { text: root.perf.resourceRows ? (root.perf.resourceRows[modelData] ?? "Unavailable") : "Unavailable"; color: "#cfdeed"; font.pixelSize: 10; font.family: "Consolas" }
                            }
                        }
                        Text { Layout.fillWidth: true; text: "Owned capacities count aliases once. VMA used / reserved are allocations / blocks; Qt and driver excluded. CPU pool budget: 8 MiB. GPU budget: unavailable (not enforced)."; wrapMode: Text.WordWrap; color: "#657992"; font.pixelSize: 10 }
                        Text { Layout.fillWidth: true; text: "Unavailable: temporal Evaluate, sequence cache, Seek-to-visible, Qt GPU composition."; wrapMode: Text.WordWrap; color: "#c6a572"; font.pixelSize: 10 }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 76; color: "#141c28"
            RowLayout {
                anchors.fill: parent; anchors.margins: 18; spacing: 18
                Button { text: root.playing ? "Pause" : "Play"; onClicked: root.playing = !root.playing }
                Text { text: "t = " + root.sceneTime.toFixed(3) + " s"; color: "#c9d8ea"; font.family: "Consolas"; font.pixelSize: 13 }
                Slider { Layout.fillWidth: true; from: 0; to: 60; value: root.sceneTime % 60; onMoved: { root.playing = false; root.sceneTime = value } }
                Text { text: "STATIC SCENE CLOCK"; color: "#657992"; font.pixelSize: 10; font.letterSpacing: 1 }
            }
        }
    }
}
