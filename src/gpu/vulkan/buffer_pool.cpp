#include "buffer_pool.hpp"
#include <algorithm>
#include <limits>

namespace orbelio {
namespace {
constexpr VkDeviceSize slotGranularityBytes = 4096;

VkDeviceSize roundedCapacity(VkDeviceSize bytes) {
    constexpr VkDeviceSize roundingRemainder = slotGranularityBytes - 1;
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - roundingRemainder) {
        throw std::invalid_argument("Invalid GPU pool request");
    }
    return ((bytes + roundingRemainder) / slotGranularityBytes) * slotGranularityBytes;
}
} // namespace

GpuBufferPool::GpuBufferPool(VmaAllocator allocator, std::size_t budget)
    : allocator_(allocator), budget_(budget) {}

GpuBufferPool::~GpuBufferPool() {
    for (const auto& entry : entries_) {
        vmaDestroyBuffer(allocator_, entry.buffer, entry.allocation);
    }
}

GpuBufferPool::Lease GpuBufferPool::acquire(VkDeviceSize bytes, VkBufferUsageFlags usage) {
    guard_.check();
    const auto capacity = roundedCapacity(bytes);
    const std::size_t reusable = findReusableSlot(capacity, usage);
    return reusable == entries_.size() ? allocate(capacity, usage) : reuseSlot(reusable);
}

void GpuBufferPool::release(Lease lease) {
    guard_.check();
    auto& entry = entries_.at(lease.slot);
    if (!entry.used || entry.buffer != lease.buffer || entry.generation != lease.generation) {
        throw std::logic_error("Invalid or stale GPU buffer release");
    }
    entry.used = false;
    stats_.activeBytes -= entry.capacity;
}

std::size_t GpuBufferPool::findReusableSlot(VkDeviceSize capacity, VkBufferUsageFlags usage) const {
    std::size_t best = entries_.size();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        if (entry.used || !entry.buffer) {
            continue;
        }
        if (entry.capacity < capacity || (entry.usage & usage) != usage) {
            continue;
        }
        if (best == entries_.size() || entry.capacity < entries_[best].capacity) {
            best = i;
        }
    }
    return best;
}

std::size_t GpuBufferPool::findVacantSlot() const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].buffer) {
            return i;
        }
    }
    return entries_.size();
}

GpuBufferPool::Lease GpuBufferPool::reuseSlot(std::size_t slot) {
    auto& entry = entries_[slot];
    entry.used = true;
    entry.generation = nextGeneration_++;
    stats_.activeBytes += entry.capacity;
    ++stats_.reuses;
    return {slot, entry.buffer, entry.capacity, entry.generation};
}

GpuBufferPool::Lease GpuBufferPool::allocate(VkDeviceSize capacity, VkBufferUsageFlags usage) {
    if (capacity > budget_) {
        throw std::runtime_error("GPU pool request exceeds budget");
    }
    if (capacity > budget_ - stats_.reservedBytes) {
        discardIdleEntries();
    }
    if (capacity > budget_ - stats_.reservedBytes) {
        throw std::runtime_error("GPU pool budget exhausted by active resources");
    }
    Entry entry{.capacity = capacity, .usage = usage, .used = true, .generation = nextGeneration_++};
    createHandles(entry);
    const std::size_t slot = insertEntry(entry);
    stats_.activeBytes += capacity;
    stats_.reservedBytes += capacity;
    stats_.peakBytes = std::max(stats_.peakBytes, stats_.reservedBytes);
    ++stats_.allocations;
    return {slot, entry.buffer, capacity, entry.generation};
}

void GpuBufferPool::createHandles(Entry& entry) {
    auto info = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = entry.capacity;
    info.usage = entry.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vkCheck(vmaCreateBuffer(allocator_, &info, &allocationInfo, &entry.buffer, &entry.allocation, nullptr),
            "vmaCreateBuffer");
}

std::size_t GpuBufferPool::insertEntry(const Entry& entry) {
    const std::size_t slot = findVacantSlot();
    try {
        if (slot == entries_.size()) {
            entries_.push_back(entry);
        } else {
            entries_[slot] = entry;
        }
    } catch (...) {
        vmaDestroyBuffer(allocator_, entry.buffer, entry.allocation);
        throw;
    }
    return slot;
}

void GpuBufferPool::discardIdleEntries() {
    for (auto& entry : entries_) {
        if (!entry.used && entry.buffer) {
            recycle(entry);
        }
    }
}

void GpuBufferPool::recycle(Entry& entry) {
    vmaDestroyBuffer(allocator_, entry.buffer, entry.allocation);
    stats_.reservedBytes -= entry.capacity;
    entry = {};
}
} // namespace orbelio
