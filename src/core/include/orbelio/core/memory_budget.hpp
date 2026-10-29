#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace orbelio {
class MemoryBudget {
    struct State {
        std::atomic_size_t used{0};
        std::size_t limit = 0;

        explicit State(std::size_t bytes) : limit(bytes) {}
    };

public:
    class Reservation {
    public:
        Reservation() = default;

        ~Reservation() { reset(); }

        Reservation(Reservation&& other) noexcept
            : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                bytes_ = std::exchange(other.bytes_, 0);
            }
            return *this;
        }

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        std::size_t bytes() const { return bytes_; }

        void shrinkTo(std::size_t bytes) {
            if (bytes > bytes_) {
                throw std::logic_error("Cannot grow a memory reservation");
            }
            if (state_) {
                state_->used.fetch_sub(bytes_ - bytes);
            }
            bytes_ = bytes;
        }

        void reset() noexcept {
            if (state_) {
                state_->used.fetch_sub(bytes_);
            }
            state_.reset();
            bytes_ = 0;
        }

    private:
        friend class MemoryBudget;

        Reservation(std::shared_ptr<State> state, std::size_t bytes)
            : state_(std::move(state)), bytes_(bytes) {}

        std::shared_ptr<State> state_;
        std::size_t bytes_ = 0;
    };

    explicit MemoryBudget(std::size_t bytes) : state_(std::make_shared<State>(bytes)) {}

    Reservation reserve(std::size_t bytes) const {
        auto used = state_->used.load();
        do {
            if (bytes > state_->limit - used) {
                throw std::runtime_error("CPU model/result memory budget exceeded");
            }
        } while (!state_->used.compare_exchange_weak(used, used + bytes));
        return Reservation(state_, bytes);
    }

    std::size_t usedBytes() const { return state_->used.load(); }

    std::size_t limitBytes() const { return state_->limit; }

private:
    std::shared_ptr<State> state_;
};

struct ModelLimits {
    static constexpr std::size_t mebibyte = 1024 * 1024;
    std::size_t inputBytes = 256 * mebibyte;
    std::size_t decodedBytes = 512 * mebibyte;
    std::size_t residentBytes = 1024 * mebibyte;
    std::size_t gpuBytes = 1024 * mebibyte;
    std::size_t maxGaussians = 8'000'000;
    std::size_t sequenceCacheBytes = 256 * mebibyte;
};
}
