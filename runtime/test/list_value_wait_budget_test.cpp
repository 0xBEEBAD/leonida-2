// Portable CPU-only unit tests for runtime/src/list_value_wait_budget.h (RT-LIST-VALUE-WAIT-WORK-AWARE-TIMEOUT):
// work-aware timeout pausing, cycle detection across multi-queue write/gate chains, overwrite-aware write matching,
// action gates, and bounded unrelated-work pausing. No Windows, D3D12, or Vulkan headers; no game files.
#include "../src/list_value_wait_budget.h"

#include <cstdio>
#include <vector>

#define CHECK(cond) do {                                                        \
    if (!(cond)) {                                                              \
        std::fprintf(stderr, "line %d: CHECK failed: %s\n", __LINE__, #cond);   \
        return 1;                                                               \
    }                                                                           \
} while (0)

namespace {

constexpr bool test_met(std::uint32_t comparison, std::uint64_t value, std::uint64_t reference, bool wide) {
    if (!wide) {
        value &= 0xffffffffu;
        reference &= 0xffffffffu;
    }
    switch (comparison) {
    case 2: return value < reference;
    case 3: return value == reference;
    case 4: return value <= reference;
    case 5: return value > reference;
    case 6: return value != reference;
    case 7: return value >= reference;
    default: return true;
    }
}

}  // namespace

int main() {
    using namespace leonida::list_value_wait;

    int q1_tag = 0, q2_tag = 0, q3_tag = 0;
    const void* const q1 = &q1_tag;
    const void* const q2 = &q2_tag;
    const void* const q3 = &q3_tag;

    // 1. Idle unmodelled wait: PauseReason::none -> expires at exact budget_us.
    {
        const WaitPredicate waits[] = {{0x1000, 5, 7, true}};  // >= 5
        const PendingGate gates[] = {{q1, 1, false, waits}};
        CHECK(pause_reason(false, {}, gates, 0, test_met) == PauseReason::none);

        GateTimeoutState t;
        t.reset(200'000, default_active_pause_cap_us(200));
        CHECK(!t.advance(100'000, PauseReason::none));
        CHECK(t.elapsed_us == 100'000 && !t.paused_ever);
        CHECK(!t.advance(99'999, PauseReason::none));
        CHECK(t.advance(1, PauseReason::none));
        CHECK(t.elapsed_us == 200'000);
    }

    // 2. Releaser actively executing held work pauses the timeout clock indefinitely without burning idle budget.
    {
        const WaitPredicate waits[] = {{0x1000, 5, 7, true}};
        const PendingGate gates[] = {{q1, 1, false, waits}};
        CHECK(pause_reason(true, {}, gates, 0, test_met) == PauseReason::releaser_busy);

        GateTimeoutState t;
        t.reset(200'000, default_active_pause_cap_us(200));
        // 30 seconds of releaser executing shadow/texture copies burns 0 us of idle timeout budget.
        for (int i = 0; i < 30; ++i) CHECK(!t.advance(1'000'000, PauseReason::releaser_busy));
        CHECK(t.elapsed_us == 0 && t.paused_ever);
        // Once idle again, still gets its full 200 ms budget before expiring.
        CHECK(!t.advance(199'000, PauseReason::none));
        CHECK(t.advance(1'000, PauseReason::none));
    }

    // 3. Matching unblocked producer write on another queue (Wait-before-Signal) -> PauseReason::matching_producer.
    {
        const WaitPredicate waits[] = {{0x2000, 10, 7, true}};  // >= 10
        const PendingWrite w_ok[] = {{0x2000, 12, 8, false}};
        const PendingWriteGroup groups[] = {{q2, 0, w_ok}};
        const PendingGate gates[] = {{q1, 1, false, waits}};
        CHECK(pause_reason(false, groups, gates, 0, test_met) == PauseReason::matching_producer);

        // Timestamp write to the same address also matches (final value unknown until landing).
        const PendingWrite w_ts[] = {{0x2000, 0, 8, true}};
        const PendingWriteGroup ts_groups[] = {{q2, 0, w_ts}};
        CHECK(pause_reason(false, ts_groups, gates, 0, test_met) == PauseReason::matching_producer);

        // Matching write overwritten later in the same WriteGroup by a non-matching value does NOT match,
        // falling back to active_work (since q2's group is unblocked).
        const PendingWrite w_overwritten[] = {{0x2000, 12, 8, false}, {0x2000, 3, 8, false}};
        const PendingWriteGroup ow_groups[] = {{q2, 0, w_overwritten}};
        CHECK(pause_reason(false, ow_groups, gates, 0, test_met) == PauseReason::active_work);

        // Partial overlap by a later write in the same group invalidates the earlier matching write.
        const PendingWrite w_partial[] = {{0x2000, 12, 8, false}, {0x2004, 0, 4, false}};
        const PendingWriteGroup part_groups[] = {{q2, 0, w_partial}};
        CHECK(pause_reason(false, part_groups, gates, 0, test_met) == PauseReason::active_work);

        // 4-byte write cannot satisfy an 8-byte (wide) wait.
        const PendingWrite w_narrow[] = {{0x2000, 12, 4, false}};
        const PendingWriteGroup nar_groups[] = {{q2, 0, w_narrow}};
        CHECK(pause_reason(false, nar_groups, gates, 0, test_met) == PauseReason::active_work);

        // 8-byte write CAN satisfy a 4-byte wait (low 32 bits compared).
        const WaitPredicate wait32[] = {{0x2000, 10, 7, false}};
        const PendingWrite w_hi_lo[] = {{0x2000, 0x100000000ull | 12ull, 8, false}};
        const PendingWriteGroup hilo_groups[] = {{q2, 0, w_hi_lo}};
        const PendingGate gates32[] = {{q1, 1, false, wait32}};
        CHECK(pause_reason(false, hilo_groups, gates32, 0, test_met) == PauseReason::matching_producer);
    }

    // 4. Same-queue ordering: a write submitted BEFORE the gate (after_gate < gate.value) IS a live producer;
    //    a write submitted AFTER the gate (after_gate >= gate.value) is a self-deadlock and MUST NOT pause the gate.
    {
        const WaitPredicate waits[] = {{0x3000, 1, 7, true}};
        const PendingWrite w[] = {{0x3000, 1, 8, false}};
        const PendingGate gates[] = {{q1, 2, false, waits}};

        const PendingWriteGroup before_gate[] = {{q1, 1, w}};  // after_gate 1 < gate.value 2 (unblocked!)
        CHECK(pause_reason(false, before_gate, gates, 0, test_met) == PauseReason::matching_producer);

        const PendingWriteGroup behind_gate[] = {{q1, 2, w}};  // after_gate 2 == gate.value 2 (held by itself!)
        CHECK(pause_reason(false, behind_gate, gates, 0, test_met) == PauseReason::none);
    }

    // 5. Mutual 2-queue cycle (Q1 waits on Q2's write behind G2; Q2 waits on Q1's write behind G1):
    //    NEITHER gate pauses, so the cycle breaks cleanly instead of deadlocking.
    {
        const WaitPredicate w1[] = {{0x4000, 1, 7, true}};
        const WaitPredicate w2[] = {{0x5000, 1, 7, true}};
        const PendingWrite pw1[] = {{0x5000, 1, 8, false}};  // Q1 writes what Q2 waits for, behind Q1's gate 1
        const PendingWrite pw2[] = {{0x4000, 1, 8, false}};  // Q2 writes what Q1 waits for, behind Q2's gate 1
        const PendingWriteGroup groups[] = {{q1, 1, pw1}, {q2, 1, pw2}};
        const PendingGate gates[] = {{q1, 1, false, w1}, {q2, 1, false, w2}};
        CHECK(pause_reason(false, groups, gates, 0, test_met) == PauseReason::none);
        CHECK(pause_reason(false, groups, gates, 1, test_met) == PauseReason::none);
    }

    // 6. 3-queue cycle involving a SECOND (non-front) gate on Q2:
    //    Q1 gate 1 waits on W2 (behind Q2 gates 1 & 2); Q2 gate 2 waits on W1 (behind Q1 gate 1).
    //    Checking all gates <= after_gate catches this indirect cycle!
    {
        const WaitPredicate w_q1[] = {{0x6000, 1, 7, true}};
        const WaitPredicate w_q2_g1[] = {{0x6100, 1, 7, true}};  // unrelated front gate on Q2
        const WaitPredicate w_q2_g2[] = {{0x6200, 1, 7, true}};  // second gate on Q2 waits on Q1
        const PendingWrite pw_q1[] = {{0x6200, 1, 8, false}};    // behind Q1 gate 1
        const PendingWrite pw_q2[] = {{0x6000, 1, 8, false}};    // behind Q2 gate 2 (after_gate = 2)
        const PendingWriteGroup groups[] = {{q1, 1, pw_q1}, {q2, 2, pw_q2}};
        const PendingGate gates[] = {
            {q1, 1, false, w_q1},
            {q2, 1, false, w_q2_g1},
            {q2, 2, false, w_q2_g2},
        };
        CHECK(pause_reason(false, groups, gates, 0, test_met) == PauseReason::none);
        CHECK(pause_reason(false, groups, gates, 2, test_met) == PauseReason::none);
    }

    // 7. Acyclic 3-queue chain (Q1 -> Q2 -> Q3 where Q3's gate has no producer):
    //    Q3 does NOT pause (times out first), while Q2 and Q1 wait for their upstream producers to run in order!
    {
        const WaitPredicate w_q1[] = {{0x7000, 1, 7, true}};
        const WaitPredicate w_q2[] = {{0x7100, 1, 7, true}};
        const WaitPredicate w_q3[] = {{0x7200, 1, 7, true}};  // unmodelled wait on Q3
        const PendingWrite pw_q2[] = {{0x7000, 1, 8, false}};  // Q2 feeds Q1 (behind Q2 gate 1)
        const PendingWrite pw_q3[] = {{0x7100, 1, 8, false}};  // Q3 feeds Q2 (behind Q3 gate 1)
        const PendingWriteGroup groups[] = {{q2, 1, pw_q2}, {q3, 1, pw_q3}};
        const PendingGate gates[] = {
            {q1, 1, false, w_q1},
            {q2, 1, false, w_q2},
            {q3, 1, false, w_q3},
        };
        CHECK(pause_reason(false, groups, gates, 0, test_met) == PauseReason::matching_producer);
        CHECK(pause_reason(false, groups, gates, 1, test_met) == PauseReason::matching_producer);
        CHECK(pause_reason(false, groups, gates, 2, test_met) == PauseReason::none);
    }

    // 8. Action gate on another queue counts as active_work when unblocked, and does NOT count when blocked behind a
    //    memory gate on its own queue.
    {
        const WaitPredicate w_q1[] = {{0x8000, 1, 7, true}};
        const WaitPredicate w_q2[] = {{0x8100, 1, 7, true}};
        const PendingGate unblocked_action[] = {
            {q1, 1, false, w_q1},
            {q2, 1, true, {}},  // unblocked action gate on Q2
        };
        CHECK(pause_reason(false, {}, unblocked_action, 0, test_met) == PauseReason::active_work);

        const PendingGate blocked_action[] = {
            {q1, 1, false, w_q1},
            {q2, 1, false, w_q2},  // memory gate blocking Q2
            {q2, 2, true, {}},     // action gate behind Q2's memory gate
        };
        CHECK(pause_reason(false, {}, blocked_action, 0, test_met) == PauseReason::none);
    }

    // 9. Bounded unrelated active_work pause: pauses up to active_pause_cap_us, then drains idle budget_us cleanly.
    {
        CHECK(default_active_pause_cap_us(200) == 60'000'000ull);   // 60 s at default 200 ms
        CHECK(default_active_pause_cap_us(1) == 1'000'000ull);      // 1 s floor
        CHECK(default_active_pause_cap_us(30000) == 600'000'000ull);  // 10 min ceiling

        GateTimeoutState t;
        t.reset(200'000, 5'000'000);  // 200 ms budget, 5 s unrelated-work cap
        // 5 full seconds of unrelated active work exhausts the pause cap, leaving 0 elapsed.
        for (int i = 0; i < 5; ++i) CHECK(!t.advance(1'000'000, PauseReason::active_work));
        CHECK(t.active_paused_us == 5'000'000 && t.elapsed_us == 0 && t.paused_ever);
        // Next 200 ms of unrelated work now counts against idle budget and expires the gate.
        CHECK(!t.advance(199'000, PauseReason::active_work));
        CHECK(t.advance(1'000, PauseReason::active_work));
        CHECK(t.elapsed_us == 200'000);

        // Partial step straddling the cap splits cleanly between active_paused_us and elapsed_us.
        GateTimeoutState t2;
        t2.reset(200'000, 500'000);
        CHECK(!t2.advance(600'000, PauseReason::active_work));  // 500k paused, 100k elapsed
        CHECK(t2.active_paused_us == 500'000 && t2.elapsed_us == 100'000);
        CHECK(t2.advance(100'000, PauseReason::active_work));
    }

    // 10. Overflow-safe address / dt_us guards.
    {
        const WaitPredicate overflow_wait[] = {{UINT64_MAX - 2, 1, 7, true}};
        const PendingWrite overflow_write[] = {{UINT64_MAX - 2, 1, 8, false}};
        const PendingWriteGroup groups[] = {{q2, 0, overflow_write}};
        const PendingGate gates[] = {{q1, 1, false, overflow_wait}};
        // Write doesn't match due to address wrap guard, falls back to active_work for the unblocked group.
        CHECK(pause_reason(false, groups, gates, 0, test_met) == PauseReason::active_work);

        GateTimeoutState t;
        t.reset(1000, 0);
        CHECK(t.advance(UINT64_MAX, PauseReason::none));
        CHECK(t.elapsed_us == UINT64_MAX);
    }

    std::printf("list_value_wait_budget_test: all checks passed\n");
    return 0;
}
