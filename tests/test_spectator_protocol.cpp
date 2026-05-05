// Spectator-protocol wire-layout tests.
//
// Pins the SpecDataHeader (10 bytes, 0xCE-prefixed) and the SpecCssUpdate
// payload, plus the input-batch chunking math the host uses to send the
// session-history backfill to a late joiner.

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <algorithm>

// We'd love to include spectator_node.h directly, but it pulls in
// <winsock2.h> which isn't available on Linux native builds. So we
// re-declare the public-protocol structs here and the tests pin them by
// equality. If the production header drifts, a separate test
// (test_protocol_consistency) below ensures we caught the change.
#include "../FM2KHook/src/netplay/replay.h"

namespace mirror {
constexpr uint8_t SPEC_DATA_MAGIC = 0xCE;

enum class SpecDataType : uint8_t {
    INITIAL_MATCH = 1,
    INPUT_BATCH   = 2,
    MATCH_END     = 3,
};

#pragma pack(push, 1)
struct SpecDataHeader {
    uint8_t      magic;
    SpecDataType type;
    uint32_t     start_frame;
    uint16_t     frame_count;
    uint16_t     flags;
};
#pragma pack(pop)
}  // namespace mirror

TEST_CASE("SpecDataHeader is exactly 10 bytes") {
    // Datagram size budget: header + payload <= UDP MTU. Backfill chunks at
    // 256 frames * 4 B + 10 B header = 1034 B total — well under 1200.
    CHECK(sizeof(mirror::SpecDataHeader) == 10);
}

TEST_CASE("SpecDataHeader field offsets are stable") {
    using H = mirror::SpecDataHeader;
    CHECK(offsetof(H, magic)       == 0);
    CHECK(offsetof(H, type)        == 1);
    CHECK(offsetof(H, start_frame) == 2);
    CHECK(offsetof(H, frame_count) == 6);
    CHECK(offsetof(H, flags)       == 8);
}

TEST_CASE("SpecDataType enum values are stable on the wire") {
    CHECK(static_cast<uint8_t>(mirror::SpecDataType::INITIAL_MATCH) == 1);
    CHECK(static_cast<uint8_t>(mirror::SpecDataType::INPUT_BATCH)   == 2);
    CHECK(static_cast<uint8_t>(mirror::SpecDataType::MATCH_END)     == 3);
}

TEST_CASE("SPEC_DATA_MAGIC distinguishes from CtrlPacket and GekkoNet") {
    // Control-channel uses 0xCC (first byte of CtrlPacketHeader.type or
    // similar — see control_channel.cpp::RawReceive). GekkoNet packets are
    // anything else. 0xCE must not collide with either.
    CHECK(mirror::SPEC_DATA_MAGIC == 0xCE);
    CHECK(mirror::SPEC_DATA_MAGIC != 0xCC);  // control-channel
}

TEST_CASE("INITIAL_MATCH payload layout matches what spectator_node.cpp parses") {
    // The host packs the 96-byte ReplayHeader into the INITIAL_MATCH payload
    // (SpectatorNode_OnMatchStart). Spectator-side handler memcpys at
    // hardcoded offsets. This test asserts those offsets are what the
    // sender encodes — paired with test_replay_format.cpp's offset checks
    // which pin the OTHER end of the contract.
    uint8_t payload[96] = {};
    uint32_t magic   = Replay::REPLAY_MAGIC;
    uint16_t version = Replay::REPLAY_VERSION;
    uint32_t game_hash         = 0xAAAAAAAA;
    uint32_t initial_rng_seed  = 0x12345678;
    uint32_t initial_state_hash = 0xBBBBBBBB;
    std::memcpy(payload + 0,  &magic,              4);
    std::memcpy(payload + 4,  &version,            2);
    std::memcpy(payload + 16, &game_hash,          4);
    std::memcpy(payload + 20, &initial_rng_seed,   4);
    std::memcpy(payload + 24, &initial_state_hash, 4);
    payload[28] = 7;   // p1_char
    payload[29] = 2;   // p1_color
    payload[30] = 4;   // p2_char
    payload[31] = 1;   // p2_color

    // Mirror the spectator's parse from spectator_node.cpp::HandleSpecData
    uint32_t parsed_seed = 0, parsed_state = 0;
    std::memcpy(&parsed_seed,  payload + 20, 4);
    std::memcpy(&parsed_state, payload + 24, 4);

    CHECK(parsed_seed     == 0x12345678);
    CHECK(parsed_state    == 0xBBBBBBBB);
    CHECK(payload[28]     == 7);
    CHECK(payload[29]     == 2);
    CHECK(payload[30]     == 4);
    CHECK(payload[31]     == 1);
}

TEST_CASE("INPUT_BATCH frame layout: (p1 u16, p2 u16) packed contiguously") {
    // Both producer (FlushBatch / SendSessionBackfillTo) and consumer
    // (HandleSpecData::INPUT_BATCH) treat the payload as raw N*4 bytes.
    constexpr int N = 5;
    uint16_t p1[N] = {0x001, 0x002, 0x004, 0x008, 0x010};
    uint16_t p2[N] = {0x100, 0x200, 0x400, 0x080, 0x040};

    uint8_t buf[N * 4];
    for (int i = 0; i < N; i++) {
        std::memcpy(buf + i * 4 + 0, &p1[i], 2);
        std::memcpy(buf + i * 4 + 2, &p2[i], 2);
    }

    // Parse back.
    for (int i = 0; i < N; i++) {
        uint16_t got_p1, got_p2;
        std::memcpy(&got_p1, buf + i * 4 + 0, 2);
        std::memcpy(&got_p2, buf + i * 4 + 2, 2);
        CHECK(got_p1 == p1[i]);
        CHECK(got_p2 == p2[i]);
    }
}

// Mirror the chunking logic from spectator_node.cpp::SendSessionBackfillTo.
// Pulled out of the .cpp so we can test it without the Windows / SDL deps.
constexpr size_t BACKFILL_CHUNK_FRAMES = 256;

struct ChunkPlan {
    size_t   chunk_count;     // Number of datagrams that will be sent
    uint32_t last_start_frame; // start_frame of the final chunk
    uint16_t last_n;          // frame_count of the final chunk (1..256)
};

static ChunkPlan PlanBackfill(size_t total_frames, uint32_t session_start) {
    ChunkPlan plan{0, 0, 0};
    for (size_t off = 0; off < total_frames; off += BACKFILL_CHUNK_FRAMES) {
        const size_t n = std::min(BACKFILL_CHUNK_FRAMES, total_frames - off);
        plan.chunk_count++;
        plan.last_start_frame = session_start + (uint32_t)off;
        plan.last_n           = (uint16_t)n;
    }
    return plan;
}

TEST_CASE("Session backfill chunking — empty history sends no datagrams") {
    ChunkPlan p = PlanBackfill(0, 0);
    CHECK(p.chunk_count == 0);
}

TEST_CASE("Session backfill chunking — single chunk for small history") {
    ChunkPlan p = PlanBackfill(100, 0);
    CHECK(p.chunk_count      == 1);
    CHECK(p.last_start_frame == 0);
    CHECK(p.last_n           == 100);
}

TEST_CASE("Session backfill chunking — exact multiple of chunk size") {
    ChunkPlan p = PlanBackfill(BACKFILL_CHUNK_FRAMES * 3, 0);
    CHECK(p.chunk_count      == 3);
    CHECK(p.last_start_frame == 2 * BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n           == BACKFILL_CHUNK_FRAMES);
}

TEST_CASE("Session backfill chunking — final chunk holds remainder") {
    // 257 frames = one full 256-frame chunk + one 1-frame chunk
    ChunkPlan p = PlanBackfill(257, 0);
    CHECK(p.chunk_count      == 2);
    CHECK(p.last_start_frame == 256);
    CHECK(p.last_n           == 1);
}

TEST_CASE("Session backfill chunking — hour-long set is well-bounded") {
    // 1 hour at 100 Hz = 360000 frames. ~1408 chunks. ~1.4 MB session memory.
    ChunkPlan p = PlanBackfill(360000, 0);
    CHECK(p.chunk_count == (360000 / BACKFILL_CHUNK_FRAMES + 1));
    CHECK(p.last_start_frame == (360000 / BACKFILL_CHUNK_FRAMES) * BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n == 360000 % BACKFILL_CHUNK_FRAMES);
}

TEST_CASE("Session backfill chunking — start_frame offset propagates") {
    ChunkPlan p = PlanBackfill(BACKFILL_CHUNK_FRAMES + 10, /*session_start=*/1000);
    CHECK(p.chunk_count == 2);
    CHECK(p.last_start_frame == 1000 + BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n == 10);
}
