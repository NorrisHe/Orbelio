#include "model_decode_internal.hpp"
#include <charconv>
#include <cctype>
#include <sstream>

namespace orbelio::detail {
namespace {
constexpr std::size_t chunkSize = 256;
constexpr std::size_t chunkComponents = 18;
constexpr unsigned byteMask = 255;
constexpr unsigned tripleMask = 2047;
constexpr unsigned middleMask = 1023;
constexpr unsigned tripleShift = 21;
constexpr unsigned middleShift = 11;
constexpr unsigned quaternionShift = 30;
constexpr unsigned quaternionHighShift = 20;
constexpr unsigned quaternionMidShift = 10;
constexpr std::size_t headerWorkingFactor = 8;
constexpr std::size_t scaleMinOffset = 6;
constexpr std::size_t scaleMaxOffset = 9;
constexpr std::size_t colorMinOffset = 12;
constexpr std::size_t colorMaxOffset = 15;
constexpr float compressedShRange = 8.f;

enum class Scalar { i8, u8, i16, u16, i32, u32, f32, f64 };

struct Property {
    std::string name;
    Scalar type{};
    bool list = false;
    Scalar countType{};
};

struct Element {
    std::string name;
    std::size_t count = 0;
    std::vector<Property> properties;
};

struct Header {
    bool formatFound = false;
    bool ascii = false;
    bool little = true;
    bool antialiased = false;
    bool twoDimensional = false;
    std::size_t offset = 0;
    std::vector<Element> elements;
};

Scalar scalarType(std::string_view type) {
    if (type == "char" || type == "int8") {
        return Scalar::i8;
    }
    if (type == "uchar" || type == "uint8") {
        return Scalar::u8;
    }
    if (type == "short" || type == "int16") {
        return Scalar::i16;
    }
    if (type == "ushort" || type == "uint16") {
        return Scalar::u16;
    }
    if (type == "int" || type == "int32") {
        return Scalar::i32;
    }
    if (type == "uint" || type == "uint32") {
        return Scalar::u32;
    }
    if (type == "float" || type == "float32") {
        return Scalar::f32;
    }
    if (type == "double" || type == "float64") {
        return Scalar::f64;
    }
    throw std::runtime_error("Unsupported PLY scalar type");
}

void parseProperty(std::istringstream& row, Element& element) {
    std::string type;
    row >> type;
    Property property{};
    if (type == "list") {
        property.list = true;
        row >> type;
        property.countType = scalarType(type);
        require(property.countType <= Scalar::u32, "Invalid PLY list count type");
        row >> type;
    }
    property.type = scalarType(type);
    row >> property.name;
    require(bool(row) && !property.name.empty(), "Invalid PLY property");
    for (const auto& existing : element.properties) {
        require(existing.name != property.name, "Duplicate PLY property");
    }
    element.properties.push_back(std::move(property));
}

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

void parseComment(std::string line, Header& header) {
    for (auto& c : line) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto comment = trimmed(std::string_view(line).substr(line.find("comment") + 7));
    constexpr std::string_view modeKey = "splatrendermode: ";
    constexpr std::string_view aaKey = "antialiased ";
    if (comment.starts_with(modeKey)) {
        const auto mode = trimmed(comment.substr(modeKey.size()));
        if (mode != "default" && mode != "mip" && mode != "2dgs") {
            return;
        }
        header.antialiased = mode == "mip";
        header.twoDimensional = mode == "2dgs";
    } else if (comment.starts_with(aaKey)) {
        const auto value = trimmed(comment.substr(aaKey.size()));
        if (value != "0" && value != "1") {
            return;
        }
        header.antialiased = value == "1";
        header.twoDimensional = false;
    }
}

void parseFormat(std::istringstream& row, Header& header) {
    std::string format;
    std::string version;
    row >> format >> version;
    require(!header.formatFound && version == "1.0", "Invalid PLY format");
    require(format == "ascii" || format == "binary_little_endian" || format == "binary_big_endian",
            "Unsupported PLY encoding");
    header.ascii = format == "ascii";
    header.little = format != "binary_big_endian";
    header.formatFound = true;
}

void parseElement(std::istringstream& row, Header& header, const ModelLimits& limits) {
    Element element{};
    row >> element.name >> element.count;
    require(bool(row) && element.count <= limits.maxGaussians, "Invalid/oversized PLY element");
    for (const auto& existing : header.elements) {
        require(existing.name != element.name, "Duplicate PLY element");
    }
    header.elements.push_back(std::move(element));
}

bool parseHeaderLine(const std::string& line, Header& header, const ModelLimits& limits) {
    std::istringstream row(line);
    std::string tag;
    row >> tag;
    if (tag == "end_header") {
        require(header.formatFound && !header.elements.empty(), "Invalid PLY header");
        require(!header.twoDimensional, "2D Gaussian scenes are not supported");
        return true;
    }
    if (tag == "format") {
        parseFormat(row, header);
    } else if (tag == "element") {
        parseElement(row, header, limits);
    } else if (tag == "property") {
        require(!header.elements.empty(), "PLY property without element");
        parseProperty(row, header.elements.back());
    } else if (tag == "comment") {
        parseComment(line, header);
    } else {
        require(tag == "obj_info" || tag.empty(), "Unknown PLY header directive");
    }
    return false;
}

Header readHeader(std::span<const std::byte> data, const ModelLimits& limits) {
    const std::string_view input(reinterpret_cast<const char*>(data.data()), data.size());
    Header header{};
    const auto firstEnd = input.find('\n');
    require(firstEnd != std::string_view::npos && trimmed(input.substr(0, firstEnd)) == "ply",
            "Not a PLY file");
    header.offset = firstEnd + 1;
    while (header.offset < std::min(data.size(), headerByteLimit)) {
        const auto end = input.find('\n', header.offset);
        require(end != std::string_view::npos && end < headerByteLimit, "Missing/bounded PLY header");
        std::string line(input.substr(header.offset, end - header.offset));
        header.offset = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (parseHeaderLine(line, header, limits)) {
            return header;
        }
    }
    throw std::runtime_error("Missing/bounded PLY header");
}

class Cursor {
public:
    Cursor(std::span<const std::byte> data, const Header& header)
        : bytes_(data), text_(reinterpret_cast<const char*>(data.data()), data.size()), at_(header.offset),
          ascii_(header.ascii), little_(header.little) {}

    double scalar(Scalar type) {
        if (ascii_) {
            return asciiScalar(type);
        }
        switch (type) {
        case Scalar::i8:
            return binary<std::int8_t>();
        case Scalar::u8:
            return binary<std::uint8_t>();
        case Scalar::i16:
            return binary<std::int16_t>();
        case Scalar::u16:
            return binary<std::uint16_t>();
        case Scalar::i32:
            return binary<std::int32_t>();
        case Scalar::u32:
            return binary<std::uint32_t>();
        case Scalar::f32:
            return binary<float>();
        case Scalar::f64:
            return binary<double>();
        }
        throw std::runtime_error("Invalid PLY scalar");
    }

    double property(const Property& property) {
        if (!property.list) {
            return scalar(property.type);
        }
        const auto count = scalar(property.countType);
        require(count >= 0 && count <= bytes_.size(), "Oversized PLY list");
        for (std::size_t i = 0; i < count; ++i) {
            scalar(property.type);
        }
        return 0;
    }

    void finish() {
        if (ascii_) {
            skipSpace();
        }
        require(at_ == text_.size(), "Trailing PLY payload");
    }

private:
    template <class T> double binary() {
        const auto value = bytes_.read<T>(at_, little_);
        at_ += sizeof(T);
        return value;
    }

    void skipSpace() {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) {
            ++at_;
        }
    }

    double asciiScalar(Scalar type) {
        skipSpace();
        const auto start = at_;
        while (at_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[at_]))) {
            ++at_;
        }
        double value = 0;
        const auto result = std::from_chars(text_.data() + start, text_.data() + at_, value);
        require(result.ec == std::errc{} && result.ptr == text_.data() + at_ && std::isfinite(value),
                "Invalid ASCII PLY value");
        if (type <= Scalar::u32) {
            constexpr std::array<double, 6> minimum = {-128, 0, -32768, 0, -2147483648., 0};
            constexpr std::array<double, 6> maximum = {127, 255, 32767, 65535, 2147483647., 4294967295.};
            const auto index = static_cast<std::size_t>(type);
            require(value == std::trunc(value) && value >= minimum[index] && value <= maximum[index],
                    "ASCII PLY integer out of range");
        }
        return value;
    }

    Bytes bytes_;
    std::string_view text_;
    std::size_t at_;
    bool ascii_;
    bool little_;
};

const Element* findElement(const Header& header, std::string_view name) {
    for (const auto& element : header.elements) {
        if (element.name == name) {
            return &element;
        }
    }
    return nullptr;
}

int propertyIndex(const Element& element, std::string_view name) {
    for (std::size_t i = 0; i < element.properties.size(); ++i) {
        if (element.properties[i].name == name) {
            require(!element.properties[i].list, "Gaussian attributes cannot be PLY lists");
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::size_t restCount(const Element& element) {
    std::size_t count = 0;
    for (const auto& property : element.properties) {
        if (property.name.starts_with("f_rest_")) {
            ++count;
        }
    }
    shDegreeForRest(count);
    for (std::size_t i = 0; i < count; ++i) {
        require(propertyIndex(element, "f_rest_" + std::to_string(i)) >= 0,
                "Noncontiguous PLY SH coefficients");
    }
    return count;
}

struct Target {
    int field = -1;
    std::size_t component = 0;
};

std::vector<Target> ordinaryTargets(const Element& vertex, std::size_t rest) {
    constexpr std::array<std::string_view, baseFloatCount> names = {"x",
                                                                    "y",
                                                                    "z",
                                                                    "rot_0",
                                                                    "rot_1",
                                                                    "rot_2",
                                                                    "rot_3",
                                                                    "scale_0",
                                                                    "scale_1",
                                                                    "scale_2",
                                                                    "opacity",
                                                                    "f_dc_0",
                                                                    "f_dc_1",
                                                                    "f_dc_2"};
    std::vector<Target> targets(vertex.properties.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto index = propertyIndex(vertex, names[i]);
        require(index >= 0, "PLY is not a 3D Gaussian scene: missing required attribute");
        targets[index] = {static_cast<int>(i), 0};
    }
    for (std::size_t i = 0; i < rest; ++i) {
        const auto coefficients = rest / 3;
        targets[propertyIndex(vertex, "f_rest_" + std::to_string(i))] = {
            static_cast<int>(baseFloatCount), (i % coefficients) * 3 + i / coefficients};
    }
    return targets;
}

void storeOrdinary(StaticScene& scene, std::size_t index, Target target, float value) {
    if (target.field < 0) {
        return;
    }
    const auto field = static_cast<std::size_t>(target.field);
    if (field < 3) {
        scene.positions[index * 3 + field] = value;
    } else if (field < 7) {
        scene.rotations[index * 4 + field - 3] = value;
    } else if (field < 10) {
        scene.logScales[index * 3 + field - 7] = value;
    } else if (field == 10) {
        scene.opacityLogits[index] = value;
    } else if (field < baseFloatCount) {
        scene.sh0[index * 3 + field - 11] = value;
    } else {
        scene.shRest[index * scene.shRestCoefficients() * 3 + target.component] = value;
    }
}

// PlayCanvas packs a normalized triple as 11 + 10 + 11 bits.
std::array<float, 3> unpackTriple(std::uint32_t word) {
    return {float(word >> tripleShift) / tripleMask,
            float((word >> middleShift) & middleMask) / middleMask,
            float(word & tripleMask) / tripleMask};
}

void unpackRotation(StaticScene& scene, std::size_t index, std::uint32_t word) {
    const auto a = (float((word >> quaternionHighShift) & middleMask) / middleMask - .5f) * std::sqrt(2.f);
    const auto b = (float((word >> quaternionMidShift) & middleMask) / middleMask - .5f) * std::sqrt(2.f);
    const auto c = (float(word & middleMask) / middleMask - .5f) * std::sqrt(2.f);
    const auto missing = std::sqrt(std::max(0.f, 1.f - a * a - b * b - c * c));
    std::array<float, 4> xyzw{};
    switch (word >> quaternionShift) {
    case 0:
        xyzw = {a, b, c, missing};
        break;
    case 1:
        xyzw = {missing, b, c, a};
        break;
    case 2:
        xyzw = {b, missing, c, a};
        break;
    default:
        xyzw = {b, c, missing, a};
        break;
    }
    // The scene stores wxyz while the packed encoding orders xyzw.
    scene.rotations[index * 4] = xyzw[3];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        scene.rotations[index * 4 + axis + 1] = xyzw[axis];
    }
}

void unpackVertex(StaticScene& scene,
                  std::size_t index,
                  const std::array<std::uint32_t, 4>& words,
                  const std::array<float, chunkComponents>& bounds) {
    const auto position = unpackTriple(words[0]);
    const auto scale = unpackTriple(words[2]);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto channel = float((words[3] >> (24 - 8 * axis)) & byteMask) / byteMask;
        scene.positions[index * 3 + axis] = std::lerp(bounds[axis], bounds[3 + axis], position[axis]);
        scene.logScales[index * 3 + axis] =
            std::lerp(bounds[scaleMinOffset + axis], bounds[scaleMaxOffset + axis], scale[axis]);
        scene.sh0[index * 3 + axis] =
            (std::lerp(bounds[colorMinOffset + axis], bounds[colorMaxOffset + axis], channel) - .5f) / shDc;
    }
    scene.opacityLogits[index] = logit(float(words[3] & byteMask) / byteMask);
    unpackRotation(scene, index, words[1]);
}

std::vector<std::array<float, chunkComponents>> readChunks(Cursor& cursor, const Element& chunks) {
    constexpr std::array<std::string_view, chunkComponents> names = {"min_x",
                                                                     "min_y",
                                                                     "min_z",
                                                                     "max_x",
                                                                     "max_y",
                                                                     "max_z",
                                                                     "min_scale_x",
                                                                     "min_scale_y",
                                                                     "min_scale_z",
                                                                     "max_scale_x",
                                                                     "max_scale_y",
                                                                     "max_scale_z",
                                                                     "min_r",
                                                                     "min_g",
                                                                     "min_b",
                                                                     "max_r",
                                                                     "max_g",
                                                                     "max_b"};
    require(chunks.properties.size() == colorMinOffset || chunks.properties.size() == chunkComponents,
            "Unsupported compressed PLY chunk layout");
    std::vector<int> mapping(chunks.properties.size());
    for (std::size_t j = 0; j < mapping.size(); ++j) {
        const auto index = propertyIndex(chunks, names[j]);
        require(index >= 0 && chunks.properties[index].type == Scalar::f32,
                "Invalid compressed PLY chunk property");
        mapping[index] = static_cast<int>(j);
    }
    std::vector<std::array<float, chunkComponents>> frames(chunks.count);
    for (auto& frame : frames) {
        std::fill(frame.begin() + colorMaxOffset, frame.end(), 1.f);
        for (std::size_t j = 0; j < mapping.size(); ++j) {
            const auto value = static_cast<float>(cursor.property(chunks.properties[j]));
            require(std::isfinite(value), "Nonfinite PLY chunk");
            frame[mapping[j]] = value;
        }
    }
    return frames;
}

void readPackedVertices(Cursor& cursor,
                        const Element& vertex,
                        const std::vector<std::array<float, chunkComponents>>& frames,
                        StaticScene& scene) {
    constexpr std::array<std::string_view, 4> names = {
        "packed_position", "packed_rotation", "packed_scale", "packed_color"};
    require(vertex.properties.size() == names.size(), "Invalid compressed PLY vertex");
    std::array<int, 4> mapping{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto index = propertyIndex(vertex, names[i]);
        require(index >= 0 && vertex.properties[index].type == Scalar::u32, "Invalid packed PLY property");
        mapping[index] = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < vertex.count; ++i) {
        std::array<std::uint32_t, 4> words{};
        for (std::size_t j = 0; j < names.size(); ++j) {
            words[mapping[j]] = static_cast<std::uint32_t>(cursor.property(vertex.properties[j]));
        }
        unpackVertex(scene, i, words, frames[i / chunkSize]);
    }
}

void readCompressedSh(Cursor& cursor, const Element& sh, StaticScene& scene) {
    const auto rest = scene.shRestCoefficients() * 3;
    require(sh.properties.size() == rest, "Invalid compressed PLY SH properties");
    std::vector<std::size_t> targets(rest);
    for (std::size_t i = 0; i < rest; ++i) {
        const auto property = propertyIndex(sh, "f_rest_" + std::to_string(i));
        require(property >= 0 && sh.properties[property].type == Scalar::u8,
                "Invalid compressed PLY SH property");
        targets[property] = (i % (rest / 3)) * 3 + i / (rest / 3);
    }
    for (std::size_t i = 0; i < scene.count(); ++i) {
        for (std::size_t j = 0; j < rest; ++j) {
            const auto code = static_cast<unsigned>(cursor.property(sh.properties[j]));
            const auto normalized = code == 0 ? 0.f : code == byteMask ? 1.f : (float(code) + .5f) / 256.f;
            scene.shRest[i * rest + targets[j]] = (normalized - .5f) * compressedShRange;
        }
    }
}

StaticScene compressed(Cursor& cursor, const Header& header, DecodeContext& context, const Element& vertex) {
    const auto* chunks = findElement(header, "chunk");
    const auto* sh = findElement(header, "sh");
    const auto rest = sh ? restCount(*sh) : 0;
    require(chunks && chunks == &header.elements.front() &&
                chunks->count == (vertex.count + chunkSize - 1) / chunkSize,
            "Invalid compressed PLY chunks");
    require(header.elements.size() == (sh ? 3uz : 2uz) && &header.elements[1] == &vertex &&
                (!sh || (sh == &header.elements.back() && sh->count == vertex.count)),
            "Invalid compressed PLY elements");
    auto scene = context.createScene(vertex.count, shDegreeForRest(rest), "Compressed PLY");
    WorkingReservation framesMemory(
        context, checkedProduct(chunks->count, sizeof(std::array<float, chunkComponents>)));
    const auto frames = readChunks(cursor, *chunks);
    readPackedVertices(cursor, vertex, frames, scene);
    if (sh) {
        readCompressedSh(cursor, *sh, scene);
    }
    return scene;
}
}

StaticScene readPly(std::span<const std::byte> data, DecodeContext& context) {
    require(data.size() >= 4 && std::memcmp(data.data(), "ply", 3) == 0, "Not a PLY file");
    WorkingReservation headerMemory(context, std::min(headerByteLimit, data.size()) * headerWorkingFactor);
    const auto header = readHeader(data, context.limits);
    const auto* vertex = findElement(header, "vertex");
    require(vertex && vertex->count, "Missing PLY Gaussian vertices");
    Cursor cursor(data, header);
    StaticScene scene{};
    if (findElement(header, "chunk")) {
        scene = compressed(cursor, header, context, *vertex);
    } else {
        const auto rest = restCount(*vertex);
        const auto targets = ordinaryTargets(*vertex, rest);
        scene = context.createScene(
            vertex->count, shDegreeForRest(rest), header.ascii ? "ASCII PLY" : "Binary PLY");
        for (const auto& element : header.elements) {
            if (element.properties.empty()) {
                continue;
            }
            for (std::size_t i = 0; i < element.count; ++i) {
                for (std::size_t j = 0; j < element.properties.size(); ++j) {
                    const auto value = cursor.property(element.properties[j]);
                    if (&element == vertex) {
                        storeOrdinary(scene, i, targets[j], static_cast<float>(value));
                    }
                }
            }
        }
    }
    cursor.finish();
    scene.antialiased = header.antialiased;
    return scene;
}
}
