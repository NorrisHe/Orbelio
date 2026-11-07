#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace orbelio {
enum class CameraProjection { Perspective, Orthographic };

struct CameraIntrinsics {
    float fx = 1;
    float fy = 1;
    float cx = 0;
    float cy = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

struct CameraState {
    CameraState();

    std::array<float, 3> target{};
    float distance{};
    float yawDegrees = 0;
    float pitchDegrees{};
    float rollDegrees = 0;
    float verticalFieldOfViewDegrees{};
    float sceneRadius = 1;
    bool freeFlight = false;
    CameraProjection projection = CameraProjection::Perspective;
    std::optional<CameraIntrinsics> intrinsics;
    float orthographicSpan = 2.5f;
    float nearClip = .01f;
    std::optional<float> farClip;

    void fit(const std::array<float, 3>& center, float radius);
    void reset(const std::array<float, 3>& center, float radius);
    void orbit(float yawDeltaDegrees, float pitchDeltaDegrees);
    void pan(float horizontalFraction, float verticalFraction);
    void zoom(float steps);
    void moveLocal(float right, float up, float forward, float seconds, bool fast);
};

struct CameraView {
    std::array<float, 3> eye{};
    std::array<float, 3> right{};
    std::array<float, 3> down{};
    std::array<float, 3> forward{};
    std::array<float, 16> worldToCamera{};
    float focalPixels = 1;
    std::array<float, 4> intrinsics{};
    std::array<float, 2> depthRange{};
    CameraProjection projection = CameraProjection::Perspective;
};

CameraView cameraView(const CameraState& state, std::uint32_t width, std::uint32_t height);
// Column-major rigid world-to-camera matrix, with +Z forward and +Y down.
CameraState cameraFromViewMatrix(const std::array<float, 16>& matrix);
}
