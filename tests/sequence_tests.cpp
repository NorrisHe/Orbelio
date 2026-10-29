#include <orbelio/core/ply_sequence.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
const std::string manifest = R"({"schema":"orbelio.ply-sequence","version":1,"endTimeSeconds":1.25,
"frames":[{"path":"first.ply","timestampSeconds":0.125},
{"path":"frames/second.ply","timestampSeconds":0.4},{"path":"last.ply","timestampSeconds":1.0}]})";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

orbelio::PlySequence decode(const std::string& text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return orbelio::decodePlySequence(bytes, "C:/data/scene.gsseq", orbelio::MemoryBudget(1048576));
}

void reject(std::string from, std::string to) {
    auto invalid = manifest;
    invalid.replace(invalid.find(from), from.size(), to);
    bool rejected = false;
    try {
        decode(invalid);
    } catch (const std::exception&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("Invalid sequence manifest was accepted: " + to);
    }
}

void timestampBoundaries() {
    const auto sequence = decode(manifest);
    require(sequence.frameAt(-10) == 0 && sequence.frameAt(0.125) == 0 &&
                sequence.frameAt(0.3999999999) == 0 && sequence.frameAt(0.4) == 1 &&
                sequence.frameAt(1.0) == 2 && sequence.frameAt(10) == 2,
            "Timestamp selection did not use the latest non-future frame");
    require(sequence.advance(1.0, 0.25, true) == 0.125 && sequence.advance(1.0, 0.25, false) == 1.25,
            "Loop endpoint or non-loop clamp is incorrect");
    require(sequence.frames[1].asset == "C:/data/frames/second.ply",
            "Manifest-relative frame path was not resolved");
}

void invalidManifests() {
    reject("\"version\":1", "\"version\":2");
    reject("0.4", "0.125");
    reject("0.4", "-1");
    reject("0.4", "1e309");
    reject("1.25", "1.0");
    reject("first.ply", "../first.ply");
    reject("first.ply", "/first.ply");
    reject("first.ply", "https://invalid/first.ply");
    reject("first.ply", "first.spz");
    reject("\"version\":1", "\"version\":1,\"version\":1");
    bool rejected = false;
    try {
        orbelio::decodePlySequence(std::as_bytes(std::span(manifest.data(), manifest.size())),
                                   "scene.gsseq",
                                   orbelio::MemoryBudget(64));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Manifest metadata escaped the resident memory budget");
}
}

int main() {
    try {
        timestampBoundaries();
        invalidManifests();
        std::cout << "Sequence timestamps, loop endpoints, paths, versions and budget passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
