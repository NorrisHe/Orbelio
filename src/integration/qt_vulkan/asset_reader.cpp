#include "asset_reader.hpp"
#include <QFile>
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
        throw std::runtime_error("Compressed PLY file exceeds the configured read budget");
    }
    auto bytes = pool.acquire(static_cast<std::size_t>(size));
    if (file.read(reinterpret_cast<char*>(bytes.bytes().data()), size) != size) {
        throw std::runtime_error("Incomplete model read");
    }
    return bytes;
}
}
