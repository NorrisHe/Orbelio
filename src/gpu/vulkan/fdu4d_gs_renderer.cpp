#include "fdu4d_gs_renderer.hpp"

namespace orbelio {
void Fdu4dGsRenderer::render(const FrameState& frame,
                             VkImage image,
                             VkImageView view,
                             bool initialized,
                             FrameReport& report) {
    if (!frame.fdu4d || frame.scene) {
        throw std::invalid_argument("FDU renderer requires one dynamic scene");
    }
    const auto& source = *frame.fdu4d;
    const GaussianTemporalView temporal{source.times,
                                        source.variances,
                                        source.velocities,
                                        source.temporalSh,
                                        source.temporalDegree,
                                        source.clampTime(frame.timestampSeconds),
                                        source.endTime - source.startTime};
    auto conditioned = frame;
    conditioned.scene = std::shared_ptr<const StaticScene>(frame.fdu4d, &source.spatial);
    conditioned.fdu4d.reset();
    rasterizer_.renderTemporal(conditioned, temporal, image, view, initialized, report);
    report.frame.temporalEvaluationCpuMs = 0.;
    report.frame.gaussianCount = frame.fdu4d->count();
}
}
