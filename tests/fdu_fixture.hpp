#pragma once
#include <bit>
#include <cstdint>
#include <string>

namespace fdu_fixture {
template <class T> void append(std::string& bytes, T value) {
    const auto bits = std::bit_cast<std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(char((bits >> (index * 8)) & 255));
    }
}

inline std::string model() {
    std::string bytes("FDU4DGS\0", 8);
    append(bytes, std::uint32_t(1));
    append(bytes, std::uint32_t(64));
    append(bytes, std::uint64_t(1));
    for (int field = 0; field < 4; ++field) {
        append(bytes, std::uint32_t(0));
    }
    append(bytes, 0.);
    append(bytes, 1.);
    append(bytes, 4.);
    for (float value : {1.f,
                        0.f,
                        0.f,
                        .25f,
                        -2.f,
                        -2.f,
                        -2.f,
                        1.f,
                        0.f,
                        0.f,
                        0.f,
                        .5f,
                        0.f,
                        0.f,
                        .25f,
                        2.f,
                        .5f,
                        .2f,
                        .1f}) {
        append(bytes, value);
    }
    return bytes;
}
}
