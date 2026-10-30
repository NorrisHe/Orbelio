import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var editor
    property bool resources: false
    property var perf: appModel.performance
    readonly property var sequenceStats: appModel.timeline.statistics
    readonly property var timings: [
        ["CPU 完成帧", "completeFrameMs"], ["完成帧间隔", "completedIntervalMs"],
        ["CPU 引擎（含等待）", "engineBlockingMs"], ["CPU Qt 合成区间", "qtCompositionCpuSpanMs"],
        ["后端等待", "backendWaitMs"], ["Qt 完成等待", "qtCompletionWaitMs"],
        ["全部 GPU 等待", "allGpuWaitsMs"], ["查询读回", "queryReadbackCpuMs"],
        ["CPU 时间条件化", "temporalEvaluationCpuMs"],
        ["GPU 预处理", "gpuPreprocessMs"], ["GPU 深度排序", "gpuDepthSortMs"],
        ["GPU 光栅", "gpuRasterMs"], ["GPU 图像合成", "gpuImageComposeMs"],
        ["GPU GS 阶段合计", "gpuGsStagesMs"]
    ]
    readonly property var allocations: [
        ["模型", "Model buffers"], ["预处理", "Preprocess buffers"],
        ["排序临时资源", "Sort scratch"], ["像素与视口目标", "Pixel buffers + target"],
        ["统计与截图", "Statistics / capture"], ["GS 预留 / 扩容", "GS reserved / growths"],
        ["VMA 使用 / 预留", "VMA used / reserved"], ["VMA 使用峰值", "VMA used peak"],
        ["输入使用 / 预留", "Input active / reserved"], ["输入峰值 / 分配", "Input peak / allocations"],
        ["CPU 场景与队列", "CPU scenes + queued"], ["GPU 预算", "GPU allocation limit"],
        ["GS 分配 / 复用", "GS allocations / reuse"], ["图像创建 / 扩容", "Image creates / growths"]
    ]
    objectName: "performancePanel"
    Accessible.name: "性能检查"
    visible: editor.perfOpen
    width: 320
    anchors.right: parent.right
    y: 57
    height: editor.height - y - editor.timelineHeight
    color: "#f5111820"
    border.color: "#30a1b4c6"
    function metric(rows, key) { return (rows || {})[key] ?? "unavailable" }
    function sequenceTiming(key) {
        const value = sequenceStats[key]
        return value && value.samples > 0 ? Number(value.p50).toFixed(2) + " / " + Number(value.p95).toFixed(2) + " ms" : "不可用"
    }
    function cacheSize() {
        return sequenceStats.cacheBytes == null ? "不适用" : (sequenceStats.cacheBytes / 1048576).toFixed(1) + " / " + (sequenceStats.cacheCapacityBytes / 1048576).toFixed(1) + " MiB"
    }
    ScrollView {
        anchors.fill: parent; anchors.margins: 21
        contentWidth: availableWidth; clip: true
        ColumnLayout {
            width: panel.width - 42
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                UiButton {
                    objectName: "closePerformanceButton"
                    glyph: "X"; Accessible.name: "关闭性能面板"; width: 23; height: 23; glyphSize: 18
                    onClicked: editor.perfOpen = false
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 0
                UiButton { objectName: "inspectorTab"; text: "模型"; selected: editor.inspectorOpen; underlined: true; Layout.fillWidth: true; onClicked: editor.inspectorOpen = true }
                UiButton { objectName: "performanceTab"; text: "性能"; selected: !editor.inspectorOpen; underlined: true; Layout.fillWidth: true; onClicked: editor.inspectorOpen = false }
            }
            InspectorPanel { editor: panel.editor; Layout.fillWidth: true; visible: editor.inspectorOpen }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 16; visible: !editor.inspectorOpen
            Row {
                spacing: 10
                Text {
                    id: fpsValue
                    objectName: "fpsValue"
                    width: 150
                    text: panel.perf.fps == null ? "—" : Number(panel.perf.fps).toFixed(1)
                    Accessible.name: panel.perf.fps == null ? "FPS 不可用，尚无有效完成帧" : text + " FPS"
                    color: editor.ink; font.pixelSize: 40; font.weight: Font.Light
                }
                Text { objectName: "fpsUnit"; text: "FPS"; color: editor.muted; font.pixelSize: 12; anchors.baseline: fpsValue.baseline }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 0
                UiButton { text: "概览"; selected: !panel.resources; underlined: true; Layout.fillWidth: true; onClicked: panel.resources = false }
                UiButton { text: "资源"; selected: panel.resources; underlined: true; Layout.fillWidth: true; onClicked: panel.resources = true }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "每帧高斯上限"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                SpinBox {
                    objectName: "renderSplatBudget"
                    Accessible.name: "每帧高斯上限，零为不限"
                    implicitWidth: 140; implicitHeight: 32
                    font.pixelSize: 12; font.family: "Consolas"
                    Layout.preferredWidth: 140; Layout.minimumWidth: 140; Layout.maximumWidth: 140
                    from: 0; to: 8000000; stepSize: 50000; editable: true
                    palette.base: "#111820"; palette.text: editor.ink
                    palette.button: "#1a2430"; palette.buttonText: editor.ink
                    value: appModel.maxGaussians
                    textFromValue: function(value) { return value === 0 ? "不限" : String(value) }
                    valueFromText: function(text) { return text === "不限" ? 0 : Number(text) }
                    onValueModified: appModel.maxGaussians = value
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "每帧上传"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                SpinBox {
                    objectName: "renderUploadBatch"
                    Accessible.name: "每帧上传高斯数，零为不限"
                    implicitWidth: 140; implicitHeight: 32
                    font.pixelSize: 12; font.family: "Consolas"
                    Layout.preferredWidth: 140; Layout.minimumWidth: 140; Layout.maximumWidth: 140
                    from: 0; to: 8000000; stepSize: 32768; editable: true
                    palette.base: "#111820"; palette.text: editor.ink
                    palette.button: "#1a2430"; palette.buttonText: editor.ink
                    value: appModel.uploadGaussiansPerFrame
                    textFromValue: function(value) { return value === 0 ? "不限" : String(value) }
                    valueFromText: function(text) { return text === "不限" ? 0 : Number(text) }
                    onValueModified: appModel.uploadGaussiansPerFrame = value
                }
            }
            RowLayout {
                objectName: "renderBudgetProgress"
                Layout.fillWidth: true
                Text { text: panel.perf.refining ? "上传中" : "已提交"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                Text {
                    Layout.preferredWidth: 150; horizontalAlignment: Text.AlignRight
                    text: (panel.perf.submittedGaussians ?? 0) + " / " + (panel.perf.budgetedGaussians ?? 0)
                    Accessible.name: "已提交和目标高斯数"
                    color: editor.muted; font.pixelSize: 11; font.family: "Consolas"
                }
            }
            RowLayout {
                visible: !panel.resources
                Layout.fillWidth: true; Layout.topMargin: 8
                Text { text: "p50 / p95 · ms"; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                Text { text: (panel.perf.samples ?? 0) + " / 120 帧"; color: editor.muted; font.pixelSize: 11 }
            }
            Repeater {
                model: panel.resources ? panel.allocations : panel.timings
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Text { text: modelData[0]; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                    Text {
                        text: panel.metric(panel.resources ? panel.perf.resourceRows : panel.perf.timingRows, modelData[1])
                        color: editor.ink; font.pixelSize: 11; font.family: "Consolas"
                    }
                }
            }
            Rectangle { height: 1; Layout.fillWidth: true; color: editor.rule }
            Repeater {
                model: [["Gaussian", "gaussians"], ["可见", "visible"]]
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Text { text: modelData[0]; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true }
                    Text { text: panel.metric(panel.perf, modelData[1]); color: editor.ink; font.pixelSize: 11; font.family: "Consolas" }
                }
            }
            Text {
                Layout.fillWidth: true; Layout.topMargin: 6
                text: appModel.gpuName; wrapMode: Text.WordWrap
                color: editor.muted; font.pixelSize: 11
            }
            ColumnLayout {
                visible: appModel.timeline.active; Layout.fillWidth: true; spacing: 16
                Rectangle { height: 1; Layout.fillWidth: true; color: editor.rule }
                Repeater {
                    model: [["缓存使用 / 容量", panel.cacheSize()],
                            ["缓存命中 / 查询", panel.sequenceStats.cacheLookups == null ? "不适用" : panel.sequenceStats.cacheHits + " / " + panel.sequenceStats.cacheLookups],
                            ["Seek 可见 p50/p95", panel.sequenceTiming("seekFirstVisibleMs")],
                            ["Seek 就绪 p50/p95", panel.sequenceTiming("seekTargetReadyMs")],
                            ["Seek 有效样本", (panel.sequenceStats.seekTargetReadyMs?.samples ?? 0) + " / 120"],
                            ["读取队列", panel.sequenceTiming("workerQueueMs")],
                            ["文件读取", panel.sequenceTiming("readMs")],
                            ["解码与关联资源", panel.sequenceTiming("decodeMs")],
                            ["完成回调队列", panel.sequenceTiming("completionQueueMs")],
                            ["加载总耗时", panel.sequenceTiming("loadTotalMs")]]
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Text { text: modelData[0]; color: editor.muted; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                        Text {
                            text: modelData[1]; color: editor.ink; font.pixelSize: 11; font.family: "Consolas"
                            Layout.preferredWidth: 158; Layout.minimumWidth: 158; Layout.maximumWidth: 158
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
            }
        }
    }
}
