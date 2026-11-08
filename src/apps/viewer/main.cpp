#include "app_model.hpp"
#include "camera_tools.hpp"
#include "vulkan_host.hpp"
#include "window_placement.hpp"
#include <viewport_item.hpp>
#include <asset_reader.hpp>
#include <gui_executor.hpp>
#include <io_executor.hpp>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickView>
#include <QScreen>
#include <QThread>
#include <QSettings>
#include <algorithm>
#include <iostream>
#include <limits>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace {
constexpr int preferredWindowWidth = 1200;
constexpr int minimumWindowWidth = 800;
constexpr int preferredWindowHeight = 760;
constexpr int minimumWindowHeight = 500;
constexpr int screenMarginWidth = 80;
constexpr int screenMarginHeight = 100;

struct Options {
    bool validation = false;
    orbelio::ModelLimits limits{};
    orbelio::RenderSettings renderSettings;
    QString modelPath = QStringLiteral(":/samples/guitar/guitar.compressed.ply");
};

std::uint32_t gaussianOption(const QCommandLineParser& parser, const char* name, std::uint32_t fallback) {
    if (!parser.isSet(name)) {
        return fallback;
    }
    bool valid = false;
    const auto value = parser.value(name).toUInt(&valid);
    if (!valid || value > orbelio::ModelLimits{}.maxGaussians) {
        throw std::invalid_argument(std::string(name) + " must be between 0 and 8000000");
    }
    return value;
}

orbelio::RenderSettings renderSettings(const QCommandLineParser& parser) {
    const QSettings stored("Orbelio", "Viewer");
    const auto limit = std::uint32_t(orbelio::ModelLimits{}.maxGaussians);
    return {gaussianOption(
                parser, "splat-budget", std::min(stored.value("render/maxGaussians", 0).toUInt(), limit)),
            gaussianOption(parser,
                           "upload-gaussians-per-frame",
                           std::min(stored.value("render/uploadGaussiansPerFrame", 262144).toUInt(), limit))};
}

std::size_t budgetOption(const QCommandLineParser& parser, const char* name, std::size_t fallback) {
    if (!parser.isSet(name)) {
        return fallback;
    }
    bool valid = false;
    const auto value = parser.value(name).toULongLong(&valid);
    constexpr auto maximumBudgetMiB = 65536;
    if (!valid || !value || value > maximumBudgetMiB) {
        throw std::invalid_argument(std::string(name) + " must be between 1 and 65536 MiB");
    }
    return static_cast<std::size_t>(value) * orbelio::ModelLimits::mebibyte;
}

std::size_t availableMemoryBytes() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        throw std::runtime_error("Could not query available Windows memory");
    }
    return static_cast<std::size_t>(std::min<unsigned long long>(
        {status.ullAvailPhys, status.ullAvailPageFile, std::numeric_limits<std::size_t>::max()}));
#elif defined(_SC_AVPHYS_PAGES)
    const auto pages = sysconf(_SC_AVPHYS_PAGES);
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        return std::min(std::size_t(pages), maximum / std::size_t(pageSize)) * std::size_t(pageSize);
    }
#endif
    return std::numeric_limits<std::size_t>::max();
}

orbelio::ModelLimits modelLimits(const QCommandLineParser& parser) {
    orbelio::ModelLimits limits{};
    const auto automatic = std::numeric_limits<std::size_t>::max();
    limits.inputBytes = budgetOption(parser, "input-budget-mib", automatic);
    limits.decodedBytes = budgetOption(parser, "decode-budget-mib", automatic);
    limits.residentBytes = budgetOption(parser, "cpu-budget-mib", automatic);
    limits.gpuBytes = budgetOption(parser, "gpu-budget-mib", 0);
    const auto cacheHeadroom = std::min(availableMemoryBytes(), limits.residentBytes) / 2;
    limits.sequenceCacheBytes = budgetOption(parser, "sequence-cache-mib", cacheHeadroom);
    return limits;
}

Options parseOptions(QGuiApplication& app) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Orbelio Gaussian Splatting viewer.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"validation", "Require the Khronos validation layer."});
    parser.addOption({"model", "Gaussian model path (default: bundled PlayCanvas guitar).", "path"});
    parser.addOption(
        {"input-budget-mib", "Input pool limit in MiB (default: available system memory).", "MiB"});
    parser.addOption(
        {"decode-budget-mib", "Decode/workspace limit in MiB (default: available system memory).", "MiB"});
    parser.addOption({"cpu-budget-mib", "CPU scene limit in MiB (default: available system memory).", "MiB"});
    parser.addOption({"gpu-budget-mib", "GPU allocation limit in MiB (default: driver budget).", "MiB"});
    parser.addOption(
        {"sequence-cache-mib", "Sequence cache limit in MiB (default: based on available memory).", "MiB"});
    parser.addOption(
        {"splat-budget", "Maximum Gaussians submitted per frame; 0 keeps full quality.", "count"});
    parser.addOption({"upload-gaussians-per-frame", "GPU upload batch; 0 uploads all.", "count"});
    parser.process(app);
    Options options{};
    options.validation = parser.isSet("validation");
    options.limits = modelLimits(parser);
    options.renderSettings = renderSettings(parser);
    if (parser.isSet("model")) {
        options.modelPath = parser.value("model");
    }
    return options;
}

void configureApplication(QGuiApplication& app) {
    app.setApplicationName("Orbelio");
    app.setApplicationVersion("0.1.0");
    QThread::currentThread()->setObjectName("Orbelio.GUI");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<orbelio::ViewportItem>("Orbelio.Native", 1, 0, "Viewport");
    qmlRegisterUncreatableType<orbelio::ViewportBridge>(
        "Orbelio.Native", 1, 0, "ViewportBridge", "Created by the application.");
}

void attachWindow(QQuickView& view, orbelio::AppModel& model, orbelio::VulkanHost& host) {
    view.setTitle("Orbelio");
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    host.attach(view);
    model.setGpuName(host.deviceName());
}

void connectViewport(orbelio::AppModel& model, orbelio::ViewportBridge& bridge) {
    QObject::connect(
        &model, &orbelio::AppModel::frameRequested, &bridge, &orbelio::ViewportBridge::requestFrame);
    QObject::connect(
        &bridge, &orbelio::ViewportBridge::frameCompleted, &model, &orbelio::AppModel::acceptFrame);
    QObject::connect(
        &model, &orbelio::AppModel::sceneChanged, &bridge, &orbelio::ViewportBridge::refreshCamera);
}

QSize windowSize(const QQuickView& view) {
    const auto available = view.screen()->availableGeometry().size();
    return {std::clamp(available.width() - screenMarginWidth, minimumWindowWidth, preferredWindowWidth),
            std::clamp(available.height() - screenMarginHeight, minimumWindowHeight, preferredWindowHeight)};
}

void showWindow(QQuickView& view,
                orbelio::AppModel& model,
                orbelio::ViewportBridge& bridge,
                const QString& path,
                orbelio::CameraTools& cameraTools) {
    const auto size = windowSize(view);
    view.resize(size);
    view.rootContext()->setContextProperty("appModel", &model);
    view.rootContext()->setContextProperty("viewportBridge", &bridge);
    view.rootContext()->setContextProperty("cameraTools", &cameraTools);
    view.setSource(QUrl("qrc:/qt/qml/Orbelio/Main.qml"));
    if (view.status() == QQuickView::Error) {
        throw std::runtime_error("Could not load Main.qml");
    }
    view.resize(size);
    view.show();
    orbelio::centerWindow(view);
    model.loadModel(path);
}

void releaseWindow(QQuickView& view) {
    // Detach the QML object before its context model is destroyed.
    view.setSource(QUrl());
    view.hide();
    view.releaseResources();
}

int runViewer(QGuiApplication& app, const Options& options) {
    // Declaration order guarantees: Window -> Model/IO -> Device -> Instance.
    orbelio::VulkanHost host(options.validation);
    orbelio::BytePool cpuPool{
        options.limits.inputBytes, orbelio::IAssetStream::blockBytes, availableMemoryBytes};
    orbelio::QtAssetReader reader;
    orbelio::QtGuiExecutor completions;
    orbelio::QtIoExecutor ioExecutor(16, options.limits.inputBytes);
    orbelio::SceneLoader loader(ioExecutor,
                                completions,
                                reader,
                                cpuPool,
                                options.limits,
                                orbelio::SceneLoader::Clock::now,
                                availableMemoryBytes);
    orbelio::ViewportState viewport;
    viewport.setRenderSettings(options.renderSettings);
    orbelio::AppModel model(loader, viewport, cpuPool);
    QObject::connect(&model, &orbelio::AppModel::renderSettingsChanged, &model, [&viewport] {
        QSettings stored("Orbelio", "Viewer");
        stored.setValue("render/maxGaussians", viewport.renderSettings().maxGaussians);
        stored.setValue("render/uploadGaussiansPerFrame", viewport.renderSettings().uploadGaussiansPerFrame);
    });
    // The bridge must outlive the window's render-thread shutdown.
    std::unique_ptr<orbelio::ViewportBridge> bridge;
    std::unique_ptr<orbelio::CameraTools> cameraTools;
    QQuickView view;
    attachWindow(view, model, host);
    bridge = std::make_unique<orbelio::ViewportBridge>(
        viewport,
        std::make_shared<orbelio::VulkanRendererFactory>(
            host.context(),
            options.limits.gpuBytes,
            (QCoreApplication::applicationDirPath() + "/shaders").toStdString()));
    cameraTools = std::make_unique<orbelio::CameraTools>(viewport, *bridge, ioExecutor, completions);
    QObject::connect(
        &model, &orbelio::AppModel::sceneChanged, cameraTools.get(), &orbelio::CameraTools::sceneChanged);
    connectViewport(model, *bridge);
    showWindow(view, model, *bridge, options.modelPath, *cameraTools);
    const int result = app.exec();
    releaseWindow(view);
    return result;
}
}

int main(int argc, char** argv) {
    qputenv("QSG_RENDER_LOOP", "threaded");
    QGuiApplication app(argc, argv);
    configureApplication(app);
    try {
        const auto options = parseOptions(app);
        return runViewer(app, options);
    } catch (const std::exception& error) {
        std::cerr << "Orbelio startup failed: " << error.what() << '\n';
        return 1;
    }
}
