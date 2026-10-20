#include "asset_reader.hpp"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <stdexcept>

namespace orbelio {
BytePool::Lease QtAssetReader::read(std::string_view asset, BytePool& pool, std::size_t limitBytes) {
    QThread::currentThread()->setObjectName("Orbelio.IO");
    QFile file(QString::fromUtf8(asset.data(), static_cast<qsizetype>(asset.size())));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    const auto size = file.size();
    if (size <= 0 || static_cast<std::uint64_t>(size) > limitBytes) {
        throw std::runtime_error("Model file exceeds the configured read budget");
    }
    auto bytes = pool.acquire(static_cast<std::size_t>(size));
    if (file.read(reinterpret_cast<char*>(bytes.bytes().data()), size) != size) {
        throw std::runtime_error("Incomplete model read");
    }
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
