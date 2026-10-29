#include <orbelio/core/byte_pool.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbelio {
namespace {
constexpr std::size_t slotGranularityBytes = 4096;

std::size_t roundedCapacity(std::size_t bytes) {
    constexpr std::size_t roundingRemainder = slotGranularityBytes - 1;
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - roundingRemainder) {
        throw std::invalid_argument("Invalid byte-pool request");
    }
    return ((bytes + roundingRemainder) / slotGranularityBytes) * slotGranularityBytes;
}
} // namespace

// Every method below is called with mutex already held by BytePool.
struct BytePool::State {
    struct Slot {
        std::unique_ptr<std::byte[]> data;
        std::size_t capacity = 0;
        bool used = false;
    };

    std::mutex mutex;
    std::vector<Slot> slots;
    std::size_t budget;
    PoolStats counters;

    explicit State(std::size_t bytes) : budget(bytes) {}

    // Best fit: the smallest idle slot big enough for the request, earliest index on ties.
    std::size_t findReusableSlot(std::size_t capacity) const {
        std::size_t best = slots.size();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& slot = slots[i];
            if (slot.used || !slot.data || slot.capacity < capacity) {
                continue;
            }
            if (best == slots.size() || slot.capacity < slots[best].capacity) {
                best = i;
            }
        }
        return best;
    }

    // A slot vector entry without storage is the one place a fresh allocation may land.
    std::size_t findVacantSlot() const {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].used && !slots[i].data) {
                return i;
            }
        }
        return slots.size();
    }

    void forget(Slot& slot) {
        counters.reservedBytes -= slot.capacity;
        slot.data.reset();
        slot.capacity = 0;
    }

    void forgetIdleSlots() {
        for (auto& slot : slots) {
            if (!slot.used && slot.data) {
                forget(slot);
            }
        }
    }

    std::size_t reserveSlot(std::size_t capacity) {
        if (capacity > budget) {
            throw std::runtime_error("CPU pool budget exceeded");
        }
        if (capacity > budget - counters.reservedBytes) {
            forgetIdleSlots();
        }
        if (capacity > budget - counters.reservedBytes) {
            throw std::runtime_error("CPU pool budget exceeded by active leases");
        }
        Slot fresh{std::make_unique<std::byte[]>(capacity), capacity};
        const std::size_t index = findVacantSlot();
        if (index == slots.size()) {
            slots.push_back(std::move(fresh));
        } else {
            slots[index] = std::move(fresh);
        }
        counters.reservedBytes += capacity;
        ++counters.allocations;
        return index;
    }
};

BytePool::BytePool(std::size_t budgetBytes) : state_(std::make_shared<State>(budgetBytes)) {}

BytePool::Lease::Lease(std::shared_ptr<State> state, std::size_t slot, std::size_t size)
    : state_(std::move(state)), slot_(slot), size_(size) {}

BytePool::Lease::~Lease() { release(); }

BytePool::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_), size_(std::exchange(other.size_, 0)) {}

BytePool::Lease& BytePool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void BytePool::Lease::release() {
    if (!state_) {
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        auto& slot = state_->slots[slot_];
        slot.used = false;
        state_->counters.activeBytes -= slot.capacity;
    }
    state_.reset();
    size_ = 0;
}

std::span<std::byte> BytePool::Lease::bytes() const {
    if (!state_) {
        return {};
    }
    std::lock_guard lock(state_->mutex);
    return {state_->slots[slot_].data.get(), size_};
}

BytePool::Lease BytePool::acquire(std::size_t bytes) {
    const std::size_t capacity = roundedCapacity(bytes);
    std::lock_guard lock(state_->mutex);
    std::size_t slotIndex = state_->findReusableSlot(capacity);
    if (slotIndex == state_->slots.size()) {
        slotIndex = state_->reserveSlot(capacity);
    } else {
        ++state_->counters.reuses;
    }
    auto& slot = state_->slots[slotIndex];
    slot.used = true;
    state_->counters.activeBytes += slot.capacity;
    state_->counters.peakBytes = std::max(state_->counters.peakBytes, state_->counters.reservedBytes);
    return Lease(state_, slotIndex, bytes);
}

PoolStats BytePool::stats() const {
    std::lock_guard lock(state_->mutex);
    return state_->counters;
}

void BytePool::trim() {
    std::lock_guard lock(state_->mutex);
    state_->forgetIdleSlots();
}
} // namespace orbelio
