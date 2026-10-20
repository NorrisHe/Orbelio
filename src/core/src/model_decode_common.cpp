#include "model_decode_internal.hpp"

namespace orbelio::detail {
namespace {
constexpr float coordinateLimit = 1e10f;
constexpr float scaleLimit = 40.f;
constexpr float minimumQuaternionNorm = 1e-12f;
constexpr float quaternionTolerance = 1e-6f;
constexpr unsigned halfSignMask = 0x8000;
constexpr unsigned halfFractionBits = 10;
constexpr unsigned halfExponentMask = 31;
constexpr unsigned halfFractionMask = 1023;
constexpr int halfExponentBias = 15;

void validateValues(const std::vector<float>& values, float limit) {
    for (const auto value : values) {
        require(std::isfinite(value) && std::abs(value) <= limit,
                "Nonfinite/out-of-range Gaussian parameter");
    }
}
}

float Bytes::f16(std::size_t offset) const {
    const auto bits = u16(offset);
    const auto exponent = (bits >> halfFractionBits) & halfExponentMask;
    const auto fraction = bits & halfFractionMask;
    require(exponent != halfExponentMask, "Nonfinite half-float Gaussian parameter");
    const auto value = exponent ? std::ldexp(1.f + float(fraction) / (halfFractionMask + 1),
                                             int(exponent) - halfExponentBias)
                                : std::ldexp(float(fraction), 1 - halfExponentBias - halfFractionBits);
    return bits & halfSignMask ? -value : value;
}

void DecodeContext::checkWorking(std::size_t additional) const {
    require(outputBytes <= limits.decodedBytes && temporaryBytes <= limits.decodedBytes - outputBytes &&
                additional <= limits.decodedBytes - outputBytes - temporaryBytes,
            "Decoded model/workspace exceeds CPU memory budget");
}

std::size_t DecodeContext::sceneBytes(std::size_t count, unsigned degree) const {
    require(count && count <= limits.maxGaussians && degree <= 3, "Invalid Gaussian count or SH degree");
    const auto rest = degree * (degree + 2) * 3;
    return checkedProduct(count, (baseFloatCount + framingFloatCount + rest) * sizeof(float));
}

StaticScene DecodeContext::createScene(std::size_t count, unsigned degree, std::string format) {
    const auto rest = degree * (degree + 2) * 3;
    const auto bytes = sceneBytes(count, degree);
    checkWorking(bytes);
    StaticScene scene{};
    scene.memory = resident.reserve(bytes);
    outputBytes = bytes;
    scene.shDegree = degree;
    scene.format = std::move(format);
    scene.positions.resize(count * 3);
    scene.rotations.resize(count * 4);
    scene.logScales.resize(count * 3);
    scene.opacityLogits.resize(count);
    scene.sh0.resize(count * 3);
    scene.shRest.resize(count * rest);
    return scene;
}

WorkingReservation::WorkingReservation(DecodeContext& context, std::size_t bytes) : context_(&context) {
    context.checkWorking(bytes);
    memory_ = context.resident.reserve(bytes);
    context.temporaryBytes += bytes;
}

WorkingReservation::~WorkingReservation() {
    if (context_) {
        context_->temporaryBytes -= memory_.bytes();
    }
}

void normalizeRotation(StaticScene& scene, std::size_t index) {
    float squaredNorm = 0;
    for (std::size_t axis = 0; axis < 4; ++axis) {
        const auto value = scene.rotations[index * 4 + axis];
        squaredNorm += value * value;
    }
    require(std::isfinite(squaredNorm) && squaredNorm > minimumQuaternionNorm, "Invalid Gaussian quaternion");
    if (std::abs(squaredNorm - 1.f) <= quaternionTolerance) {
        return;
    }
    const auto inverseNorm = 1.f / std::sqrt(squaredNorm);
    for (std::size_t axis = 0; axis < 4; ++axis) {
        scene.rotations[index * 4 + axis] *= inverseNorm;
    }
}

void validateDecodedScene(StaticScene& scene) {
    validateValues(scene.positions, coordinateLimit);
    validateValues(scene.logScales, scaleLimit);
    validateValues(scene.opacityLogits, coordinateLimit);
    validateValues(scene.sh0, coordinateLimit);
    validateValues(scene.shRest, coordinateLimit);
    for (std::size_t index = 0; index < scene.count(); ++index) {
        normalizeRotation(scene, index);
    }
}
}
