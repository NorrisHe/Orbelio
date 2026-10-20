#include <camera_tools.hpp>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <algorithm>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ManualExecutor final : public orbelio::IExecutor {
public:
    bool acceptsTasks = true;
    std::deque<Task> tasks;
    std::size_t retainedBytes = 0;

    bool tryPost(Task task, std::size_t bytes) override {
        if (!acceptsTasks) {
            return false;
        }
        retainedBytes = bytes;
        tasks.push_back(std::move(task));
        return true;
    }

    void runOne() {
        require(!tasks.empty(), "No deferred task");
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
};

class UnusedRendererFactory final : public orbelio::IVulkanRendererFactory {
public:
    std::unique_ptr<orbelio::IVulkanRenderer> create() const override {
        throw std::runtime_error("Camera services must not create a renderer");
    }
};

struct Fixture {
    ManualExecutor worker;
    ManualExecutor completions;
    orbelio::ViewportState state;
    orbelio::ViewportBridge bridge{state, std::make_shared<UnusedRendererFactory>()};
    std::unique_ptr<orbelio::CameraTools> tools =
        std::make_unique<orbelio::CameraTools>(state, bridge, worker, completions);
    orbelio::FrameReport report{};
    QTemporaryDir directory;

    Fixture() {
        require(directory.isValid(), "Could not create temporary output directory");
        state.setScene(std::make_shared<orbelio::StaticScene>());
        report.frame.sceneVersion = state.sceneVersion();
        report.frame.viewportWidth = 2;
        report.frame.viewportHeight = 2;
        tools->acceptFrame(report, {});
    }

    QUrl path(const char* name) const { return QUrl::fromLocalFile(directory.filePath(name)); }

    void finishWrite() {
        worker.runOne();
        require(tools->busy(), "Worker mutated GUI state directly");
        completions.runOne();
        require(!tools->busy(), "Completion left output busy");
    }
};

void bookmarksAreBoundedAndRestoreFullState() {
    Fixture fixture;
    fixture.bridge.setFreeCamera(true);
    fixture.bridge.moveCamera(1, 0, 1, .1f, true);
    const auto saved = fixture.state.camera();
    fixture.tools->saveBookmark();
    fixture.bridge.resetCamera();
    fixture.tools->restoreBookmark(0);
    const auto restored = fixture.state.camera();
    require(restored.target == saved.target && restored.freeFlight == saved.freeFlight &&
                restored.distance == saved.distance && restored.sceneRadius == saved.sceneRadius &&
                restored.verticalFieldOfViewDegrees == saved.verticalFieldOfViewDegrees,
            "Bookmark did not restore full state");
    for (int index = 0; index < 30; ++index) {
        fixture.tools->saveBookmark();
    }
    require(fixture.tools->bookmarks().size() == 16, "Bookmark memory is unbounded");
    fixture.tools->removeBookmark(0);
    fixture.tools->saveBookmark();
    require(fixture.tools->bookmarks().back() == "View 17", "Removed bookmark name was reused");
    fixture.state.setScene({});
    fixture.tools->sceneChanged();
    require(fixture.tools->bookmarks().isEmpty(), "Scene change retained bookmarks");
}

void exportIsDeferredAndRejectsConcurrentWrites() {
    Fixture fixture;
    const auto destination = fixture.path("camera.json");
    fixture.tools->exportCamera(destination);
    fixture.tools->exportCamera(fixture.path("ignored.json"));
    require(fixture.tools->busy() && fixture.worker.tasks.size() == 1 &&
                !QFileInfo::exists(destination.toLocalFile()),
            "Output ran synchronously or queued concurrent writes");
    fixture.finishWrite();
    require(fixture.tools->status() == "Saved" && QFileInfo::exists(destination.toLocalFile()) &&
                !QFileInfo::exists(fixture.path("ignored.json").toLocalFile()),
            "Deferred export failed");
}

void screenshotMatchesRequestAndDefersEncoding() {
    Fixture fixture;
    const auto destination = fixture.path("viewport.png");
    fixture.tools->saveScreenshot(destination);
    auto capture = std::make_shared<orbelio::FrameCapture>();
    capture->requestId = fixture.state.frame(0, 1, 1).captureRequestId;
    capture->width = 2;
    capture->height = 2;
    capture->rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    fixture.report.frame.capture = capture;
    ++fixture.report.frame.sceneVersion;
    fixture.tools->acceptFrame(fixture.report, {});
    require(fixture.worker.tasks.empty(), "Stale scene capture reached the writer");
    --fixture.report.frame.sceneVersion;
    fixture.tools->acceptFrame(fixture.report, {});
    require(fixture.worker.tasks.size() == 1 && fixture.worker.retainedBytes == capture->rgba.size() &&
                fixture.state.frame(0, 1, 1).captureRequestId == 0 &&
                !QFileInfo::exists(destination.toLocalFile()),
            "Capture encoding ran on GUI or request/accounting was retained");
    fixture.finishWrite();
    const auto image = QImage(destination.toLocalFile()).convertToFormat(QImage::Format_RGBA8888);
    require(!image.isNull() && image.width() == 2 && image.height() == 2 &&
                std::equal(capture->rgba.begin(), capture->rgba.end(), image.constBits()),
            "PNG encoding lost captured pixels");
}

void rejectedWritesAndSceneChangesRecover() {
    Fixture fixture;
    fixture.worker.acceptsTasks = false;
    fixture.tools->exportCamera(fixture.path("rejected.json"));
    require(!fixture.tools->busy() && fixture.tools->status().contains("IO queue is full"),
            "Rejected worker task left output busy");
    fixture.worker.acceptsTasks = true;
    fixture.tools->exportCamera(fixture.path("missing/camera.json"));
    fixture.finishWrite();
    require(fixture.tools->status().startsWith("Save failed:"), "Disk failure was not surfaced");
    fixture.tools->saveScreenshot(fixture.path("cancelled.png"));
    fixture.state.setScene({});
    fixture.tools->sceneChanged();
    require(!fixture.tools->busy() && fixture.worker.tasks.empty() &&
                fixture.state.frame(0, 1, 1).captureRequestId == 0,
            "Scene change failed to cancel pending capture");
}

void destroyedReceiverDropsCompletion() {
    Fixture fixture;
    fixture.tools->exportCamera(fixture.path("after-close.json"));
    fixture.tools.reset();
    fixture.worker.runOne();
    fixture.completions.runOne();
    require(QFileInfo::exists(fixture.path("after-close.json").toLocalFile()),
            "Owned output data was destroyed with GUI receiver");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        bookmarksAreBoundedAndRestoreFullState();
        exportIsDeferredAndRejectsConcurrentWrites();
        screenshotMatchesRequestAndDefersEncoding();
        rejectedWritesAndSceneChangesRecover();
        destroyedReceiverDropsCompletion();
        std::cout << "Camera bookmarks, deferred output, backpressure and receiver lifetime: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
