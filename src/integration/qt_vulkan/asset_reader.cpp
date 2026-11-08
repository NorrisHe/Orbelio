#include "asset_reader.hpp"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <stdexcept>

namespace orbelio {
namespace {
class QtAssetStream final : public IAssetStream {
public:
    explicit QtAssetStream(std::string_view asset)
        : file_(QString::fromUtf8(asset.data(), static_cast<qsizetype>(asset.size()))) {
        if (!file_.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(file_.errorString().toStdString());
        }
        const auto size = file_.size();
        if (size <= 0 || !std::in_range<std::size_t>(size)) {
            throw std::runtime_error("Invalid model file size");
        }
        size_ = static_cast<std::size_t>(size);
    }

    std::size_t size() const override { return size_; }

    void read(std::span<std::byte> destination) override {
        const auto size = static_cast<qint64>(destination.size());
        if (file_.read(reinterpret_cast<char*>(destination.data()), size) != size) {
            throw std::runtime_error("Incomplete model read");
        }
    }

private:
    QFile file_;
    std::size_t size_ = 0;
};
}

std::unique_ptr<IAssetStream> QtAssetReader::openStream(std::string_view asset) {
    QThread::currentThread()->setObjectName("Orbelio.IO");
    return std::make_unique<QtAssetStream>(asset);
}

BytePool::Lease QtAssetReader::read(std::string_view asset, BytePool& pool, std::size_t limitBytes) {
    const auto stream = openStream(asset);
    if (stream->size() > limitBytes) {
        throw std::runtime_error("Model file exceeds the configured read budget");
    }
    auto bytes = pool.acquire(stream->size());
    stream->read(bytes.bytes());
    return bytes;
}

BytePool::Lease QtAssetReader::readRelated(std::string_view asset,
                                           std::string_view relative,
                                           BytePool& pool,
                                           std::size_t limitBytes) {
    const auto source = QString::fromUtf8(asset.data(), qsizetype(asset.size()));
    const auto child = QString::fromUtf8(relative.data(), qsizetype(relative.size()));
    if (!QDir::isRelativePath(child) || child.contains(':') || child.contains('\\')) {
        throw std::runtime_error("SOG texture must be a relative local path");
    }
    const auto parent = QFileInfo(source).canonicalPath();
    const auto path = QFileInfo(QDir(parent).filePath(child)).canonicalFilePath();
    const auto inside = QDir(parent).relativeFilePath(path);
    if (parent.isEmpty() || path.isEmpty() || inside == ".." || inside.startsWith("../") ||
        !QDir::isRelativePath(inside)) {
        throw std::runtime_error("Missing SOG texture or path escapes model directory");
    }
    return read(path.toUtf8().toStdString(), pool, limitBytes);
}

}
