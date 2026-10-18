#include <orbelio/core/performance.hpp>
#include <limits>
#include <stdexcept>
#include <iostream>

static void require(bool ok) {
    if (!ok) {
        throw std::runtime_error("Metric contract failed");
    }
}

int main() {
    orbelio::RollingMetric metric(20);
    require(!metric.mean() && !metric.percentile(.95));
    for (int i = 1; i <= 20; ++i) {
        metric.add(i);
    }
    require(metric.percentile(.5) == 10 && metric.percentile(.95) == 19 && metric.mean() == 10.5);
    metric.add(21);
    require(metric.percentile(.5) == 11 && metric.count() == 20);
    for (int i = 0; i < 20; ++i) {
        metric.add(std::nullopt);
    }
    require(metric.count() == 0 && !metric.percentile(.95));
    metric.add(std::numeric_limits<double>::quiet_NaN());
    metric.add(-1);
    metric.add(0);
    require(metric.count() == 1 && metric.mean() == 0);
    bool rejected = false;
    try {
        orbelio::RollingMetric invalid(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected);
    std::cout << "Rolling expiry, nearest rank, invalid and unavailable samples: PASS\n";
}
