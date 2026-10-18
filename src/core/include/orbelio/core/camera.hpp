#pragma once

#include <array>
#include <cstdint>

namespace orbelio {
struct CameraState {
    CameraState();

    std::array<float, 3> target{};
    float distance{};
    float yawDegrees = 0;
    float pitchDegrees{};
    float verticalFieldOfViewDegrees{};
    float sceneRadius = 1;

    void fit(const std::array<float, 3>& center, float radius);
    void reset(const std::array<float, 3>& center, float radius);
    void orbit(float yawDeltaDegrees, float pitchDeltaDegrees);
    void pan(float horizontalFraction, float verticalFraction);
    void zoom(float steps);
};

struct CameraView {
    std::array<float, 3> eye{};
    std::array<float, 3> right{};
    std::array<float, 3> down{};
    std::array<float, 3> forward{};
    std::array<float, 16> worldToCamera{};
    float focalPixels = 1;
};

CameraView cameraView(const CameraState& state, std::uint32_t width, std::uint32_t height);
}
