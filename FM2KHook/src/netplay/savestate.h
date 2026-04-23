#pragma once
#include <cstdint>
#include <cstddef>
#include "../audio/sound_rollback.h"  // DesiredState, MAX_CHANNELS

// ============================================================================
// CHARACTER SLOT CONSTANTS
// Wave C state_manager audit (docs/game/state_manager_audit.md) corrected two
// P0 bugs here:
//   - base was 0x4D1D80 → correct is 0x4D1D90 (off-by-16, we were reading the
//     tail of an adjacent slot and missing the last 16 B of every slot).
//   - dynamic offset was 55000 → correct is 0 (we only saved the last 2407 B
//     of a 57407 B slot; 95% of each slot was unsaved frame-mutable interpreter
//     state: task vars, the 20-slot box pointer array at +0x125, playerAliveFlag,
//     hit-junction tables, opponent ptr, throw state, etc.).
// ============================================================================
constexpr size_t CHAR_SLOT_SIZE = 57407;           // Full slot size from IDA
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 0;     // Save full slot (was 55000)
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE = CHAR_SLOT_SIZE - CHAR_SLOT_DYNAMIC_OFFSET; // 57407 bytes
constexpr size_t NUM_CHAR_SLOTS = 8;
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D90;    // g_character_data_base (corrected)

// ============================================================================
// SAVE STATE DATA - ~420KB per slot
// ============================================================================
struct SaveStateData {
    uint32_t frame_number;
    uint32_t checksum;
    uint32_t rng_seed;
    uint32_t input_buffer_index;
    uint32_t render_frame_counter;                         // 0x4456FC - diverges during rollback replay

    uint8_t input_tracking_state[0xA0];                    // 160 bytes
    uint8_t char_dynamic[NUM_CHAR_SLOTS][CHAR_SLOT_DYNAMIC_SIZE]; // ~460 KB (full 57 KB slot × 8)
    uint8_t object_pool[0x5F800];                          // ~391KB
    uint8_t input_history[0x2008];                         // ~8KB
    uint8_t game_state[0x220];                             // 544 bytes
    uint8_t effect_sys1[42];                               // Effect system P1
    uint8_t effect_sys2[88];                               // 0x4456B0..0x445708: sysvars + effect-sys2 + timer_countdown1
    uint8_t shake_effects[40];                             // Shake structures

    // g_round_end_flag — written/read by vs_round_function, drives round
    // transitions. Not previously saved; IDA audit flagged it as unsaved
    // state that rollback must cover for clean round-boundary replay.
    uint32_t round_end_flag;                               // 0x424718

    // Mike Z rollback-safe sound layer: per-channel "desired" state. Not
    // DSound hardware state — only the sim's authoritative record of what
    // should be playing on each channel. Sound-layer sync at end of each
    // displayed frame reconciles this to the real DSound buffers with the
    // rollback-window filter (see sound_rollback.h).
    SoundRollback::DesiredState sound_desired[SoundRollback::MAX_CHANNELS];

    // Wave C audit additions: these regions were unsaved and are the prime
    // suspects for post-rollback determinism drift.
    // Afterimage pool: dash trails / motion blur objects. ~163 KB.
    uint8_t afterimage_pool[0x46F6C0 - 0x447930];
    // Object linked-list topology. Payloads live in object_pool above, but the
    // list heads/tails/next-ptrs that define iteration order are elsewhere.
    uint32_t current_object_ptr;                           // 0x4259A8
    uint8_t  object_list_heads_tails[0x400];               // 0x430240..0x430640 (1024 B)
    uint8_t  object_node_pool[0x2000];                     // 0x4CFA20 (8192 B)

    // Per-region CRCs captured AT SAVE TIME (the forward-sim state at this
    // frame). On desync, we dump these alongside the CURRENT (post-replay)
    // per-region CRCs — a single log file shows which region diverged between
    // the two without having to diff two dump files across peers.
    struct SavedRegionCRCs {
        uint32_t rng;
        uint32_t game_state;
        uint32_t object_pool;
        uint32_t char_dynamic;
        uint32_t input_tracking;
        uint32_t effect_sys1;
        uint32_t effect_sys2;
        uint32_t shake_effects;
        uint32_t afterimage_pool;
        uint32_t list_heads_tails;
        uint32_t node_pool;
        uint32_t current_object_ptr_val;  // value, not CRC — 4 bytes only
        uint32_t gameplay_fingerprint;
        uint32_t combined;

        // Per-object-slot CRCs. Object pool is 1023 slots × 382 B at
        // 0x4701E0..0x4FF9E0. On a desync where ObjectPool(full) diverges,
        // diffing this array forward-vs-replay tells us the EXACT slot
        // index that mutated differently — then we hit IDA with that slot's
        // contents to find the code that wrote it.
        static constexpr size_t OBJ_SLOT_SIZE  = 382;
        static constexpr size_t OBJ_SLOT_COUNT = 1023;
        uint32_t object_slot_crcs[OBJ_SLOT_COUNT];

        bool     valid;
    } saved_region_crcs;
};

// Addresses for Wave C audit regions — keep in one place.
namespace WaveCAddrs {
    constexpr uintptr_t AFTERIMAGE_POOL        = 0x447930;
    constexpr size_t    AFTERIMAGE_POOL_SZ     = 0x46F6C0 - 0x447930;
    constexpr uintptr_t CURRENT_OBJECT_PTR     = 0x4259A8;
    constexpr uintptr_t OBJECT_LIST_HEADS      = 0x430240;
    constexpr size_t    OBJECT_LIST_HEADS_SZ   = 0x400;
    constexpr uintptr_t OBJECT_NODE_POOL       = 0x4CFA20;
    constexpr size_t    OBJECT_NODE_POOL_SZ    = 0x2000;
}

// ============================================================================
// SAVESTATE API
// ============================================================================
void SaveState_Init();
bool SaveState_Save(int frame);
bool SaveState_Load(int frame);
uint32_t SaveState_GetLastChecksum(int frame);

// Per-region diagnostic checksums for desync investigation
struct RegionChecksums {
    uint32_t rng;
    uint32_t game_state;
    uint32_t object_pool;
    uint32_t char_dynamic;
    uint32_t input_tracking;
    uint32_t effect_sys1;
    uint32_t effect_sys2;
    uint32_t shake_effects;
    uint32_t combined;

    // Gameplay fingerprint: hash over just the game-visible fields (HP, positions,
    // RNG, round timer, current inputs). Process-independent by design — no
    // pointers, no heap addresses, no interpreter-internal state. If the full
    // combined hash differs but this matches, we have a false-positive desync:
    // players would see identical gameplay, only the internal memory layout
    // diverges due to per-process residue.
    uint32_t gameplay_fingerprint;
};

const RegionChecksums& SaveState_GetRegionChecksums();
uint32_t SaveState_CalculateFullChecksum();

// ============================================================================
// TESTING / VERIFICATION
// ============================================================================
struct StateSnapshot {
    uint32_t rng_seed;
    uint32_t input_buffer_index;
    uint32_t p1_x, p1_y;
    uint32_t p2_x, p2_y;
    uint32_t p1_hp, p2_hp;
    uint32_t frame_counter;
    uint32_t checksum;
};

StateSnapshot SaveState_CaptureSnapshot();
bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b, char* diff_buf, size_t buf_size);
bool SaveState_TestRoundtrip();

// BBBR-style desync diagnostic - dumps per-region CRC + hex to log file
void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc, uint32_t remote_crc, int player_index);

// Per-save rngtrace. Fixed-size in-memory ring, written to CSV on demand.
// Per-frame console logging was too noisy and tanked framerate at 100 fps.
void SaveState_PushRngTrace(int frame, uint32_t rng, uint32_t fp);
void SaveState_FlushRngTrace(int player_index, const char* reason);
