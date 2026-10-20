#include <orbelio/core/camera.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbelio {
namespace {
constexpr float radiansPerDegree = std::numbers::pi_v<float> / 180.f;
constexpr float fullTurnDegrees = 360;
constexpr float maximumPitchDegrees = 89;
constexpr float minimumSceneRadius = 1e-6f;
constexpr float fitDistanceFactor = 2.7f;
constexpr float initialHeightRatio = .1f;
constexpr float defaultFieldOfViewDegrees = 50;
constexpr float defaultSceneRadius = 1;
constexpr float minimumDistanceFactor = .01f;
constexpr float maximumDistanceFactor = 1000;
constexpr float zoomExponentPerStep = .15f;
constexpr float maximumZoomSteps = 100;
constexpr float maximumMovementSeconds = .1f;
constexpr float fastMovementFactor = 4;

struct CameraBasis {
    std::array<float, 3> right{};
    std::array<float, 3> down{};
    std::array<float, 3> forward{};
};

CameraBasis cameraBasis(const CameraState& state) {
    const float yaw = state.yawDegrees * radiansPerDegree;
    const float pitch = state.pitchDegrees * radiansPerDegree;
    const float sineYaw = std::sin(yaw);
    const float cosineYaw = std::cos(yaw);
    const float sinePitch = std::sin(pitch);
    const float cosinePitch = std::cos(pitch);
    return {.right = {-cosineYaw, 0, sineYaw},
            .down = {sineYaw * sinePitch, cosinePitch, cosineYaw * sinePitch},
            .forward = {-sineYaw * cosinePitch, sinePitch, -cosineYaw * cosinePitch}};
}

float focalPixels(const CameraState& state, float extent) {
    return extent * .5f / std::tan(state.verticalFieldOfViewDegrees * radiansPerDegree * .5f);
}

float dot(const std::array<float, 3>& first, const std::array<float, 3>& second) {
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

float framingDistance(float radius) {
    return radius * fitDistanceFactor * std::sqrt(1 + initialHeightRatio * initialHeightRatio);
}
}

CameraState::CameraState()
    : distance(framingDistance(defaultSceneRadius)),
      pitchDegrees(std::atan(initialHeightRatio) / radiansPerDegree),
      verticalFieldOfViewDegrees(defaultFieldOfViewDegrees) {}

void CameraState::fit(const std::array<float, 3>& center, float radius) {
    if (!std::all_of(center.begin(), center.end(), [](float value) { return std::isfinite(value); })) {
        return;
    }
    target = center;
    sceneRadius = std::isfinite(radius) && radius > minimumSceneRadius ? radius : 1.f;
    distance = framingDistance(sceneRadius);
}

void CameraState::reset(const std::array<float, 3>& center, float radius) {
    *this = CameraState{};
    fit(center, radius);
}

void CameraState::orbit(float yawDeltaDegrees, float pitchDeltaDegrees) {
    if (!std::isfinite(yawDeltaDegrees) || !std::isfinite(pitchDeltaDegrees)) {
        return;
    }
    const auto previous = freeFlight ? cameraBasis(*this) : CameraBasis{};
    yawDegrees =
        std::remainder(yawDegrees + std::remainder(yawDeltaDegrees, fullTurnDegrees), fullTurnDegrees);
    pitchDegrees = std::clamp(pitchDegrees + pitchDeltaDegrees, -maximumPitchDegrees, maximumPitchDegrees);
    if (freeFlight) {
        const auto next = cameraBasis(*this);
        for (std::size_t axis = 0; axis < target.size(); ++axis) {
            target[axis] += (next.forward[axis] - previous.forward[axis]) * distance;
        }
    }
}

void CameraState::moveLocal(float right, float up, float forward, float seconds, bool fast) {
    if (!freeFlight || !std::isfinite(right) || !std::isfinite(up) || !std::isfinite(forward) ||
        !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    const auto basis = cameraBasis(*this);
    std::array<float, 3> direction{};
    for (std::size_t axis = 0; axis < direction.size(); ++axis) {
        direction[axis] = right * basis.right[axis] + forward * basis.forward[axis] - (axis == 1 ? up : 0.f);
    }
    const float length = std::hypot(direction[0], direction[1], direction[2]);
    if (length == 0 || !std::isfinite(length)) {
        return;
    }
    const float step = sceneRadius * std::clamp(seconds, 0.f, maximumMovementSeconds) *
                       (fast ? fastMovementFactor : 1.f) / std::max(1.f, length);
    auto next = target;
    for (std::size_t axis = 0; axis < target.size(); ++axis) {
        next[axis] += step * direction[axis];
    }
    if (std::all_of(next.begin(), next.end(), [](float value) { return std::isfinite(value); })) {
        target = next;
    }
}

void CameraState::pan(float horizontalFraction, float verticalFraction) {
    if (!std::isfinite(horizontalFraction) || !std::isfinite(verticalFraction)) {
        return;
    }
    const auto basis = cameraBasis(*this);
    const float span = distance / focalPixels(*this, 1);
    auto next = target;
    for (std::size_t axis = 0; axis < target.size(); ++axis) {
        next[axis] -= span * (basis.right[axis] * horizontalFraction + basis.down[axis] * verticalFraction);
    }
    if (std::all_of(next.begin(), next.end(), [](float value) { return std::isfinite(value); })) {
        target = next;
    }
}

void CameraState::zoom(float steps) {
    if (!std::isfinite(steps)) {
        return;
    }
    const float exponent = -std::clamp(steps, -maximumZoomSteps, maximumZoomSteps) * zoomExponentPerStep;
    distance = std::clamp(distance * std::exp(exponent),
                          sceneRadius * minimumDistanceFactor,
                          sceneRadius * maximumDistanceFactor);
}

CameraView cameraView(const CameraState& state, std::uint32_t width, std::uint32_t height) {
    CameraView view{};
    const auto basis = cameraBasis(state);
    view.right = basis.right;
    view.down = basis.down;
    view.forward = basis.forward;
    for (std::size_t axis = 0; axis < view.eye.size(); ++axis) {
        view.eye[axis] = state.target[axis] - view.forward[axis] * state.distance;
    }
    const std::array rows{view.right, view.down, view.forward};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t column = 0; column < rows.size(); ++column) {
            view.worldToCamera[column * 4 + row] = rows[row][column];
        }
        view.worldToCamera[12 + row] = -dot(rows[row], view.eye);
    }
    view.worldToCamera[15] = 1;
    // Match the existing renderer's shorter-axis FOV, including portrait viewports.
    view.focalPixels = focalPixels(state, static_cast<float>(std::max(1u, std::min(width, height))));
    return view;
}
}
