#include "fdu_fixture.hpp"
#include <orbelio/core/model_decoder.hpp>
#include <orbelio/core/viewport_state.hpp>
#include <cmath>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

auto bytes(const std::string& value) { return std::as_bytes(std::span(value.data(), value.size())); }

void evaluationAndLifetime() {
    orbelio::MemoryBudget resident(4096);
    {
        auto decoded = orbelio::decodeScene(bytes(fdu_fixture::model()), "test.fdu4d", {}, resident);
        const auto& scene = std::get<orbelio::Fdu4dScene>(decoded);
        require(scene.sampleCount() == 5 && scene.sampleAt(.499) == 1 && scene.sampleAt(2) == 4 &&
                    scene.sampleTime(4) == 1 && scene.advance(1, 0, true) == 0,
                "FDU time domain or sample grid differs");
        const auto sourceBytes = resident.usedBytes();
        for (int run = 0; run < 100; ++run) {
            const auto frame = orbelio::evaluateFdu4d(scene, .75);
            require(frame.positions[0] == 1.25f && frame.rotations[0] == 1 &&
                        std::abs(1. / (1 + std::exp(-frame.opacityLogits[0])) -
                                 std::exp(-.5) / (1 + std::exp(-2.))) < 1e-6,
                    "Conditional mean, opacity or quaternion mismatch");
        }
        require(resident.usedBytes() == sourceBytes, "Temporal evaluation retained old CPU frames");
    }
    require(resident.usedBytes() == 0, "FDU source reservations survived scene destruction");
}

void malformedInputsAndBudgets() {
    auto expectFailure = [](std::string model, orbelio::ModelLimits limits = {}) {
        orbelio::MemoryBudget resident(limits.residentBytes);
        bool failed = false;
        try {
            auto decoded = orbelio::decodeScene(bytes(model), "test.fdu4d", limits, resident);
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed && resident.usedBytes() == 0, "Invalid FDU model accepted or leaked budget");
    };
    for (const auto offset : {8, 12, 16, 24, 28, 32}) {
        auto data = fdu_fixture::model();
        data[std::size_t(offset)] = char(127);
        expectFailure(std::move(data));
    }
    auto truncated = fdu_fixture::model();
    truncated.pop_back();
    expectFailure(std::move(truncated));
    auto zeroVariance = fdu_fixture::model();
    zeroVariance.replace(64 + 14 * 4, 4, 4, '\0');
    expectFailure(std::move(zeroVariance));
    auto zeroRotation = fdu_fixture::model();
    zeroRotation.replace(64 + 7 * 4, 4, 4, '\0');
    expectFailure(std::move(zeroRotation));
    orbelio::ModelLimits limits;
    limits.decodedBytes = 32;
    expectFailure(fdu_fixture::model(), limits);
    limits.decodedBytes = 512;
    limits.residentBytes = 32;
    expectFailure(fdu_fixture::model(), limits);
}
}

int main() {
    try {
        evaluationAndLifetime();
        malformedInputsAndBudgets();
        std::cout << "FDU decoder, conditional evaluation, timestamps and reservations passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
