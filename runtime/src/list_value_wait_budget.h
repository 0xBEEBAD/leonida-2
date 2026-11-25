#pragma once
// Work-aware timeout clock and cycle-safe producer dependency analysis for console command-list memory waits
// (runtime/shims/d3d12_xs_list_values.h, docs/d3d12_xs_console_abi.md §5.5).
//
// A memory wait gate needs a bounded timeout so an unmodelled write (one the runtime never lands) cannot hang a queue
// forever. Counting wall-clock time blindly cuts waits short whenever the runtime is busy running held work (e.g.
// multi-second CPU shadow/texture copies inside the releaser) or while an in-flight producer write has not finished on
// the GPU yet, running dependent queues out of order.
//
// This header separates the policy from Windows/D3D12 handles so it can be verified portably under sanitizers:
// - `pause_reason()` decides whether submitted or held work can still satisfy a gate without creating a deadlock cycle:
//   1. `releaser_busy`: held work from an already-opened gate is currently being released/submitted;
//   2. `matching_producer`: an in-flight WriteGroup whose final write to a waited address can satisfy an unmet wait, and
//      every pending gate preceding that WriteGroup is cycle-free with respect to the waiting queue;
//   3. `active_work`: unrelated unblocked GPU work (a WriteGroup or action gate with no pending gate ahead of it) is
//      still executing, paused only up to a finite cap so a continuously streaming queue cannot starve an unmodelled wait.
// - `GateTimeoutState` tracks idle wait time (`elapsed_us`) separately from paused time, expiring only once `elapsed_us`
//   reaches `budget_us`.
//
// Pure C++20 standard library only.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace leonida::list_value_wait {

enum class PauseReason : std::uint8_t {
    none = 0,               // no submitted or held work can still satisfy this gate -> timeout clock ticks
    releaser_busy = 1,      // releaser thread is executing or draining opened gates
    matching_producer = 2,  // an acyclic in-flight WriteGroup can satisfy an unmet wait of this gate
    active_work = 3,        // unrelated unblocked submitted work is still executing (bounded by active_pause_cap_us)
};

struct WaitPredicate {
    std::uint64_t address = 0;
    std::uint64_t reference = 0;
    std::uint32_t comparison = 0;
    bool wide = false;
};

struct PendingWrite {
    std::uint64_t address = 0;
    std::uint64_t value = 0;
    std::uint8_t bytes = 0;
    bool timestamp = false;  // GPU/system timestamp: final value is unknown until landing
};

struct PendingWriteGroup {
    const void* queue = nullptr;
    std::uint64_t after_gate = 0;  // highest pending gate value on `queue` ahead of this group when submitted (0: none)
    std::span<const PendingWrite> writes;
};

struct PendingGate {
    const void* queue = nullptr;
    std::uint64_t value = 0;
    bool is_action = false;                      // waits only on earlier GPU work on `queue`, never on memory
    std::span<const WaitPredicate> unmet_waits;  // waits not currently satisfied in memory (empty for action gates)
};

// Default cap on pausing solely for unrelated unblocked GPU work: 300x the idle timeout, clamped to [1 s, 10 min].
// At the default 200 ms timeout this allows up to 60 s for slow world-phase frames while preventing an unrelated
// continuously streaming queue from starving an unmodelled wait indefinitely.
inline constexpr std::uint64_t default_active_pause_cap_us(unsigned full_timeout_ms) {
    const std::uint64_t base_ms = full_timeout_ms ? std::uint64_t{full_timeout_ms} : 1u;
    const std::uint64_t scaled_ms = base_ms > 2000u ? 600000u : base_ms * 300u;
    const std::uint64_t clamped_ms = scaled_ms < 1000u ? 1000u : (scaled_ms > 600000u ? 600000u : scaled_ms);
    return clamped_ms * 1000u;
}

struct GateTimeoutState {
    std::uint64_t budget_us = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t active_paused_us = 0;
    std::uint64_t active_pause_cap_us = 0;
    bool paused_ever = false;

    void reset(std::uint64_t budget, std::uint64_t active_cap) {
        budget_us = budget;
        elapsed_us = 0;
        active_paused_us = 0;
        active_pause_cap_us = active_cap;
        paused_ever = false;
    }

    // Advances the gate by `dt_us` under `reason`. Returns true once the idle timeout budget is exhausted.
    bool advance(std::uint64_t dt_us, PauseReason reason) {
        if (reason == PauseReason::releaser_busy || reason == PauseReason::matching_producer) {
            paused_ever = true;
            return false;
        }
        if (reason == PauseReason::active_work && active_paused_us < active_pause_cap_us) {
            paused_ever = true;
            const std::uint64_t room = active_pause_cap_us - active_paused_us;
            const std::uint64_t absorb = dt_us < room ? dt_us : room;
            active_paused_us += absorb;
            dt_us -= absorb;
        }
        if (dt_us > UINT64_MAX - elapsed_us)
            elapsed_us = UINT64_MAX;
        else
            elapsed_us += dt_us;
        return elapsed_us >= budget_us;
    }
};

namespace detail {

// True when `group`'s final write touching [wait.address, wait.address + need) lands a value that meets `wait`
// (or is a timestamp write to the exact address whose runtime value is unknown until landing).
template <class MetFn>
inline bool write_group_matches_wait(const PendingWriteGroup& group, const WaitPredicate& wait, MetFn&& met) {
    const std::uint64_t need = wait.wide ? 8u : 4u;
    if (!wait.address || wait.address > UINT64_MAX - need) return false;
    const std::uint64_t wait_end = wait.address + need;
    for (std::size_t i = group.writes.size(); i-- > 0;) {
        const PendingWrite& w = group.writes[i];
        if (!w.bytes || w.address > UINT64_MAX - w.bytes) continue;
        const std::uint64_t write_end = w.address + w.bytes;
        if (w.address >= wait_end || wait.address >= write_end) continue;
        if (w.address == wait.address && w.bytes >= need &&
            (w.timestamp || met(wait.comparison, w.value, wait.reference, wait.wide)))
            return true;
        // A later write in the same group overwrote or partially overlapped the waited range with a non-matching value.
        return false;
    }
    return false;
}

// True when no pending gate in `gates` precedes `(queue, after_gate)`.
inline bool is_unblocked(std::span<const PendingGate> gates, const void* queue, std::uint64_t after_gate) {
    if (!after_gate) return true;
    for (const PendingGate& g : gates)
        if (g.queue == queue && g.value <= after_gate) return false;
    return true;
}

// True when `gate_idx` (directly or transitively via matching in-flight writes) depends on a gate on `target_queue`.
template <class MetFn>
inline bool gate_can_reach_queue(std::size_t gate_idx, const void* target_queue,
                                 std::span<const PendingWriteGroup> writes, std::span<const PendingGate> gates,
                                 MetFn&& met, std::vector<std::size_t>& visiting) {
    const PendingGate& g = gates[gate_idx];
    if (g.queue == target_queue) return true;
    if (g.is_action) return false;
    if (std::find(visiting.begin(), visiting.end(), gate_idx) != visiting.end()) return false;
    visiting.push_back(gate_idx);
    for (const WaitPredicate& wait : g.unmet_waits) {
        for (const PendingWriteGroup& w : writes) {
            if (!write_group_matches_wait(w, wait, met)) continue;
            if (!w.after_gate) continue;
            for (std::size_t b = 0; b < gates.size(); ++b) {
                if (gates[b].queue != w.queue || gates[b].value > w.after_gate) continue;
                if (gates[b].queue == target_queue ||
                    gate_can_reach_queue(b, target_queue, writes, gates, met, visiting)) {
                    visiting.pop_back();
                    return true;
                }
            }
        }
    }
    visiting.pop_back();
    return false;
}

}  // namespace detail

// Decides whether the timeout clock of `gates[target_idx]` should pause on this pump pass.
// `met(comparison, value, reference, wide)` must implement the same predicate as `console_value_met`.
template <class MetFn>
inline PauseReason pause_reason(bool releaser_busy, std::span<const PendingWriteGroup> writes,
                                std::span<const PendingGate> gates, std::size_t target_idx, MetFn&& met) {
    if (releaser_busy) return PauseReason::releaser_busy;
    if (target_idx >= gates.size()) return PauseReason::none;
    const PendingGate& target = gates[target_idx];
    if (target.is_action) return PauseReason::none;

    // 1. A matching in-flight WriteGroup whose preceding gates (if any) do not cycle back to `target.queue`.
    std::vector<std::size_t> visiting;
    for (const WaitPredicate& wait : target.unmet_waits) {
        for (const PendingWriteGroup& w : writes) {
            if (!detail::write_group_matches_wait(w, wait, met)) continue;
            bool cyclic = false;
            if (w.after_gate) {
                for (std::size_t b = 0; b < gates.size(); ++b) {
                    if (gates[b].queue != w.queue || gates[b].value > w.after_gate) continue;
                    visiting.clear();
                    if (gates[b].queue == target.queue ||
                        detail::gate_can_reach_queue(b, target.queue, writes, gates, met, visiting)) {
                        cyclic = true;
                        break;
                    }
                }
            }
            if (!cyclic) return PauseReason::matching_producer;
        }
    }

    // 2. Unrelated unblocked submitted GPU work (an unblocked WriteGroup on any queue, or an unblocked action gate on
    //    another queue) that could still write memory via a shader/copy or unblock a CPU producer.
    for (const PendingWriteGroup& w : writes) {
        if (detail::is_unblocked(gates, w.queue, w.after_gate)) return PauseReason::active_work;
    }
    for (const PendingGate& g : gates) {
        if (!g.is_action || g.queue == target.queue) continue;
        if (g.value == 0 || detail::is_unblocked(gates, g.queue, g.value - 1)) return PauseReason::active_work;
    }
    return PauseReason::none;
}

}  // namespace leonida::list_value_wait
