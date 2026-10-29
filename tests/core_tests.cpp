#include <orbelio/core/byte_pool.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

static void activeStorageSurvivesPoolGrowth() {
    constexpr std::size_t slotBytes = 4096;
    constexpr std::size_t additionalLeases = 64;
    orbelio::BytePool pool(slotBytes * (additionalLeases + 1));
    auto first = pool.acquire(16);
    auto* address = first.bytes().data();
    first.bytes()[0] = std::byte{42};
    std::vector<orbelio::BytePool::Lease> leases;
    for (std::size_t index = 0; index < additionalLeases; ++index) {
        leases.push_back(pool.acquire(16));
    }
    require(first.bytes().data() == address && first.bytes()[0] == std::byte{42},
            "Growing the pool moved active byte storage");
    leases.clear();
    pool.trim();
    require(pool.stats().activeBytes == slotBytes && first.bytes().data() == address,
            "Trimming other slots invalidated an active lease");
}

int main() {
    try {
        activeStorageSurvivesPoolGrowth();
        orbelio::BytePool pool(8192);
        auto first = pool.acquire(4000);
        auto* address = first.bytes().data();
        first.bytes()[0] = std::byte{42};
        auto moved = std::move(first);
        require(!first && moved.bytes()[0] == std::byte{42}, "Move lost ownership or data");
        pool.trim();
        require(pool.stats().reservedBytes == 4096, "Trim reclaimed an active lease");
        moved = {};
        auto second = pool.acquire(128);
        require(second.bytes().data() == address, "Expected reuse of released storage");
        require(pool.stats().allocations == 1 && pool.stats().reuses == 1, "Incorrect pool counters");
        bool rejected = false;
        try {
            auto oversized = pool.acquire(8192);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "Budget must account for active leases");
        second = {};
        auto grown = pool.acquire(8192);
        require(pool.stats().reservedBytes == 8192, "Idle storage was not evicted for growth");
        grown = {};
        pool.trim();
        require(pool.stats().activeBytes == 0 && pool.stats().reservedBytes == 0,
                "Pool did not release idle storage");
        orbelio::BytePool::Lease surviving;
        {
            orbelio::BytePool temporary(4096);
            surviving = temporary.acquire(16);
        }
        surviving.bytes()[0] = std::byte{7};
        require(surviving.bytes()[0] == std::byte{7}, "Lease did not outlive public pool owner");
        std::cout << "CPU pool lifecycle, reuse, budget, trim and lease lifetime: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
