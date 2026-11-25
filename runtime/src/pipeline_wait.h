#pragma once
// How long a submission waits for pipelines that are still being built (docs/d3d12_vulkan.md, "Pipelines still being
// built"). Before VulkanQueue::execute takes any lock of ours it asks for the pipelines its lists' draws and dispatches
// need (ComputePipeline::await, GraphicsPipeline::await) and waits for the ones being built, at most:
//   LEONIDA_PIPELINE_WAIT_MS        per submission, in all. 0: never waits; -1: waits for every build to finish (the
//                                   behaviour before this existed, without a lock held). Unset: 24 ms with
//                                   LEONIDA_ASYNC_PIPELINES=1, -1 without (nothing is skipped in the default mode).
//   LEONIDA_PIPELINE_WAIT_FRAME_MS  per frame (between two presents, or one second without a present), shared by every
//                                   submitting thread (default 100). -1: no frame budget.
// A draw or dispatch whose pipeline is still not built when the wait ends is skipped by replay (recorded as nothing);
// the build goes on, and a later submission uses the pipeline. LEONIDA_STRICT_REPLAY=1 waits for every build. The
// budget bounds waits for builds other threads run: without asynchronous builds, a pipeline nobody has started is
// built by the submitting thread in place, however long that takes (with no lock of the replay layer held).
// Every 10 s while anything happened, one line counts the pipelines waited for, the time, the draws and dispatches
// skipped and the pipelines replay asked for that are still being built.
// No windows.h here: vulkan_replay.h includes this, and translation units without NOMINMAX include that.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace leonida::pipeline_wait {

// Times are steady-clock nanoseconds (submit_phases::now_ns). ComputePipeline::await / GraphicsPipeline::await read
// `until` as: `probe` - the handle if built, never starting anything; a time already past (`no_wait`) - start the build
// when nobody has, without waiting for one another thread runs; a later time - and wait for it until then; `forever`.
// Their `pending` answer says whether a null handle means "not built yet" (false: built, or it cannot be built).
inline constexpr std::int64_t probe = 0;
inline constexpr std::int64_t no_wait = 1;
inline constexpr std::int64_t forever = (std::numeric_limits<std::int64_t>::max)();

// The clock of submit_phases::now_ns.
inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int env_ms(const char* name, int fallback) {
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text) return fallback;
    return value < 0 ? -1 : int((std::min<long>)(value, 600000));
}

// Milliseconds; -1 for no limit. Atomics so a test can change them (set_budgets).
inline constexpr int unset = -2;
inline std::atomic<int>& submission_ms() {
    static std::atomic<int> value{env_ms("LEONIDA_PIPELINE_WAIT_MS", unset)};
    return value;
}
// What an unset LEONIDA_PIPELINE_WAIT_MS means; the D3D12 device sets it (24 with asynchronous builds, else -1).
inline std::atomic<int>& default_submission_ms() {
    static std::atomic<int> value{24};
    return value;
}
inline int effective_submission_ms() {
    const int value = submission_ms().load(std::memory_order_relaxed);
    return value == unset ? default_submission_ms().load(std::memory_order_relaxed) : value;
}
inline std::atomic<int>& frame_ms() {
    static std::atomic<int> value{env_ms("LEONIDA_PIPELINE_WAIT_FRAME_MS", 100)};
    return value;
}
inline void set_budgets(int submission, int frame) {
    submission_ms().store(submission);
    frame_ms().store(frame);
}

// Presents, counted by the swap chain: a frame's budget starts again at each one.
inline std::atomic<std::uint64_t>& presents() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}
inline void note_present() { presents().fetch_add(1, std::memory_order_relaxed); }

// What a waiting thread took of its frame's budget (take), and gives back unused (give_back).
struct Grant {
    std::int64_t ns = 0;
    std::uint64_t frame = 0;
    bool limited = false;  // false: the frame budget is off and nothing was taken from it
};

struct FrameBudget {
    std::mutex mutex;
    std::uint64_t frame = ~0ull;  // the present count the budget belongs to
    std::int64_t started = 0;     // when it began: one second without a present starts a new one
    std::int64_t spent = 0;
};
inline FrameBudget& frame_budget() {
    static FrameBudget value;
    return value;
}

// Up to `want` ns of the current frame's budget, which the caller may wait; the rest of the frame's budget stays for
// the other submitting threads.
inline Grant take(std::int64_t want) {
    const int budget = frame_ms().load(std::memory_order_relaxed);
    if (budget < 0) return {want, 0, false};
    FrameBudget& f = frame_budget();
    std::lock_guard lock(f.mutex);
    const std::uint64_t frame = presents().load(std::memory_order_relaxed);
    const std::int64_t now = now_ns();
    if (frame != f.frame || now - f.started >= 1000000000) {
        f.frame = frame;
        f.started = now;
        f.spent = 0;
    }
    const std::int64_t left = (std::max<std::int64_t>)(0, std::int64_t(budget) * 1000000 - f.spent);
    const std::int64_t granted = (std::min)(want, left);
    f.spent += granted;
    return {granted, f.frame, true};
}
inline void give_back(const Grant& grant, std::int64_t unused) {
    if (!grant.limited || unused <= 0) return;
    FrameBudget& f = frame_budget();
    std::lock_guard lock(f.mutex);
    if (f.frame == grant.frame) f.spent = (std::max<std::int64_t>)(0, f.spent - (std::min)(unused, grant.ns));
}

// The last submission (VulkanQueue::execute). While the title submits, compute warm-ups of big modules are left to
// their first use (LEONIDA_COMPUTE_WARMUP_LIVE_MAX_BYTES, runtime/shims/d3d12_vulkan_device.cpp): a create holds the
// compile gate for its whole length, and every submission waits behind it.
inline std::atomic<std::int64_t>& last_submission() {
    static std::atomic<std::int64_t> value{0};
    return value;
}
inline void note_submission() { last_submission().store(now_ns(), std::memory_order_relaxed); }
inline bool title_submitting(std::int64_t within_ns = 1000000000) {
    const std::int64_t last = last_submission().load(std::memory_order_relaxed);
    return last && now_ns() - last < within_ns;
}

struct Stats {
    std::atomic<std::uint64_t> waited{0};    // pipelines a submission waited for
    std::atomic<std::uint64_t> wait_ns{0};   // the time it waited for them
    std::atomic<std::uint64_t> late{0};      // waits that ended before the pipeline was built
    std::atomic<std::uint64_t> skipped_draws{0};
    std::atomic<std::uint64_t> skipped_dispatches{0};
    std::atomic<std::int64_t> building{0};   // pipelines replay asked for that are still being built
    std::atomic<std::uint64_t> warmups_left{0};  // big warm-ups left to their first use while the title submits
    std::atomic<std::int64_t> last_report{0};
};
inline Stats& stats() {
    static Stats value;
    return value;
}

inline void count_skip(bool dispatch) {
    (dispatch ? stats().skipped_dispatches : stats().skipped_draws).fetch_add(1, std::memory_order_relaxed);
}

// The 10 s line. One thread wins it; counted since the last line. `age` gives the process's age in seconds
// (submit_phases::process_age), read only when the line is printed.
template<class Age> void maybe_report(Age&& age) {
    constexpr std::int64_t period = 10000000000;
    Stats& s = stats();
    const std::int64_t now = now_ns();
    std::int64_t last = s.last_report.load(std::memory_order_relaxed);
    if (!last) {
        s.last_report.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < period || !s.last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const std::uint64_t waited = s.waited.exchange(0, std::memory_order_relaxed);
    const std::uint64_t wait_ns = s.wait_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t late = s.late.exchange(0, std::memory_order_relaxed);
    const std::uint64_t draws = s.skipped_draws.exchange(0, std::memory_order_relaxed);
    const std::uint64_t dispatches = s.skipped_dispatches.exchange(0, std::memory_order_relaxed);
    const std::int64_t building = s.building.load(std::memory_order_relaxed);
    const std::uint64_t left = s.warmups_left.exchange(0, std::memory_order_relaxed);
    if (!waited && !draws && !dispatches && building <= 0 && !left) return;
    std::fprintf(stderr,
                 "[rt] pipeline waits t=%.0fs: in %.1f s %llu pipelines waited for (%.0f ms in all, %llu not built in "
                 "time), %llu draws and %llu dispatches skipped, %lld pipelines still building; budget %d ms per "
                 "submission, %d ms per frame; %llu big warm-ups left to first use\n",
                 double(age()), double(now - last) * 1e-9, static_cast<unsigned long long>(waited),
                 double(wait_ns) * 1e-6, static_cast<unsigned long long>(late), static_cast<unsigned long long>(draws),
                 static_cast<unsigned long long>(dispatches), static_cast<long long>(building),
                 effective_submission_ms(), frame_ms().load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(left));
}

}  // namespace leonida::pipeline_wait
