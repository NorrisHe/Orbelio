#include <app_model.hpp>
#include <asset_reader.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
constexpr std::size_t guitarCount = 90854;
constexpr std::size_t bikerCount = 152746;
constexpr int rapidRequests = 100;
const QString guitarPath = ORBELIO_SAMPLE_DIR "/guitar/guitar.compressed.ply";
const QString bikerPath = ORBELIO_SAMPLE_DIR "/biker/biker.compressed.ply";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ManualExecutor final : public orbelio::IExecutor {
public:
    bool acceptsTasks = true;
    std::deque<Task> tasks;
    std::size_t maximumPending = 0;

    bool tryPost(Task task, std::size_t) override {
        if (!acceptsTasks || !task) {
            return false;
        }
        tasks.push_back(std::move(task));
        maximumPending = std::max(maximumPending, tasks.size());
        return true;
    }

    void runOne() {
        require(!tasks.empty(), "No queued task");
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
};

class CountingReader final : public orbelio::IAssetReader {
public:
    QStringList paths;
    orbelio::QtAssetReader reader;

    orbelio::BytePool::Lease read(std::string_view asset,
                                  orbelio::BytePool& pool,
                                  std::size_t limit) override {
        paths.append(QString::fromUtf8(asset.data(), qsizetype(asset.size())));
        return reader.read(asset, pool, limit);
    }
};

struct Fixture {
    orbelio::BytePool pool{orbelio::SceneLoader::modelByteBudget};
    CountingReader reader;
    ManualExecutor worker;
    ManualExecutor completions;
    orbelio::SceneLoader loader{worker, completions, reader, pool};
    orbelio::ViewportState viewport;
    std::unique_ptr<orbelio::AppModel> model =
        std::make_unique<orbelio::AppModel>(false, loader, viewport, pool);

    void completeRead() {
        worker.runOne();
        completions.runOne();
    }

    void completeFrame() {
        orbelio::FrameReport report{};
        report.frame.sceneVersion = viewport.sceneVersion();
        report.frame.gaussianCount = viewport.gaussianCount();
        report.frame.sceneUploads = 1;
        model->acceptFrame(report, {});
    }

    void loadReady(const QString& path) {
        model->loadModel(path);
        completeRead();
        completeFrame();
        require(model->loadState() == "Ready", "Model did not reach Ready");
    }
};

void latestRequestWinsWithBoundedQueues() {
    Fixture fixture;
    fixture.model->loadModel(guitarPath);
    for (int request = 0; request < rapidRequests; ++request) {
        fixture.model->loadModel(request % 2 ? bikerPath : guitarPath);
    }
    require(fixture.worker.tasks.size() == 1, "Rapid requests flooded the worker queue");
    fixture.completeRead();
    require(!fixture.viewport.hasScene() && fixture.model->isLoading(), "Obsolete result was published");
    fixture.completeRead();
    require(fixture.model->isLoading(), "Ready was published before a GPU completion");
    orbelio::FrameReport stale{};
    stale.frame.gaussianCount = guitarCount;
    fixture.model->acceptFrame(stale, {});
    require(fixture.model->isLoading(), "An obsolete frame completed the latest load");
    fixture.completeFrame();
    require(fixture.model->loadState() == "Ready" && fixture.model->modelPath() == bikerPath &&
                fixture.model->gaussianCount() == bikerCount,
            "Latest model was not displayed");
    require(fixture.reader.paths == QStringList{guitarPath, bikerPath},
            "Superseded paths were still decoded");
    require(fixture.worker.maximumPending == 1 && fixture.completions.maximumPending == 1,
            "Request or result queue was not bounded");
}

void failurePreservesSceneAndRetryResetsCamera() {
    Fixture fixture;
    fixture.loadReady(guitarPath);
    fixture.viewport.orbitCamera(30, 10);
    const auto original = fixture.viewport.frame(0, 1, 1);
    fixture.model->loadModel(guitarPath + ".missing");
    fixture.completeRead();
    require(fixture.model->loadState() == "Error" && fixture.model->hasScene() &&
                fixture.viewport.frame(0, 1, 1).scene == original.scene,
            "Failed read discarded the current scene");
    fixture.loadReady(bikerPath);
    require(fixture.model->gaussianCount() == bikerCount &&
                fixture.viewport.frame(0, 1, 1).camera.yawDegrees == 0,
            "Retry failed or scene switching kept the previous camera");
}

void unloadInvalidatesReadAndQueuedResult() {
    for (const bool resultAlreadyQueued : {false, true}) {
        Fixture fixture;
        fixture.model->loadModel(guitarPath);
        if (resultAlreadyQueued) {
            fixture.worker.runOne();
        }
        fixture.model->unloadModel();
        if (!resultAlreadyQueued) {
            fixture.worker.runOne();
        }
        fixture.completions.runOne();
        require(!fixture.model->hasScene() && fixture.model->loadState() == "Empty" &&
                    fixture.model->modelPath().isEmpty() && fixture.model->performance().isEmpty(),
                "An obsolete completion repopulated an unloaded model");
        require(fixture.model->ioStatus() == "No model loaded", "Unloaded IO status remained busy");
        require(fixture.pool.stats().activeBytes == 0, "Unload retained a read lease");
        fixture.loadReady(bikerPath);
    }
}

void invalidDropSupersedesLoadingAndUnicodePathsWork() {
    Fixture fixture;
    fixture.model->loadModel(guitarPath);
    fixture.model->openFiles({QUrl("https://example.com/model.ply")});
    fixture.completeRead();
    require(fixture.model->loadState() == "Error" && !fixture.model->hasScene(),
            "Remote URL or obsolete read was accepted");
    fixture.model->openFiles({QUrl::fromLocalFile(guitarPath), QUrl::fromLocalFile(bikerPath)});
    require(fixture.worker.tasks.empty() && fixture.model->loadState() == "Error",
            "Multi-file drop was queued");
    QTemporaryDir directory;
    require(directory.isValid(), "Could not create temporary Unicode fixture directory");
    const auto path = directory.filePath(QString::fromUtf8("模型 空格.compressed.ply"));
    require(QFile::copy(guitarPath, path), "Could not create Unicode fixture");
    fixture.model->openFiles({QUrl::fromLocalFile(path)});
    fixture.completeRead();
    fixture.completeFrame();
    require(fixture.model->loadState() == "Ready" && fixture.model->modelPath() == path &&
                fixture.model->modelName() == QString::fromUtf8("模型 空格.compressed.ply"),
            "File URL decoding corrupted Unicode or spaces");
}

void admissionAndBudgetsFailWithoutPoisoningTheSession() {
    Fixture fixture;
    fixture.worker.acceptsTasks = false;
    fixture.model->loadModel(guitarPath);
    require(fixture.model->loadState() == "Error" && fixture.worker.tasks.empty(),
            "Backpressure was ignored");
    fixture.worker.acceptsTasks = true;
    fixture.loadReady(guitarPath);
    QTemporaryFile oversized;
    require(oversized.open() && oversized.resize(orbelio::SceneLoader::modelByteBudget + 1),
            "Could not create oversized fixture");
    fixture.model->loadModel(oversized.fileName());
    fixture.completeRead();
    require(fixture.model->loadState() == "Error" && fixture.model->gaussianCount() == guitarCount,
            "Input budget failure discarded the current model");
    QFile file(guitarPath);
    require(file.open(QIODevice::ReadOnly), "Could not read fixture");
    const auto bytes = file.readAll();
    bool rejected = false;
    try {
        orbelio::decodeCompressedPly(std::as_bytes(std::span(bytes.constData(), std::size_t(bytes.size()))),
                                     1);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Decoded-memory budget was ignored");
    fixture.loadReady(bikerPath);
}

void closingDropsLateCompletions() {
    Fixture fixture;
    fixture.model->loadModel(guitarPath);
    fixture.model.reset();
    fixture.worker.runOne();
    fixture.completions.runOne();
    require(!fixture.viewport.hasScene() && fixture.pool.stats().activeBytes == 0,
            "Late completion accessed a destroyed model or retained a read lease");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        latestRequestWinsWithBoundedQueues();
        failurePreservesSceneAndRetryResetsCamera();
        unloadInvalidatesReadAndQueuedResult();
        invalidDropSupersedesLoadingAndUnicodePathsWork();
        admissionAndBudgetsFailWithoutPoisoningTheSession();
        closingDropsLateCompletions();
        std::cout << "Model loading, coalescing, stale frames, budgets, recovery and shutdown: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
