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
