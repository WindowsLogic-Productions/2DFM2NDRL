// Spectator playback queue mechanics.
//
// Mirror of the queue logic in spectator_node.cpp (PopFrameInputs +
// IsPlayingBack drain semantics) so we can test it without the Windows
// socket deps the production .cpp drags in.

#include "doctest.h"

#include <cstdint>
#include <vector>

namespace mirror {

struct PlaybackFrame {
    uint16_t p1_input;
    uint16_t p2_input;
};

// Same shape as g_state in spectator_node.cpp, minus the host-side fields.
struct SpectatorQueue {
    bool                       playing_back  = false;
    std::vector<PlaybackFrame> queue;
    uint16_t                   current_p1    = 0;
    uint16_t                   current_p2    = 0;

    // Mirror SpectatorNode_HandleJoinAck behavior.
    void OnJoinAck() {
        playing_back = true;
        queue.clear();
        current_p1 = current_p2 = 0;
    }

    // Mirror HandleSpecData::INPUT_BATCH.
    void EnqueueBatch(const PlaybackFrame* frames, size_t n) {
        for (size_t i = 0; i < n; i++) queue.push_back(frames[i]);
    }

    // Mirror HandleSpecData::MATCH_END (does NOT clear queue — drains naturally).
    void OnMatchEnd() {
        playing_back = false;
    }

    // Mirror SpectatorNode_PopFrameInputs.
    bool PopFrameInputs(uint16_t* p1, uint16_t* p2) {
        if (queue.empty()) return false;
        PlaybackFrame f = queue.front();
        queue.erase(queue.begin());
        current_p1 = f.p1_input;
        current_p2 = f.p2_input;
        if (p1) *p1 = f.p1_input;
        if (p2) *p2 = f.p2_input;
        return true;
    }

    // Mirror SpectatorNode_IsPlayingBack — true while either still
    // subscribed OR queue still has frames to drain.
    bool IsPlayingBack() const {
        return playing_back || !queue.empty();
    }

    size_t PendingFrameCount() const { return queue.size(); }
};

}  // namespace mirror

TEST_CASE("Subscribe with no inputs yet: playing_back true, queue empty") {
    mirror::SpectatorQueue q;
    q.OnJoinAck();
    CHECK(q.IsPlayingBack());
    CHECK(q.PendingFrameCount() == 0);
    uint16_t p1, p2;
    CHECK_FALSE(q.PopFrameInputs(&p1, &p2));
}

TEST_CASE("PopFrameInputs caches per-frame values for repeated reads") {
    // The hook layer (Hook_GetPlayerInput) is called multiple times per sim
    // tick (once per slot per get-input call inside the sim). PopFrameInputs
    // is called once by the trampoline; subsequent reads via
    // GetCurrentP1Input/P2Input should return the cached pair.
    mirror::SpectatorQueue q;
    q.OnJoinAck();
    mirror::PlaybackFrame in[] = {{0x111, 0x222}, {0x333, 0x444}};
    q.EnqueueBatch(in, 2);

    uint16_t p1, p2;
    CHECK(q.PopFrameInputs(&p1, &p2));
    CHECK(p1 == 0x111);
    CHECK(p2 == 0x222);

    // Cached for repeated GetCurrent reads — multiple Hook_GetPlayerInput
    // calls within the same sim tick all see the same value.
    CHECK(q.current_p1 == 0x111);
    CHECK(q.current_p2 == 0x222);
}

TEST_CASE("Drains in order, FIFO") {
    mirror::SpectatorQueue q;
    q.OnJoinAck();
    mirror::PlaybackFrame in[] = {{0xA, 0xB}, {0xC, 0xD}, {0xE, 0xF}};
    q.EnqueueBatch(in, 3);
    CHECK(q.PendingFrameCount() == 3);

    uint16_t p1, p2;
    CHECK(q.PopFrameInputs(&p1, &p2));
    CHECK(p1 == 0xA); CHECK(p2 == 0xB);
    CHECK(q.PopFrameInputs(&p1, &p2));
    CHECK(p1 == 0xC); CHECK(p2 == 0xD);
    CHECK(q.PopFrameInputs(&p1, &p2));
    CHECK(p1 == 0xE); CHECK(p2 == 0xF);
    CHECK(q.PendingFrameCount() == 0);
    CHECK_FALSE(q.PopFrameInputs(&p1, &p2));
}

TEST_CASE("MATCH_END does NOT clear queue — drain semantics") {
    // Final buffered frames must be visible after MATCH_END so the player
    // sees the actual last frame of the match, not a snap-cut.
    mirror::SpectatorQueue q;
    q.OnJoinAck();
    mirror::PlaybackFrame in[] = {{1,1}, {2,2}, {3,3}};
    q.EnqueueBatch(in, 3);

    q.OnMatchEnd();
    CHECK_FALSE(q.playing_back);  // flag flipped
    CHECK(q.PendingFrameCount() == 3);  // queue intact

    // IsPlayingBack remains true until the queue drains.
    CHECK(q.IsPlayingBack());
    uint16_t p1, p2;
    while (q.PopFrameInputs(&p1, &p2)) {}
    CHECK_FALSE(q.IsPlayingBack());  // both flag false AND queue empty
}

TEST_CASE("Multi-batch: streaming append while draining works") {
    mirror::SpectatorQueue q;
    q.OnJoinAck();

    // First batch arrives.
    mirror::PlaybackFrame a[] = {{1,1}, {2,2}};
    q.EnqueueBatch(a, 2);
    CHECK(q.PendingFrameCount() == 2);

    uint16_t p1, p2;
    q.PopFrameInputs(&p1, &p2);  // drain one
    CHECK(q.PendingFrameCount() == 1);

    // Second batch arrives mid-drain.
    mirror::PlaybackFrame b[] = {{3,3}, {4,4}, {5,5}};
    q.EnqueueBatch(b, 3);
    CHECK(q.PendingFrameCount() == 4);

    // Should drain in order: 2, 3, 4, 5
    uint16_t expected[] = {2, 3, 4, 5};
    for (int i = 0; i < 4; i++) {
        CHECK(q.PopFrameInputs(&p1, &p2));
        CHECK(p1 == expected[i]);
        CHECK(p2 == expected[i]);
    }
    CHECK_FALSE(q.PopFrameInputs(&p1, &p2));
}

TEST_CASE("Fast-forward threshold: large queue triggers catch-up") {
    // Trampoline policy: if PendingFrameCount() > 30, sleep 1ms (fast-fwd).
    // Test that the boundary condition lines up where we expect.
    mirror::SpectatorQueue q;
    q.OnJoinAck();
    constexpr size_t FAST_FORWARD_THRESHOLD = 30;

    // Just under threshold → live pacing.
    std::vector<mirror::PlaybackFrame> warm(FAST_FORWARD_THRESHOLD, {0, 0});
    q.EnqueueBatch(warm.data(), warm.size());
    CHECK_FALSE(q.PendingFrameCount() > FAST_FORWARD_THRESHOLD);

    // Just over threshold → fast-fwd.
    mirror::PlaybackFrame extra[] = {{0, 0}, {0, 0}};
    q.EnqueueBatch(extra, 2);
    CHECK(q.PendingFrameCount() > FAST_FORWARD_THRESHOLD);
}

TEST_CASE("Backfill scale: 1 hour of session catches up cleanly") {
    // 1 hr * 100 Hz = 360000 frames. Ensure mass-enqueue + mass-drain works.
    mirror::SpectatorQueue q;
    q.OnJoinAck();

    constexpr size_t TOTAL = 360000;
    std::vector<mirror::PlaybackFrame> hist(TOTAL, {0xABCD, 0x1234});
    q.EnqueueBatch(hist.data(), hist.size());
    CHECK(q.PendingFrameCount() == TOTAL);

    uint16_t p1, p2;
    size_t drained = 0;
    while (q.PopFrameInputs(&p1, &p2)) {
        CHECK(p1 == 0xABCD);
        CHECK(p2 == 0x1234);
        drained++;
    }
    CHECK(drained == TOTAL);
}

// =============================================================================
// SessionEvent queue: head-of-queue non-INPUT drain (C2)
// =============================================================================
//
// Mirror of the new pb_queue behavior in spectator_node.cpp. The production
// PopFrameInputs drains non-INPUT events at the head of the queue before
// popping the next INPUT — this is the gate that lets C3+ apply ops
// (PIN_RNG, RESET_INPUT_STATE, SOUND_INIT, MATCH_START) at the exact moment
// the input that follows them is consumed by the sim. C2 leaves the apply
// path as a no-op (silently drops the event); the test pins the drain
// semantic so it can't regress.

namespace mirror_eq {

enum class EventType : uint8_t {
    INPUT             = 1,
    PIN_RNG           = 2,
    RESET_INPUT_STATE = 3,
    SOUND_INIT        = 4,
    MATCH_START       = 5,
    MATCH_END         = 6,
    FINGERPRINT       = 7,
};

struct Event {
    EventType type;
    uint16_t  p1 = 0;
    uint16_t  p2 = 0;
    uint32_t  payload = 0;  // PIN_RNG seed / FINGERPRINT hash / unused
};

struct EventQueue {
    std::vector<Event> q;
    uint16_t           current_p1 = 0;
    uint16_t           current_p2 = 0;
    // Side-channel: count non-INPUT events dispatched (C3 will count
    // applies; C2 just counts drops).
    size_t             non_input_drained = 0;

    bool PopInput(uint16_t* p1, uint16_t* p2) {
        // Drain non-INPUT events at head.
        while (!q.empty() && q.front().type != EventType::INPUT) {
            ++non_input_drained;
            q.erase(q.begin());
        }
        if (q.empty()) return false;
        Event ev = q.front();
        q.erase(q.begin());
        current_p1 = ev.p1;
        current_p2 = ev.p2;
        if (p1) *p1 = ev.p1;
        if (p2) *p2 = ev.p2;
        return true;
    }
};

}  // namespace mirror_eq

TEST_CASE("Head-drain: PopInput skips non-INPUT events at head, returns next INPUT") {
    using namespace mirror_eq;
    EventQueue q;
    q.q.push_back({EventType::PIN_RNG,           0, 0, 0x12345678});
    q.q.push_back({EventType::RESET_INPUT_STATE, 0, 0, 0});
    q.q.push_back({EventType::SOUND_INIT,        0, 0, 0});
    q.q.push_back({EventType::INPUT,             0x111, 0x222, 0});

    uint16_t p1 = 0, p2 = 0;
    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 0x111);
    CHECK(p2 == 0x222);
    CHECK(q.non_input_drained == 3);
    CHECK(q.q.empty());
}

TEST_CASE("Head-drain: queue with only non-INPUT events returns false") {
    using namespace mirror_eq;
    EventQueue q;
    q.q.push_back({EventType::PIN_RNG, 0, 0, 0xDEADBEEF});
    q.q.push_back({EventType::SOUND_INIT, 0, 0, 0});

    uint16_t p1 = 0, p2 = 0;
    CHECK_FALSE(q.PopInput(&p1, &p2));
    CHECK(q.non_input_drained == 2);
    CHECK(q.q.empty());
}

TEST_CASE("Head-drain: ops between INPUTs are applied at the right boundary") {
    // Stream shape that C3+ will emit at battle entry:
    //   [INPUT(css_last), PIN_RNG, RESET_INPUT_STATE, SOUND_INIT, INPUT(battle_first)]
    // Spectator drains: pop css_last → run sim → pop battle_first → BEFORE
    // popping it, drain the three non-INPUTs (apply RNG pin etc.) → run sim
    // with cleanly-initialized state. This is the property under test.
    using namespace mirror_eq;
    EventQueue q;
    q.q.push_back({EventType::INPUT,             0xCC0, 0xCC1, 0});  // css_last
    q.q.push_back({EventType::PIN_RNG,           0, 0, 0x12345678});
    q.q.push_back({EventType::RESET_INPUT_STATE, 0, 0, 0});
    q.q.push_back({EventType::SOUND_INIT,        0, 0, 0});
    q.q.push_back({EventType::INPUT,             0xBB0, 0xBB1, 0});  // battle_first

    uint16_t p1 = 0, p2 = 0;
    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 0xCC0); CHECK(p2 == 0xCC1);
    CHECK(q.non_input_drained == 0);  // first INPUT was at the head — no drain

    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 0xBB0); CHECK(p2 == 0xBB1);
    CHECK(q.non_input_drained == 3);  // PIN_RNG + RESET + SOUND_INIT drained before battle_first
    CHECK(q.q.empty());
}

TEST_CASE("Head-drain: MATCH_END and MATCH_START at head are also drained") {
    using namespace mirror_eq;
    EventQueue q;
    q.q.push_back({EventType::MATCH_END, 0, 0, 0});
    q.q.push_back({EventType::MATCH_START, 0, 0, 0});
    q.q.push_back({EventType::INPUT, 0xAA, 0xBB, 0});

    uint16_t p1 = 0, p2 = 0;
    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 0xAA); CHECK(p2 == 0xBB);
    CHECK(q.non_input_drained == 2);
}

TEST_CASE("Apply order: ops at head are applied BEFORE the next INPUT pops (C3)") {
    // Production drain dispatches each non-INPUT event to ApplySessionEvent
    // (writes RNG, calls SoundRollback::Init, etc.) BEFORE popping the next
    // INPUT event. Mirror that here with a side-effect counter to pin the
    // ordering invariant: when PopInput returns true, all preceding head
    // ops must have already executed.
    using namespace mirror_eq;

    struct ApplyTrace {
        std::vector<EventType> applied_in_order;
        uint16_t               sim_p1 = 0xFFFF;
        uint16_t               sim_p2 = 0xFFFF;
    };

    ApplyTrace trace;

    auto pop_with_apply = [&](EventQueue& q, uint16_t* p1, uint16_t* p2) -> bool {
        while (!q.q.empty() && q.q.front().type != EventType::INPUT) {
            trace.applied_in_order.push_back(q.q.front().type);
            ++q.non_input_drained;
            q.q.erase(q.q.begin());
        }
        if (q.q.empty()) return false;
        Event ev = q.q.front();
        q.q.erase(q.q.begin());
        if (p1) *p1 = ev.p1;
        if (p2) *p2 = ev.p2;
        // Simulate the sim "consuming" the input.
        trace.sim_p1 = ev.p1;
        trace.sim_p2 = ev.p2;
        return true;
    };

    EventQueue q;
    q.q.push_back({EventType::PIN_RNG,           0, 0, 0xDEAD});
    q.q.push_back({EventType::INPUT,             0xAA, 0xBB, 0});

    // Before pop: nothing applied, sim hasn't consumed.
    CHECK(trace.applied_in_order.empty());
    CHECK(trace.sim_p1 == 0xFFFF);

    uint16_t p1 = 0, p2 = 0;
    CHECK(pop_with_apply(q, &p1, &p2));

    // After pop: PIN_RNG was applied, THEN the input was consumed.
    REQUIRE(trace.applied_in_order.size() == 1);
    CHECK(trace.applied_in_order[0] == EventType::PIN_RNG);
    CHECK(trace.sim_p1 == 0xAA);  // sim consumed AFTER apply
    CHECK(trace.sim_p2 == 0xBB);
}

TEST_CASE("Apply order: full battle-entry op chain applies in stream order") {
    // Realistic Netplay_StartBattle stream:
    //   PIN_RNG(0x12345678) → RESET_INPUT_STATE → SOUND_INIT → first battle INPUT
    using namespace mirror_eq;

    std::vector<EventType> trace;
    auto pop_with_apply = [&](EventQueue& q, uint16_t* p1, uint16_t* p2) -> bool {
        while (!q.q.empty() && q.q.front().type != EventType::INPUT) {
            trace.push_back(q.q.front().type);
            ++q.non_input_drained;
            q.q.erase(q.q.begin());
        }
        if (q.q.empty()) return false;
        Event ev = q.q.front();
        q.q.erase(q.q.begin());
        if (p1) *p1 = ev.p1;
        if (p2) *p2 = ev.p2;
        return true;
    };

    EventQueue q;
    q.q.push_back({EventType::PIN_RNG,           0, 0, 0x12345678});
    q.q.push_back({EventType::RESET_INPUT_STATE, 0, 0, 0});
    q.q.push_back({EventType::SOUND_INIT,        0, 0, 0});
    q.q.push_back({EventType::INPUT,             0x010, 0x020, 0});

    uint16_t p1, p2;
    CHECK(pop_with_apply(q, &p1, &p2));
    REQUIRE(trace.size() == 3);
    CHECK(trace[0] == EventType::PIN_RNG);
    CHECK(trace[1] == EventType::RESET_INPUT_STATE);
    CHECK(trace[2] == EventType::SOUND_INIT);
    CHECK(p1 == 0x010);
    CHECK(p2 == 0x020);
}

TEST_CASE("Head-drain: trailing non-INPUT events stay queued until next INPUT batch") {
    // After the last INPUT of a batch, if a non-INPUT op trails (e.g. a
    // FINGERPRINT diagnostic), it sits at the head of the queue waiting for
    // the next INPUT batch. The drain loop must not pop it until then.
    using namespace mirror_eq;
    EventQueue q;
    q.q.push_back({EventType::INPUT, 1, 2, 0});
    q.q.push_back({EventType::FINGERPRINT, 0, 0, 0xCAFEBABE});

    uint16_t p1 = 0, p2 = 0;
    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 1); CHECK(p2 == 2);
    CHECK(q.non_input_drained == 0);

    // Second pop: drain FINGERPRINT, then queue is empty → false.
    CHECK_FALSE(q.PopInput(&p1, &p2));
    CHECK(q.non_input_drained == 1);
    CHECK(q.q.empty());

    // New INPUT batch arrives — pop should succeed cleanly.
    q.q.push_back({EventType::INPUT, 3, 4, 0});
    CHECK(q.PopInput(&p1, &p2));
    CHECK(p1 == 3); CHECK(p2 == 4);
    CHECK(q.non_input_drained == 1);  // unchanged
}
