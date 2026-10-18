#include <orbelio/core/viewport_state.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
constexpr std::array<float, 3> sceneCenter{2, -3, 4};
constexpr float sceneRadius = 2;
constexpr std::uint32_t viewportWidth = 800;
constexpr std::uint32_t viewportHeight = 600;
constexpr float tolerance = 1e-5f;
constexpr float legacyDistanceFactor = 2.7f;
constexpr float legacyHeightRatio = .1f;
constexpr float legacyHalfFovDegrees = 25;
constexpr float radiansPerDegree = std::numbers::pi_v<float> / 180.f;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(float first, float second) { return std::abs(first - second) < tolerance; }

float transformAxis(const std::array<float, 16>& matrix, const std::array<float, 3>& point, int row) {
    return matrix[row] * point[0] + matrix[4 + row] * point[1] + matrix[8 + row] * point[2] +
           matrix[12 + row];
}

void defaultViewMatchesExistingConvention() {
    orbelio::CameraState camera;
    camera.fit(sceneCenter, sceneRadius);
    const auto view = orbelio::cameraView(camera, viewportWidth, viewportHeight);
    require(near(view.eye[0], sceneCenter[0]) &&
                near(view.eye[1], sceneCenter[1] - sceneRadius * legacyDistanceFactor * legacyHeightRatio) &&
                near(view.eye[2], sceneCenter[2] + sceneRadius * legacyDistanceFactor),
            "Initial camera no longer matches the previous framing");
    require(near(transformAxis(view.worldToCamera, view.eye, 0), 0) &&
                near(transformAxis(view.worldToCamera, view.eye, 1), 0) &&
                near(transformAxis(view.worldToCamera, view.eye, 2), 0),
            "World-to-camera matrix does not map the eye to the origin");
    require(near(transformAxis(view.worldToCamera, camera.target, 0), 0) &&
                near(transformAxis(view.worldToCamera, camera.target, 1), 0) &&
                near(transformAxis(view.worldToCamera, camera.target, 2), camera.distance),
            "Camera target is not centered in front of the camera");
    require(view.right[0] < 0 && view.down[1] > 0 && view.forward[2] < 0,
            "Camera handedness or Y-down convention changed");
    const float expectedFocal = viewportHeight * .5f / std::tan(legacyHalfFovDegrees * radiansPerDegree);
    require(near(view.focalPixels, expectedFocal), "Projection FOV changed");
    const auto portrait = orbelio::cameraView(camera, viewportHeight, viewportWidth);
    const auto highDpi = orbelio::cameraView(camera, viewportWidth * 2, viewportHeight * 2);
    require(near(portrait.focalPixels, view.focalPixels) && near(highDpi.focalPixels, view.focalPixels * 2),
            "Portrait or DPI scaling changes the camera framing");
}

void gesturesAndFitHaveSeparateResponsibilities() {
    orbelio::CameraState camera;
    camera.fit(sceneCenter, sceneRadius);
    const auto initial = camera;
    camera.orbit(90, 10);
    require(camera.target == initial.target && camera.distance == initial.distance,
            "Orbit moved its target or changed distance");
    const auto rotated = camera;
    camera.pan(.1f, -.2f);
    require(camera.target != rotated.target && camera.yawDegrees == rotated.yawDegrees &&
                camera.pitchDegrees == rotated.pitchDegrees,
            "Pan changed orientation or failed to move the target");
    camera.zoom(1);
    require(camera.distance < initial.distance, "Wheel-up failed to zoom in");
    camera.zoom(-1);
    require(near(camera.distance, initial.distance), "Opposite zoom steps did not restore distance");
    camera.fit(sceneCenter, sceneRadius);
    require(camera.target == initial.target && camera.distance == initial.distance &&
                camera.yawDegrees == rotated.yawDegrees && camera.pitchDegrees == rotated.pitchDegrees,
            "Fit did not preserve orientation while restoring framing");
    camera.reset(sceneCenter, sceneRadius);
    require(camera.target == initial.target && camera.distance == initial.distance &&
                camera.yawDegrees == initial.yawDegrees && camera.pitchDegrees == initial.pitchDegrees,
            "Reset did not restore the initial camera");
}

void extremeGesturesRemainFinite() {
    orbelio::CameraState camera;
    camera.orbit(1e30f, 1e30f);
    camera.zoom(1e30f);
    require(camera.distance > 0 && std::abs(camera.pitchDegrees) < 90, "Camera bounds failed");
    camera.zoom(-1e30f);
    const auto view = orbelio::cameraView(camera, 0, 0);
    require(std::all_of(view.worldToCamera.begin(),
                        view.worldToCamera.end(),
                        [](float value) { return std::isfinite(value); }) &&
                view.focalPixels > 0,
            "Extreme input or empty viewport produced an invalid matrix");
    const auto stable = camera;
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    camera.orbit(invalid, 0);
    camera.pan(0, invalid);
    camera.zoom(invalid);
    require(camera.target == stable.target && camera.distance == stable.distance &&
                camera.yawDegrees == stable.yawDegrees && camera.pitchDegrees == stable.pitchDegrees,
            "Invalid input corrupted the camera");
    camera.reset({}, 0);
    require(std::isfinite(camera.distance) && camera.distance > 0, "Degenerate bounds broke Fit");
}

void cameraAndSceneTimeHaveIndependentSnapshots() {
    orbelio::ViewportState viewport;
    auto scene = std::make_shared<orbelio::StaticScene>();
    scene->center = sceneCenter;
    scene->radius = sceneRadius;
    viewport.setScene(scene);
    const auto initial = viewport.frame(0, viewportWidth, viewportHeight);
    const auto later = viewport.frame(10, viewportWidth, viewportHeight);
    require(initial.camera.target == later.camera.target &&
                initial.camera.yawDegrees == later.camera.yawDegrees,
            "Scene time still drives the camera");
    viewport.orbitCamera(30, 10);
    viewport.panCamera(.1f, .1f);
    viewport.zoomCamera(1);
    viewport.setScene(scene);
    const auto moved = viewport.frame(0, viewportWidth, viewportHeight);
    require(moved.timestampSeconds == initial.timestampSeconds && moved.scene == initial.scene &&
                moved.camera.yawDegrees != initial.camera.yawDegrees &&
                moved.camera.target != initial.camera.target &&
                moved.camera.distance < initial.camera.distance,
            "Paused gestures changed the asset/time or same-scene assignment reset the camera");
    viewport.resetCamera();
    require(moved.camera.yawDegrees != viewport.frame(0, 1, 1).camera.yawDegrees,
            "A later reset mutated an immutable frame snapshot");
    orbelio::ViewportState other;
    require(other.frame(0, 1, 1).camera.target != initial.camera.target, "Camera leaked between viewports");
}
}

int main() {
    try {
        defaultViewMatchesExistingConvention();
        gesturesAndFitHaveSeparateResponsibilities();
        extremeGesturesRemainFinite();
        cameraAndSceneTimeHaveIndependentSnapshots();
        std::cout << "Camera framing, gestures, limits, DPI and snapshot isolation: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
