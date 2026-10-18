#include "app_model.hpp"
#include "vulkan_host.hpp"
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
#include <algorithm>
#include <iostream>

namespace {
constexpr int preferredWindowWidth = 1200;
constexpr int minimumWindowWidth = 800;
constexpr int preferredWindowHeight = 760;
constexpr int minimumWindowHeight = 500;
constexpr int screenMarginWidth = 80;
constexpr int screenMarginHeight = 100;

struct Options {
    bool validation = false;
    QString modelPath = QStringLiteral(":/samples/guitar/guitar.compressed.ply");
};

Options parseOptions(QGuiApplication& app) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Orbelio Gaussian Splatting viewer.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"validation", "Require the Khronos validation layer."});
    parser.addOption({"model", "Compressed SH0 PLY path (default: bundled PlayCanvas guitar).", "path"});
    parser.process(app);
    Options options{};
    options.validation = parser.isSet("validation");
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
        &bridge, &orbelio::ViewportBridge::frameCompleted, &model, &orbelio::AppModel::acceptFrame);
    QObject::connect(
        &model, &orbelio::AppModel::sceneLoaded, &bridge, &orbelio::ViewportBridge::requestFrame);
}

QSize windowSize(const QQuickView& view) {
    const auto available = view.screen()->availableGeometry().size();
    return {std::clamp(available.width() - screenMarginWidth, minimumWindowWidth, preferredWindowWidth),
            std::clamp(available.height() - screenMarginHeight, minimumWindowHeight, preferredWindowHeight)};
}

void showWindow(QQuickView& view,
                orbelio::AppModel& model,
                orbelio::ViewportBridge& bridge,
                const QString& path) {
    const auto size = windowSize(view);
    view.resize(size);
    view.rootContext()->setContextProperty("appModel", &model);
    view.rootContext()->setContextProperty("viewportBridge", &bridge);
    view.setSource(QUrl("qrc:/qt/qml/Orbelio/Main.qml"));
    if (view.status() == QQuickView::Error) {
        throw std::runtime_error("Could not load Main.qml");
    }
    view.resize(size);
    view.show();
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
    orbelio::BytePool cpuPool{orbelio::SceneLoader::modelByteBudget};
    orbelio::QtAssetReader reader;
    orbelio::QtGuiExecutor completions;
    orbelio::QtIoExecutor ioExecutor;
    orbelio::SceneLoader loader(ioExecutor, completions, reader, cpuPool);
    orbelio::ViewportState viewport;
    orbelio::AppModel model(host.validationEnabled(), loader, viewport, cpuPool);
    // The bridge must outlive the window's render-thread shutdown.
    std::unique_ptr<orbelio::ViewportBridge> bridge;
    QQuickView view;
    attachWindow(view, model, host);
    bridge = std::make_unique<orbelio::ViewportBridge>(
        viewport, std::make_shared<orbelio::VulkanRendererFactory>(host.context()));
    connectViewport(model, *bridge);
    showWindow(view, model, *bridge, options.modelPath);
    const int result = app.exec();
    releaseWindow(view);
    return result;
}
}

int main(int argc, char** argv) {
    qputenv("QSG_RENDER_LOOP", "threaded");
    QGuiApplication app(argc, argv);
    configureApplication(app);
    const auto options = parseOptions(app);
    try {
        return runViewer(app, options);
    } catch (const std::exception& error) {
        std::cerr << "Orbelio startup failed: " << error.what() << '\n';
        return 1;
    }
}
