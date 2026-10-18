#pragma once

#include "types.hpp"
#include <memory>
#include <span>

namespace orbelio {

// Move-only leases keep the storage alive even if its public pool owner goes away.
// Thread safe: an IO lease may be transferred to the render executor.
class BytePool {
    struct State;

public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        std::span<std::byte> bytes() const;

        explicit operator bool() const { return static_cast<bool>(state_); }

    private:
        friend class BytePool;
        Lease(std::shared_ptr<State> state, std::size_t slot, std::size_t size);
        void release();
        std::shared_ptr<State> state_;
        std::size_t slot_ = 0;
        std::size_t size_ = 0;
    };

    explicit BytePool(std::size_t budgetBytes);
    Lease acquire(std::size_t bytes); // throws on invalid size or budget exhaustion
    PoolStats stats() const;
    void trim(); // releases only unused storage
private:
    std::shared_ptr<State> state_;
};

} // namespace orbelio
