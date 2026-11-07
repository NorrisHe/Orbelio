#include <orbelio/core/camera.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <stdexcept>

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
constexpr float fitOrthographicSpanFactor = 2.5f;
constexpr float minimumNearClip = 1e-6f;
constexpr float maximumProjectionCoefficient = 1e10f;
constexpr float matrixTolerance = 1e-4f;

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
    CameraBasis result{.right = {-cosineYaw, 0, sineYaw},
                       .down = {sineYaw * sinePitch, cosinePitch, cosineYaw * sinePitch},
                       .forward = {-sineYaw * cosinePitch, sinePitch, -cosineYaw * cosinePitch}};
    if (state.rollDegrees != 0) {
        const auto cosine = std::cos(state.rollDegrees * radiansPerDegree);
        const auto sine = std::sin(state.rollDegrees * radiansPerDegree);
        for (std::size_t axis = 0; axis < result.right.size(); ++axis) {
            const auto right = result.right[axis];
            const auto down = result.down[axis];
            result.right[axis] = cosine * right + sine * down;
            result.down[axis] = -sine * right + cosine * down;
        }
    }
    return result;
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

void configureProjection(CameraView& view, const CameraState& state, float width, float height) {
    view.projection = state.projection;
    if (state.projection != CameraProjection::Perspective &&
        state.projection != CameraProjection::Orthographic) {
        throw std::invalid_argument("Unknown camera projection");
    }
    if (!state.intrinsics && state.projection == CameraProjection::Perspective &&
        !(state.verticalFieldOfViewDegrees > 0 && state.verticalFieldOfViewDegrees < 180)) {
        throw std::invalid_argument("Camera field of view must be between 0 and 180 degrees");
    }
    view.focalPixels = state.projection == CameraProjection::Orthographic
                           ? std::min(width, height) / state.orthographicSpan
                           : focalPixels(state, std::min(width, height));
    view.intrinsics = {view.focalPixels, view.focalPixels, width * .5f, height * .5f};
    if (state.intrinsics) {
        const auto& lens = *state.intrinsics;
        if (!lens.width || !lens.height) {
            throw std::invalid_argument("Camera calibration image is empty");
        }
        const auto sx = width / lens.width;
        const auto sy = height / lens.height;
        view.intrinsics = {lens.fx * sx, lens.fy * sy, lens.cx * sx, lens.cy * sy};
        view.focalPixels = view.intrinsics[0];
    }
    for (const auto value : view.intrinsics) {
        if (!std::isfinite(value) || std::abs(value) > maximumProjectionCoefficient) {
            throw std::invalid_argument("Camera intrinsics outside supported numeric range");
        }
    }
    view.depthRange = {state.nearClip, state.farClip.value_or(std::numeric_limits<float>::max())};
    if (view.intrinsics[0] <= 0 || view.intrinsics[1] <= 0 || !std::isfinite(state.nearClip) ||
        state.nearClip < minimumNearClip || !std::isfinite(view.depthRange[1]) ||
        view.depthRange[1] <= state.nearClip) {
        throw std::invalid_argument("Invalid camera focal length or clipping range");
    }
}

std::array<std::array<float, 3>, 3> rigidRows(const std::array<float, 16>& matrix) {
    if (!std::all_of(matrix.begin(), matrix.end(), [](float x) { return std::isfinite(x); }) ||
        std::abs(matrix[3]) > matrixTolerance || std::abs(matrix[7]) > matrixTolerance ||
        std::abs(matrix[11]) > matrixTolerance || std::abs(matrix[15] - 1) > matrixTolerance) {
        throw std::invalid_argument("Camera view matrix must be finite and affine");
    }
    const std::array<std::array<float, 3>, 3> rows{{{matrix[0], matrix[4], matrix[8]},
                                                    {matrix[1], matrix[5], matrix[9]},
                                                    {matrix[2], matrix[6], matrix[10]}}};
    for (std::size_t a = 0; a < rows.size(); ++a) {
        for (std::size_t b = 0; b < rows.size(); ++b) {
            if (std::abs(dot(rows[a], rows[b]) - float(a == b)) > matrixTolerance) {
                throw std::invalid_argument("Camera view matrix rotation must be orthonormal");
            }
        }
    }
    const auto& a = rows[0];
    const auto& b = rows[1];
    const std::array cross{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    if (dot(cross, rows[2]) < 1 - matrixTolerance) {
        throw std::invalid_argument("Camera view matrix rotation must preserve handedness");
    }
    return rows;
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
    orthographicSpan = sceneRadius * fitOrthographicSpanFactor;
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
    float span = distance / focalPixels(*this, 1);
    if (projection == CameraProjection::Orthographic) {
        span = orthographicSpan;
        if (intrinsics) {
            span = intrinsics->width <= intrinsics->height ? intrinsics->width / intrinsics->fx
                                                           : intrinsics->height / intrinsics->fy;
        }
    }
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
    if (projection == CameraProjection::Orthographic && intrinsics) {
        const auto fx = intrinsics->fx * std::exp(-exponent);
        const auto fy = intrinsics->fy * std::exp(-exponent);
        if (std::isfinite(fx) && std::isfinite(fy) && fx > 0 && fy > 0 &&
            std::max(fx, fy) <= maximumProjectionCoefficient) {
            intrinsics->fx = fx;
            intrinsics->fy = fy;
        }
        return;
    }
    auto& size = projection == CameraProjection::Orthographic ? orthographicSpan : distance;
    size = std::clamp(
        size * std::exp(exponent), sceneRadius * minimumDistanceFactor, sceneRadius * maximumDistanceFactor);
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
    if (!std::all_of(view.worldToCamera.begin(), view.worldToCamera.end(), [](float value) {
            return std::isfinite(value);
        })) {
        throw std::invalid_argument("Camera transform must be finite");
    }
    // Match the existing renderer's shorter-axis FOV, including portrait viewports.
    configureProjection(view, state, float(std::max(1u, width)), float(std::max(1u, height)));
    return view;
}

CameraState cameraFromViewMatrix(const std::array<float, 16>& matrix) {
    const auto rows = rigidRows(matrix);
    CameraState state;
    state.freeFlight = true;
    state.pitchDegrees = std::asin(std::clamp(rows[2][1], -1.f, 1.f)) / radiansPerDegree;
    state.yawDegrees = std::atan2(-rows[2][0], -rows[2][2]) / radiansPerDegree;
    const auto base = cameraBasis(state);
    state.rollDegrees = std::atan2(dot(rows[0], base.down), dot(rows[0], base.right)) / radiansPerDegree;
    for (std::size_t axis = 0; axis < state.target.size(); ++axis) {
        const auto eye =
            -(rows[0][axis] * matrix[12] + rows[1][axis] * matrix[13] + rows[2][axis] * matrix[14]);
        state.target[axis] = eye + rows[2][axis] * state.distance;
    }
    if (!std::all_of(
            state.target.begin(), state.target.end(), [](float value) { return std::isfinite(value); })) {
        throw std::invalid_argument("Camera view matrix translation exceeds float range");
    }
    return state;
}
}
