#pragma once

#include "memory_budget.hpp"
#include <span>
#include <string>
#include <vector>

namespace orbelio {
struct SequenceFrame {
    double timestampSeconds = 0;
    std::string asset;
};

struct PlySequence {
    MemoryBudget::Reservation memory;
    std::vector<SequenceFrame> frames;
    double endTimeSeconds = 0;

    std::size_t frameAt(double time) const;
    double clampTime(double time) const;
    double advance(double time, double seconds, bool loop) const;
};

bool isPlySequence(std::span<const std::byte> bytes, std::string_view asset);
PlySequence decodePlySequence(std::span<const std::byte> bytes,
                              std::string_view asset,
                              const MemoryBudget& resident);
}
