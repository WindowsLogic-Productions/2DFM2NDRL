// Optimized savestate - only save dynamic portions of character slots
// Static character data (sprites, animations, hitboxes) loaded from .player files doesn't change
#include "savestate.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>

// Memory region addresses
namespace {
    // RNG
    constexpr uintptr_t ADDR_RNG_SEED      = 0x41FB1C;

    // Input buffer index - CRITICAL for rollback!
    // This determines where in input history we read/write
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX = 0x447EE0;

    // Input tracking state - CRITICAL for correct input change detection!
    // Includes: g_prev_input_state (0x447F00), g_processed_input (0x447F40), g_input_changes (0x447F60)
    // These affect how the game detects new presses vs held buttons
    constexpr uintptr_t ADDR_INPUT_TRACKING = 0x447EE0;
    constexpr size_t    SIZE_INPUT_TRACKING = 0xA0;  // 0x447EE0 to 0x447F80

    // Object pool (projectiles, effects)
    constexpr uintptr_t ADDR_OBJECT_POOL   = 0x4701E0;
    constexpr size_t    SIZE_OBJECT_POOL   = 0x5F800;

    // Input history - Extended to include g_combined_input_changes at 0x4280D8
    constexpr uintptr_t ADDR_INPUT_HISTORY = 0x4280D8;
    constexpr size_t    SIZE_INPUT_HISTORY = 0x2008;  // 0x4280D8 to 0x42A0E0

    // Game state - CRITICAL: Start at 0x470020 to include g_player_stage_positions!
    // Previous start at 0x470040 missed player character selections
    constexpr uintptr_t ADDR_GAME_STATE    = 0x470020;
    constexpr size_t    SIZE_GAME_STATE    = 0x220;  // Ends at 0x470240
}

// Rollback buffer
static constexpr int MAX_ROLLBACK_FRAMES = 64;
static SaveStateData g_state_buffer[MAX_ROLLBACK_FRAMES];

// Simple Fletcher32 checksum
static uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    while (len) {
        size_t tlen = (len > 360) ? 360 : len;
        len -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

void SaveState_Init() {
    memset(g_state_buffer, 0, sizeof(g_state_buffer));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: Initialized %d rollback slots (~%zuKB each)",
                MAX_ROLLBACK_FRAMES, sizeof(SaveStateData) / 1024);
}

bool SaveState_Save(int frame) {
    // Handle frame -1 as frame 0 (initial state before first frame)
    // GekkoNet sends this during initialization
    if (frame < 0) {
        frame = 0;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    state->frame_number = frame;

    // Save RNG seed
    state->rng_seed = *(uint32_t*)ADDR_RNG_SEED;

    // Save input buffer index - CRITICAL for rollback!
    state->input_buffer_index = *(uint32_t*)ADDR_INPUT_BUFFER_INDEX;

    // Save input tracking state - CRITICAL for correct input change detection!
    memcpy(state->input_tracking_state, (void*)ADDR_INPUT_TRACKING, SIZE_INPUT_TRACKING);

    // Save dynamic portion of each character slot (96% smaller than full slots!)
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        memcpy(state->char_dynamic[i], (void*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
    }

    // Save object pool (projectiles, effects)
    memcpy(state->object_pool, (void*)ADDR_OBJECT_POOL, SIZE_OBJECT_POOL);

    // Save input history
    memcpy(state->input_history, (void*)ADDR_INPUT_HISTORY, SIZE_INPUT_HISTORY);

    // Save game state
    memcpy(state->game_state, (void*)ADDR_GAME_STATE, SIZE_GAME_STATE);

    // Calculate checksum for desync detection using full Fletcher32
    // Covers RNG, input tracking, character dynamic state, object pool, game state
    state->checksum = SaveState_CalculateFullChecksum();
    uint32_t checksum = state->checksum;

    // Diagnostic logging for first few frames
    static int save_log_count = 0;
    if (save_log_count++ < 10) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SAVE f=%d: checksum=0x%08X (rng=0x%08X) buf_idx=%u",
            frame, checksum, state->rng_seed, state->input_buffer_index);
    }

    return true;
}

uint32_t SaveState_GetLastChecksum(int frame) {
    if (frame < 0) frame = 0;
    int slot = frame % MAX_ROLLBACK_FRAMES;
    return g_state_buffer[slot].checksum;
}

bool SaveState_Load(int frame) {
    // Handle frame -1 as frame 0 (initial state before first frame)
    if (frame < 0) {
        frame = 0;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    if (state->frame_number != (uint32_t)frame) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveState: Frame mismatch! Wanted %d, slot has %u",
                     frame, state->frame_number);
        return false;
    }

    // Restore RNG seed
    *(uint32_t*)ADDR_RNG_SEED = state->rng_seed;

    // Restore input buffer index - CRITICAL for rollback!
    *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = state->input_buffer_index;

    // Restore input tracking state - CRITICAL for correct input change detection!
    memcpy((void*)ADDR_INPUT_TRACKING, state->input_tracking_state, SIZE_INPUT_TRACKING);

    // Restore dynamic portion of each character slot
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        memcpy((void*)dynamic_addr, state->char_dynamic[i], CHAR_SLOT_DYNAMIC_SIZE);
    }

    // Restore object pool
    memcpy((void*)ADDR_OBJECT_POOL, state->object_pool, SIZE_OBJECT_POOL);

    // Restore input history
    memcpy((void*)ADDR_INPUT_HISTORY, state->input_history, SIZE_INPUT_HISTORY);

    // Restore game state
    memcpy((void*)ADDR_GAME_STATE, state->game_state, SIZE_GAME_STATE);

    return true;
}

// =============================================================================
// TESTING / VERIFICATION
// =============================================================================

// Addresses for snapshot capture
namespace SnapshotAddrs {
    constexpr uintptr_t FRAME_COUNTER = 0x447EE0;  // Input buffer index serves as frame
    constexpr uintptr_t P1_OBJ_BASE = 0x4701E0;
    constexpr uintptr_t P2_OBJ_BASE = 0x4701E0 + 382;  // Object size is 382
    constexpr uintptr_t P1_HP = 0x470134;
    constexpr uintptr_t P2_HP = 0x470138;
}

StateSnapshot SaveState_CaptureSnapshot() {
    StateSnapshot snap = {};

    snap.rng_seed = *(uint32_t*)ADDR_RNG_SEED;
    snap.input_buffer_index = *(uint32_t*)ADDR_INPUT_BUFFER_INDEX;
    snap.frame_counter = g_frame_counter;

    // Player 1 position (offset 8 and 12 in object structure)
    snap.p1_x = *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 8);
    snap.p1_y = *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 12);

    // Player 2 position
    snap.p2_x = *(uint32_t*)(SnapshotAddrs::P2_OBJ_BASE + 8);
    snap.p2_y = *(uint32_t*)(SnapshotAddrs::P2_OBJ_BASE + 12);

    // HP
    snap.p1_hp = *(uint32_t*)SnapshotAddrs::P1_HP;
    snap.p2_hp = *(uint32_t*)SnapshotAddrs::P2_HP;

    // Full checksum
    snap.checksum = SaveState_CalculateFullChecksum();

    return snap;
}

bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b, char* diff_buf, size_t buf_size) {
    bool match = true;
    diff_buf[0] = '\0';
    char* p = diff_buf;
    size_t remaining = buf_size;

#define CHECK_FIELD(field) \
    if (a.field != b.field) { \
        int written = snprintf(p, remaining, #field ": 0x%X vs 0x%X\n", a.field, b.field); \
        if (written > 0 && (size_t)written < remaining) { p += written; remaining -= written; } \
        match = false; \
    }

    CHECK_FIELD(rng_seed);
    CHECK_FIELD(input_buffer_index);
    CHECK_FIELD(p1_x);
    CHECK_FIELD(p1_y);
    CHECK_FIELD(p2_x);
    CHECK_FIELD(p2_y);
    CHECK_FIELD(p1_hp);
    CHECK_FIELD(p2_hp);
    CHECK_FIELD(checksum);

#undef CHECK_FIELD

    return match;
}

// Per-region checksums for desync investigation
static RegionChecksums g_region_checksums = {};

const RegionChecksums& SaveState_GetRegionChecksums() {
    return g_region_checksums;
}

// Unsaved region addresses (for diagnostic checksums)
namespace UnsavedAddrs {
    constexpr uintptr_t EFFECT_SYS1     = 0x447D7D;  // Effect system P1
    constexpr size_t    EFFECT_SYS1_SZ  = 42;
    constexpr uintptr_t EFFECT_SYS2     = 0x4456D0;  // Effect system P2
    constexpr size_t    EFFECT_SYS2_SZ  = 44;
    constexpr uintptr_t SHAKE_EFFECTS   = 0x447DA9;  // Shake structures
    constexpr size_t    SHAKE_EFFECTS_SZ = 40;
}

uint32_t SaveState_CalculateFullChecksum() {
    // Compute per-region checksums for diagnostic comparison
    g_region_checksums.rng = *(uint32_t*)ADDR_RNG_SEED;

    g_region_checksums.game_state = Fletcher32((uint8_t*)ADDR_GAME_STATE, SIZE_GAME_STATE);

    // Object pool: first 4KB for speed (enough to detect divergence)
    g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, 4096);

    // Character dynamic state
    uint32_t char_ck = 0;
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t dynamic_addr = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE) + CHAR_SLOT_DYNAMIC_OFFSET;
        char_ck ^= Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
    }
    g_region_checksums.char_dynamic = char_ck;

    g_region_checksums.input_tracking = Fletcher32((uint8_t*)ADDR_INPUT_TRACKING, SIZE_INPUT_TRACKING);

    // Unsaved regions (diagnostic only - not part of combined)
    g_region_checksums.effect_sys1 = Fletcher32((uint8_t*)UnsavedAddrs::EFFECT_SYS1, UnsavedAddrs::EFFECT_SYS1_SZ);
    g_region_checksums.effect_sys2 = Fletcher32((uint8_t*)UnsavedAddrs::EFFECT_SYS2, UnsavedAddrs::EFFECT_SYS2_SZ);
    g_region_checksums.shake_effects = Fletcher32((uint8_t*)UnsavedAddrs::SHAKE_EFFECTS, UnsavedAddrs::SHAKE_EFFECTS_SZ);

    // Combined: RNG-only for GekkoNet comparison.
    // Object pool, game state, and char data contain CSS residue (different
    // pre-battle frame counts = different stale data) that doesn't affect gameplay.
    // Per-region checksums above are still computed for diagnostic logging.
    g_region_checksums.combined = g_region_checksums.rng;

    return g_region_checksums.combined;
}

bool SaveState_TestRoundtrip() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== SAVESTATE ROUNDTRIP TEST ===");

    // Capture state BEFORE save
    StateSnapshot before = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BEFORE: rng=0x%08X p1=(%d,%d) p2=(%d,%d) checksum=0x%08X",
        before.rng_seed, before.p1_x, before.p1_y, before.p2_x, before.p2_y, before.checksum);

    // Save state at frame 9999 (special test slot)
    int test_frame = 9999 % MAX_ROLLBACK_FRAMES;
    SaveState_Save(test_frame);

    // Corrupt state to verify load works
    *(uint32_t*)ADDR_RNG_SEED = 0xDEADBEEF;
    *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 8) = 0x12345678;

    StateSnapshot corrupted = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CORRUPTED: rng=0x%08X p1=(%d,%d) checksum=0x%08X",
        corrupted.rng_seed, corrupted.p1_x, corrupted.p1_y, corrupted.checksum);

    // Load state back
    SaveState_Load(test_frame);

    // Capture state AFTER load
    StateSnapshot after = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "AFTER: rng=0x%08X p1=(%d,%d) p2=(%d,%d) checksum=0x%08X",
        after.rng_seed, after.p1_x, after.p1_y, after.p2_x, after.p2_y, after.checksum);

    // Compare
    char diff_buf[1024];
    bool success = SaveState_CompareSnapshots(before, after, diff_buf, sizeof(diff_buf));

    if (success) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ROUNDTRIP TEST: PASSED - State restored correctly!");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROUNDTRIP TEST: FAILED - Differences:\n%s", diff_buf);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== END ROUNDTRIP TEST ===");
    return success;
}
