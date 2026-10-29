#include "fdu4d_gs_renderer.hpp"
#include <chrono>

namespace orbelio {
void Fdu4dGsRenderer::render(const FrameState& frame,
                             VkImage image,
                             VkImageView view,
                             bool initialized,
                             FrameReport& report) {
    if (!frame.fdu4d || frame.scene) {
        throw std::invalid_argument("FDU renderer requires one dynamic scene");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto time = frame.fdu4d->clampTime(frame.timestampSeconds);
    const auto count = frame.renderSettings.budgetedCount(frame.fdu4d->count());
    const bool changed = source_ != frame.fdu4d || !evaluated_ || time_ != time || count_ != count;
    if (changed) {
        auto next = std::make_shared<StaticScene>(evaluateFdu4d(*frame.fdu4d, time, count));
        evaluated_ = std::move(next);
        source_ = frame.fdu4d;
        time_ = time;
        count_ = count;
    }
    const auto evaluationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    auto conditioned = frame;
    conditioned.fdu4d.reset();
    conditioned.scene = evaluated_;
    rasterizer_.render(conditioned, image, view, initialized, report);
    report.frame.temporalEvaluationCpuMs = changed ? evaluationMs : 0.;
    report.frame.gaussianCount = frame.fdu4d->count();
}
}
