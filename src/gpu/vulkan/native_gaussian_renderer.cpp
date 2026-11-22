#include "native_gaussian_renderer.hpp"
#include "native_frame_commands.hpp"
#include "native_frame_output.hpp"
#include "native_projection.hpp"
#include "native_rasterizer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;
using Stamp = NativeFrameCommands::Stamp;
constexpr auto counterInterval = std::chrono::milliseconds(250);
constexpr double fullTurnDegrees = 360.;

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void validateSettings(const RenderSettings& settings) {
    if (std::uint32_t(settings.rasterMode) > std::uint32_t(RasterMode::TiledCompute) ||
        (settings.tileBinningWidth != 16 && settings.tileBinningWidth != 32 &&
         settings.tileBinningWidth != 64) ||
        !validOutputs(settings.outputs) || std::ranges::any_of(settings.background, [](float value) {
            return !std::isfinite(value) || value < 0;
        })) {
        throw std::invalid_argument("Invalid render outputs or background");
    }
}

struct PreparedFrame {
    CameraView camera;
    NativeCapturePlan capture;
    std::size_t uploadEnd = 0;
    bool sample = false;
};
}

struct NativeGaussianRenderer::Impl {
    NativeBufferCounts counts;
    NativeSceneUpload model;
    NativeProjection projection;
    NativeFrameOutput output;
    NativeRasterizer rasterizer;
    NativeFrameCommands commands;
    Clock::time_point lastCounterSample;

    Impl(DeviceContext context, VmaAllocator allocator, std::string_view directory)
        : model(allocator, counts), projection(context, allocator, counts, directory),
          output(context, allocator, counts, directory),
          rasterizer(context, allocator, counts, output.targets(), directory), commands(context) {}

    void reserveMandatoryStorage(const FrameState& frame,
                                 const PreparedFrame& prepared,
                                 FrameReport& report) {
        model.reserveModel();
        projection.reserve(model.targetCount());
        output.reserveTargets(frame, prepared.capture.count != 0, report);
        projection.reserveNormals(model.targetCount(), output.targets().hasNormals());
        rasterizer.ensureNormals();
        model.reserveUpload(prepared.uploadEnd);
        output.reserveCapture(frame, prepared.capture);
    }

    void reserveFrameStorage(const FrameState& frame, const PreparedFrame& prepared, FrameReport& report) {
        try {
            reserveMandatoryStorage(frame, prepared, report);
        } catch (const VulkanError& error) {
            if (!rasterizer.tiled() || !nativeAllocationFailed(error)) {
                throw;
            }
            // A retained optional tile pool must not starve a larger mandatory model or upload.
            rasterizer.releaseTiles();
            reserveMandatoryStorage(frame, prepared, report);
        }
    }

    PreparedFrame prepareFrame(const FrameState& frame,
                               const GaussianTemporalView& temporal,
                               FrameReport& report) {
        validateSettings(frame.renderSettings);
        PreparedFrame prepared{};
        prepared.camera = cameraView(frame.camera, frame.width, frame.height);
        const auto change = model.select(frame, temporal);
        if (change == NativeSceneUpload::Change::Upload) {
            rasterizer.clearRejection();
        }
        const bool changed = change != NativeSceneUpload::Change::None;
        if (changed) {
            output.invalidateCapture();
        }
        const bool pending = output.capturePending(frame);
        prepared.uploadEnd = model.uploadEnd(frame);
        const bool complete = prepared.uploadEnd == model.targetCount();
        if (pending) {
            const auto capture = nativeCapturePlan(frame);
            if (complete) {
                prepared.capture = capture;
            }
        }
        prepared.sample = changed || pending || (model.uploadedCount() < model.targetCount() && complete) ||
                          Clock::now() - lastCounterSample >= counterInterval;
        reserveFrameStorage(frame, prepared, report);
        if (rasterizer.prepare(frame, model.targetCount(), projection.normalStorageBytes())) {
            prepared.sample = true;
        }
        return prepared;
    }

    void recordReadbacks(VkCommandBuffer command, bool sampled, bool captured) {
        if (sampled) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT);
            projection.recordTelemetry(command, captured);
            rasterizer.recordTelemetry(command);
        }
        if (sampled || captured) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT,
                                VK_ACCESS_HOST_READ_BIT);
        }
    }

    void recordFrame(const FrameState& frame, const PreparedFrame& prepared, NativeDisplayTarget display) {
        const bool captured = prepared.capture.count != 0;
        const auto count = static_cast<std::uint32_t>(prepared.uploadEnd);
        const auto command = commands.begin();
        model.recordUpload(command, prepared.uploadEnd);
        commands.stamp(Stamp::ProjectionBegin);
        projection.project(command, frame, prepared.camera, model, count, captured);
        commands.stamp(Stamp::ProjectionEnd);
        projection.shade(command, frame, prepared.camera, model, count, captured);
        commands.stamp(Stamp::ShadingEnd);
        projection.sort(command, count);
        commands.stamp(Stamp::SortEnd);
        const auto input = projection.rasterInput(count);
        rasterizer.recordBinning(command, frame, input, commands.tileQueries(captured));
        commands.stamp(Stamp::BinningEnd);
        rasterizer.record(command, frame, input, commands.fragmentQuery(captured), captured);
        commands.stamp(Stamp::RasterEnd);
        output.composeImage(command, frame, display);
        commands.stamp(Stamp::CompositionEnd);
        output.recordCapture(command, frame, display.image, prepared.capture);
        recordReadbacks(command, prepared.sample, captured);
        output.publishLayouts(command, display.image, captured ? frame.captureOutputs : RenderOutput(0));
    }

    void readResults(const FrameState& frame, FrameReport& report, const PreparedFrame& prepared) {
        const auto start = Clock::now();
        const bool captured = prepared.capture.count != 0;
        commands.readTimestamps(frame, report, rasterizer.tiled(), captured);
        commands.readStatistics(report.backend, captured);
        report.backend.independentlyProjectedGaussians.reset();
        report.backend.projectionDiagnostics.reset();
        if (prepared.sample) {
            projection.readTelemetry(report.backend, captured);
            rasterizer.readTelemetry(captured);
            report.backend.telemetryFrameIndex = report.frame.frameIndex + 1;
            lastCounterSample = Clock::now();
        }
        report.backend.telemetryAgeMs = milliseconds(lastCounterSample);
        report.frame.queryReadbackCpuMs = milliseconds(start);
        output.readCapture(frame, prepared.capture);
    }

    void fillReport(const FrameState& state, FrameReport& report) const {
        auto& frame = report.frame;
        frame.gaussianCount = model.scene().count();
        frame.submittedGaussians = model.uploadedCount();
        frame.budgetedGaussians = model.targetCount();
        frame.refining = frame.submittedGaussians < frame.budgetedGaussians;
        frame.sceneUploads = model.uploads();
        frame.orbitDegrees = std::fmod(state.camera.yawDegrees + fullTurnDegrees, fullTurnDegrees);
        frame.temporalEvaluationCpuMs.reset();
        auto& stats = report.backend;
        stats.gsBufferAllocations = counts.allocations;
        stats.gsBufferReuses = counts.reuses;
        stats.gsGrowths = counts.growths;
        stats.modelBytes = model.bytes();
        stats.preprocessingBytes = projection.preprocessingBytes();
        stats.sortingBytes = projection.sortingBytes() + rasterizer.sortingBytes();
        stats.pixelBytes = output.targets().bytes();
        stats.telemetryBytes =
            projection.telemetryBytes() + rasterizer.telemetryBytes() + output.telemetryBytes();
        rasterizer.fillStats(state, stats);
        output.fillCaptureStats(frame);
        stats.gsReservedBytes = stats.modelBytes + stats.preprocessingBytes + stats.sortingBytes +
                                stats.pixelBytes + stats.telemetryBytes;
    }

    void render(const FrameState& frame,
                const GaussianTemporalView& temporal,
                NativeDisplayTarget display,
                FrameReport& report) {
        const auto prepared = prepareFrame(frame, temporal, report);
        recordFrame(frame, prepared, display);
        report.frame.backendWaitMs = commands.submitAndWait();
        model.commitUpload(prepared.uploadEnd);
        output.completeSubmission();
        readResults(frame, report, prepared);
        fillReport(frame, report);
    }
};

NativeGaussianRenderer::NativeGaussianRenderer(DeviceContext context,
                                               VmaAllocator allocator,
                                               std::string directory)
    : impl_(std::make_unique<Impl>(context, allocator, directory)) {}

NativeGaussianRenderer::~NativeGaussianRenderer() = default;

VulkanRenderImages NativeGaussianRenderer::outputImages() const {
    return impl_->output.targets().outputImages();
}

void NativeGaussianRenderer::render(const FrameState& frame,
                                    VkImage image,
                                    VkImageView view,
                                    bool initialized,
                                    FrameReport& report) {
    impl_->render(frame, {}, {image, view, initialized}, report);
}

void NativeGaussianRenderer::renderTemporal(const FrameState& frame,
                                            const GaussianTemporalView& temporal,
                                            VkImage image,
                                            VkImageView view,
                                            bool initialized,
                                            FrameReport& report) {
    const auto count = frame.scene ? frame.scene->count() : 0;
    const auto coefficients = frame.scene ? (frame.scene->shRestCoefficients() + 1) * 3 : 0;
    if (!count || temporal.times.size() != count || temporal.variances.size() != count ||
        temporal.velocities.size() != count * 3 ||
        temporal.harmonics.size() != count * coefficients * temporal.degree || temporal.degree > 2 ||
        !std::isfinite(temporal.time) || !std::isfinite(temporal.period) || temporal.period <= 0) {
        throw std::invalid_argument("Invalid Gaussian temporal frame");
    }
    impl_->render(frame, temporal, {image, view, initialized}, report);
}
}
