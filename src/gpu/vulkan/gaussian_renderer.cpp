#include "gaussian_renderer.hpp"
#include "static_gs_renderer.hpp"
#include "fdu4d_gs_renderer.hpp"

namespace orbelio {
GaussianRendererKind GaussianRendererFactory::kind(const FrameState& frame) {
    return frame.fdu4d ? GaussianRendererKind::Fdu4d : GaussianRendererKind::Static;
}

std::unique_ptr<IGaussianRenderer> GaussianRendererFactory::create(GaussianRendererKind kind,
                                                                   DeviceContext context,
                                                                   VmaAllocator allocator) {
    switch (kind) {
    case GaussianRendererKind::Static:
        return std::make_unique<StaticGsRenderer>(context, allocator);
    case GaussianRendererKind::Fdu4d:
        return std::make_unique<Fdu4dGsRenderer>(context, allocator);
    }
    throw std::invalid_argument("Unknown Gaussian renderer kind");
}
}
