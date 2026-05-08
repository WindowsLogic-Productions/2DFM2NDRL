// Replay format wire-layout tests.
//
// Pins the on-disk / on-wire layout of ReplayHeader so a future edit
// (adding a field, reordering, padding change) can't silently break the
// MATCH_START SessionEvent's 96-byte payload — spectator-side decoders
// pull fields by hard-coded offset (seed @ 20, state_hash @ 24, char
// selects @ 28-31). v0.2.27 retired the legacy .fm2krep file format
// owned by replay.cpp; only the header struct survives as the
// MATCH_START wire-payload schema.

#include "doctest.h"

#include <cstdint>
#include <cstring>

// Pull in the production header. It only depends on <cstdint> + <cstddef> so
// it's safe to include in a Linux native test build.
#include "../FM2KHook/src/netplay/replay.h"

using namespace Replay;

TEST_CASE("ReplayHeader is exactly 96 bytes") {
    // The host's spectator-node code sends the header as the INITIAL_MATCH
    // datagram payload; it pulls fields by hard-coded offset (seed @ 20,
    // state_hash @ 24, char selects @ 28-31). Any size change without
    // matching offset rewrite breaks spectators.
    CHECK(sizeof(ReplayHeader) == 96);
}

TEST_CASE("ReplayHeader field offsets match the spectator-node parser's expectations") {
    // spectator_node.cpp::HandleSpecData and SpectatorNode_OnMatchStart both
    // pull fields at literal offsets. Pin them here.
    CHECK(offsetof(ReplayHeader, magic)              == 0);
    CHECK(offsetof(ReplayHeader, version)            == 4);
    CHECK(offsetof(ReplayHeader, game_hash)          == 16);
    CHECK(offsetof(ReplayHeader, initial_rng_seed)   == 20);
    CHECK(offsetof(ReplayHeader, initial_state_hash) == 24);
    CHECK(offsetof(ReplayHeader, p1_char_slot)       == 28);
    CHECK(offsetof(ReplayHeader, p1_color)           == 29);
    CHECK(offsetof(ReplayHeader, p2_char_slot)       == 30);
    CHECK(offsetof(ReplayHeader, p2_color)           == 31);
    CHECK(offsetof(ReplayHeader, frame_count)        == 92);
}

TEST_CASE("ReplayHeader magic is the FMPR little-endian sentinel") {
    CHECK(REPLAY_MAGIC == 0x52504D46u);  // "FMPR" ASCII LE
    CHECK(REPLAY_VERSION == 1);
}

TEST_CASE("Header roundtrips through a 96-byte byte buffer unchanged") {
    ReplayHeader src = {};
    src.magic              = REPLAY_MAGIC;
    src.version            = REPLAY_VERSION;
    src.game_hash          = 0xDEADBEEF;
    src.initial_rng_seed   = 0x12345678;
    src.initial_state_hash = 0xCAFEF00D;
    src.p1_char_slot       = 7;
    src.p1_color           = 2;
    src.p2_char_slot       = 4;
    src.p2_color           = 1;
    src.frame_count        = 1234;
    std::strncpy(reinterpret_cast<char*>(src.p1_name), "Player1", sizeof(src.p1_name));
    std::strncpy(reinterpret_cast<char*>(src.p2_name), "Player2", sizeof(src.p2_name));

    uint8_t buf[96] = {};
    std::memcpy(buf, &src, sizeof(buf));

    ReplayHeader rt = {};
    std::memcpy(&rt, buf, sizeof(buf));

    CHECK(rt.magic              == src.magic);
    CHECK(rt.version            == src.version);
    CHECK(rt.game_hash          == src.game_hash);
    CHECK(rt.initial_rng_seed   == src.initial_rng_seed);
    CHECK(rt.initial_state_hash == src.initial_state_hash);
    CHECK(rt.p1_char_slot       == src.p1_char_slot);
    CHECK(rt.p2_color           == src.p2_color);
    CHECK(rt.frame_count        == src.frame_count);
    CHECK(std::strcmp(reinterpret_cast<const char*>(rt.p1_name), "Player1") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(rt.p2_name), "Player2") == 0);
}

