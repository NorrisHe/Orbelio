#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace orbelio {
// Nearest-rank percentiles; absent/invalid samples never become artificial zeros.
class RollingMetric {
public:
    explicit RollingMetric(std::size_t capacity = 120) : samples_(capacity) {
        if (!capacity) {
            throw std::invalid_argument("Metric window must be nonzero");
        }
    }

    void add(std::optional<double> value) {
        if (value && (!std::isfinite(*value) || *value < 0)) {
            value.reset();
        }
        if (samples_[cursor_]) {
            sum_ -= *samples_[cursor_];
            --validCount_;
        }
        samples_[cursor_] = value;
        if (value) {
            sum_ += *value;
            ++validCount_;
        }
        cursor_ = (cursor_ + 1) % samples_.size();
    }

    std::size_t count() const { return validCount_; }

    std::optional<double> mean() const {
        return validCount_ ? std::optional<double>(sum_ / static_cast<double>(validCount_)) : std::nullopt;
    }

    std::optional<double> percentile(double p) const {
        std::vector<double> sorted;
        for (auto sample : samples_) {
            if (sample) {
                sorted.push_back(*sample);
            }
        }
        if (sorted.empty()) {
            return {};
        }
        std::sort(sorted.begin(), sorted.end());
        const auto rank =
            static_cast<std::size_t>(std::ceil(std::clamp(p, 0., 1.) * static_cast<double>(sorted.size())));
        return sorted[rank ? rank - 1 : 0];
    }

private:
    std::vector<std::optional<double>> samples_;
    std::size_t cursor_ = 0;
    std::size_t validCount_ = 0;
    double sum_ = 0;
};
}
