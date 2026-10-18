#pragma once
// Optional upstream diagnostics sink. Orbelio reports actual renderer allocation
// sizes/timestamps through its own panel; no fabricated profiler measurements.
namespace lfs::diagnostics {
class VramProfiler {
public:
    static VramProfiler& instance() { static VramProfiler value; return value; }
    template<class... T> void recordStaticBytes(T&&...) {}
    template<class... T> void recordCurrentBytes(T&&...) {}
    template<class... T> void clearStaticScope(T&&...) {}
    template<class... T> void recordTimerSample(T&&...) {}
};
}
