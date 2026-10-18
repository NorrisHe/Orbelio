#pragma once

#include "vma.hpp"
#include <orbelio/core/types.hpp>
#include <vector>

namespace orbelio {
// Render-thread-only pool. Caller releases a lease only after GPU use completes.
class GpuBufferPool {
public:
    struct Lease {
        std::size_t slot;
        VkBuffer buffer;
        VkDeviceSize capacity;
        std::uint64_t generation;
    };

    GpuBufferPool(VmaAllocator allocator, std::size_t budget);
    ~GpuBufferPool();
    GpuBufferPool(const GpuBufferPool&) = delete;
    GpuBufferPool& operator=(const GpuBufferPool&) = delete;
    Lease acquire(VkDeviceSize bytes, VkBufferUsageFlags usage);
    void release(Lease lease);

    PoolStats stats() const {
        guard_.check();
        return stats_;
    }

private:
    struct Entry {
        VkBuffer buffer;
        VmaAllocation allocation;
        VkDeviceSize capacity;
        VkBufferUsageFlags usage;
        bool used;
        std::uint64_t generation;
    };

    // Best fit: the smallest idle buffer that covers both the size and every requested usage bit.
    std::size_t findReusableSlot(VkDeviceSize capacity, VkBufferUsageFlags usage) const;
    std::size_t findVacantSlot() const;
    Lease reuseSlot(std::size_t slot);
    Lease allocate(VkDeviceSize capacity, VkBufferUsageFlags usage);
    void createHandles(Entry& entry);
    // Copies an allocated entry into the table; destroys the buffer if the table cannot grow.
    std::size_t insertEntry(const Entry& entry);
    void discardIdleEntries();
    // Frees the VkBuffer back to VMA and empties the entry, making the slot vacant.
    void recycle(Entry& entry);

    RenderThreadGuard guard_;
    VmaAllocator allocator_;
    std::size_t budget_;
    std::vector<Entry> entries_;
    PoolStats stats_;
    std::uint64_t nextGeneration_ = 1;
};
} // namespace orbelio
