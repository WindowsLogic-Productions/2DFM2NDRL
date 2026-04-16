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

    // Render frame counter - MUST save/restore for rollback!
    // Without this, rollback replay increments it extra times causing divergence.
    // If any game logic reads this counter, it produces different results per client.
    constexpr uintptr_t ADDR_RENDER_FRAME_COUNTER = 0x4456FC;

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

// Effect/shake region addresses - saved for rollback determinism
namespace EffectAddrs {
    constexpr uintptr_t EFFECT_SYS1     = 0x447D7D;
    constexpr size_t    EFFECT_SYS1_SZ  = 42;
    constexpr uintptr_t EFFECT_SYS2     = 0x4456D0;
    constexpr size_t    EFFECT_SYS2_SZ  = 44;
    constexpr uintptr_t SHAKE_EFFECTS   = 0x447DA9;
    constexpr size_t    SHAKE_EFFECTS_SZ = 40;
}

// Rollback buffer
static constexpr int MAX_ROLLBACK_FRAMES = 64;
static SaveStateData g_state_buffer[MAX_ROLLBACK_FRAMES];

// Initial sync flag - forces deterministic state on first save (like BBBR's m_initialSyncDone)
static bool g_initial_sync_done = false;

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
    g_initial_sync_done = false;  // Reset so next battle re-syncs
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

    // CRITICAL: On the very first save, force frame-dependent counters to
    // deterministic values. CSS runs different frame counts on each client,
    // so buf_idx, render_fc, etc. are diverged by the time battle starts.
    // Both clients independently set the same values here (no UDP sync needed).
    //
    // NOTE: We do NOT clear object_pool or game_state here - the game needs
    // that data for battle initialization. Those regions have CSS residue that
    // differs between clients but gets overwritten by the first game tick.
    // The checksum excludes these CSS-residue regions (see CalculateFullChecksum).
    if (!g_initial_sync_done) {
        *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = 0;         // Reset buf_idx
        *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = 0;       // Reset render frame counter
        // NOTE: Do NOT zero g_score_value (0x470050) - it's used by the round
        // system for battle transition. It's excluded from the CRC instead.

        // Clear input-related state ONLY - do NOT clear screen dimensions!
        // 0x447EE0-0x447F80 contains wDest, hDest, g_screen_x, g_screen_y
        // which render_frame and camera_manager need. Zeroing them kills rendering.
        memset((void*)0x447F00, 0, 0x20);   // g_prev_input_state (0x447F00, 32 bytes)
        memset((void*)0x447F40, 0, 0x20);   // g_processed_input  (0x447F40, 32 bytes)
        memset((void*)0x447F60, 0, 0x20);   // g_input_changes    (0x447F60, 32 bytes)

        // Clear input history - stale CSS inputs cause edge detection mismatches
        memset((void*)ADDR_INPUT_HISTORY, 0, SIZE_INPUT_HISTORY);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState: Initial sync - forced buf_idx=0 render_fc=0 score=0 input_tracking=cleared");
        g_initial_sync_done = true;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    state->frame_number = frame;

    // Save RNG seed
    state->rng_seed = *(uint32_t*)ADDR_RNG_SEED;

    // Save render frame counter - prevents divergence during rollback replay
    state->render_frame_counter = *(uint32_t*)ADDR_RENDER_FRAME_COUNTER;

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

    // Save effect/shake state - affects animation script execution during rollback
    memcpy(state->effect_sys1, (void*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    memcpy(state->effect_sys2, (void*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    memcpy(state->shake_effects, (void*)EffectAddrs::SHAKE_EFFECTS, EffectAddrs::SHAKE_EFFECTS_SZ);

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

    // Restore render frame counter - prevents divergence during rollback replay
    *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = state->render_frame_counter;

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

    // Restore effect/shake state
    memcpy((void*)EffectAddrs::EFFECT_SYS1, state->effect_sys1, EffectAddrs::EFFECT_SYS1_SZ);
    memcpy((void*)EffectAddrs::EFFECT_SYS2, state->effect_sys2, EffectAddrs::EFFECT_SYS2_SZ);
    memcpy((void*)EffectAddrs::SHAKE_EFFECTS, state->shake_effects, EffectAddrs::SHAKE_EFFECTS_SZ);

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



uint32_t SaveState_CalculateFullChecksum() {
    // Compute per-region checksums for diagnostic comparison
    g_region_checksums.rng = *(uint32_t*)ADDR_RNG_SEED;

    // Game state CRC: split to EXCLUDE known per-process/CSS-residue fields:
    //   0x470050 (4 bytes): g_score_value - CSS frame counter residue, used for battle transition
    //   0x4701CC (4 bytes): g_hInstance - different per process
    // Region 1: 0x470020 to 0x470050 (0x30 bytes)
    // Region 2: 0x470054 to 0x4701CC (0x178 bytes)
    uint32_t gs1 = Fletcher32((uint8_t*)ADDR_GAME_STATE, 0x470050 - ADDR_GAME_STATE);
    uint32_t gs2 = Fletcher32((uint8_t*)0x470054, 0x4701CC - 0x470054);
    g_region_checksums.game_state = gs1 ^ gs2;

    // Object pool: first 4KB for speed (enough to detect divergence)
    g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, 4096);

    // Character dynamic state
    uint32_t char_ck = 0;
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t dynamic_addr = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE) + CHAR_SLOT_DYNAMIC_OFFSET;
        char_ck ^= Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
    }
    g_region_checksums.char_dynamic = char_ck;

    // Input tracking CRC: only hash the actual input state, skip screen dimensions
    // (wDest/hDest/g_screen_x/g_screen_y at 0x447F20-0x447F3F may differ between instances)
    uint32_t it1 = Fletcher32((uint8_t*)0x447EE0, 0x447F00 - 0x447EE0);   // buf_idx region
    uint32_t it2 = Fletcher32((uint8_t*)0x447F00, 0x447F20 - 0x447F00);   // g_prev_input_state
    uint32_t it3 = Fletcher32((uint8_t*)0x447F40, 0x447F80 - 0x447F40);   // g_processed_input + g_input_changes
    g_region_checksums.input_tracking = it1 ^ it2 ^ it3;

    // Effect/shake regions (now saved for rollback)
    g_region_checksums.effect_sys1 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    g_region_checksums.effect_sys2 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    g_region_checksums.shake_effects = Fletcher32((uint8_t*)EffectAddrs::SHAKE_EFFECTS, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Combined CRC for GekkoNet desync detection.
    // INCLUDES: RNG, game_state (excl hInstance), input_tracking
    // EXCLUDES from CRC (still saved/restored for rollback):
    //   - object_pool: CSS residue in positions, converges in 1-2 frames
    //   - char_dynamic: CSS residue in character state, converges in 1-2 frames
    //   - render_fc: render-only counter, not gameplay
    //   - g_hInstance: per-process handle (excluded from game_state CRC above)
    // Per-region CRCs are still computed above for diagnostic logging.
    {
        uint32_t combined = 0x811C9DC5;  // FNV offset basis
        combined ^= g_region_checksums.rng;
        combined *= 0x01000193;
        combined ^= g_region_checksums.game_state;
        combined *= 0x01000193;
        combined ^= g_region_checksums.input_tracking;
        combined *= 0x01000193;
        g_region_checksums.combined = combined;
    }

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

// =============================================================================
// BBBR-STYLE DESYNC DIAGNOSTIC DUMP
// Writes per-region CRC + hex dump to file for easy comparison between P1/P2
// =============================================================================

// Named memory regions for diagnostic comparison (like BBBR's memory_regions.hpp)
struct DiagRegion {
    uintptr_t addr;
    size_t size;
    const char* name;
    bool exclude_from_crc;  // If true, saved/restored but not in desync CRC
};

static const DiagRegion g_diag_regions[] = {
    { 0x41FB1C,     4, "RNG_Seed",                    false },
    { 0x4456FC,     4, "RenderFrameCounter",           true  },  // Render-only, diverges
    { 0x447EE0,  0xA0, "InputTrackingState",           false },
    { 0x4280D8, 0x2008,"InputHistory",                 false },
    { 0x470020,  0x24, "PlayerStagePositions",         false },
    { 0x470044,  0x10, "GameTimerAndRound",            false },  // g_game_timer, g_round_limit, g_round_state, g_score_value
    { 0x470054,  0x14, "GameModeAndRoundTimer",        false },  // g_game_mode through g_round_setting
    { 0x47006C,  0x80, "PlayerMoveHistory",            false },
    { 0x4700EC,  0x20, "RoundCounts",                  false },  // g_p1_round_count, g_p1_round_state
    { 0x47010C,  0x10, "DemoModeState",                false },
    { 0x47011C,  0x80, "PlayerActionHistory",          false },
    { 0x47019C,   0x4, "P1ActionState",                false },
    { 0x4701A0,   0x4, "P2ActionState",                false },
    { 0x4701BC,   0x4, "GamePaused",                   false },
    { 0x4701C0,   0x4, "ReplayMode",                   false },
    { 0x4701C4,   0x8, "HitEffectState",               false },
    { 0x4701CC,   0x4, "hInstance",                     true  },  // ALWAYS different per process
    { 0x4701E0, 0x100, "ObjectPool_First256",          false },  // First 256 bytes for quick check
    { 0x4702E0, 0x100, "ObjectPool_Second256",         false },
    { 0x447D7D,  0x2A, "EffectSystem1",                true  },  // Unsaved
    { 0x4456D0,  0x2C, "EffectSystem2",                true  },  // Unsaved
    { 0x447DA9,  0x28, "ShakeEffects",                 true  },  // Unsaved
};
static constexpr int NUM_DIAG_REGIONS = sizeof(g_diag_regions) / sizeof(g_diag_regions[0]);

void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc, uint32_t remote_crc, int player_index) {
    char filename[256];
    snprintf(filename, sizeof(filename), "FM2K_P%d_desync_f%d.log", player_index + 1, frame);

    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open desync diagnostic file: %s", filename);
        return;
    }

    fprintf(f, "=== FM2K DESYNC DIAGNOSTIC ===\n");
    fprintf(f, "Player: P%d\n", player_index + 1);
    fprintf(f, "Frame: %d\n", frame);
    fprintf(f, "Local CRC: 0x%08X\n", local_crc);
    fprintf(f, "Remote CRC: 0x%08X\n\n", remote_crc);

    fprintf(f, "=== Per-Region CRC (live state at detection time) ===\n");
    for (int i = 0; i < NUM_DIAG_REGIONS; i++) {
        const auto& r = g_diag_regions[i];
        uint32_t crc = Fletcher32((uint8_t*)r.addr, r.size);
        fprintf(f, "  %-28s (0x%06X +%4zu): crc=0x%08X %s\n",
                r.name, (unsigned)r.addr, r.size, crc,
                r.exclude_from_crc ? "[EXCLUDED]" : "");

        // Hex dump regions up to 256 bytes
        if (r.size <= 256) {
            const uint8_t* p = (const uint8_t*)r.addr;
            for (size_t row = 0; row < r.size; row += 16) {
                fprintf(f, "    +%03X:", (unsigned)row);
                for (size_t col = 0; col < 16 && row + col < r.size; col++)
                    fprintf(f, " %02X", p[row + col]);
                fprintf(f, "\n");
            }
        }
    }

    // Character dynamic state per-slot CRC
    fprintf(f, "\n=== Character Dynamic Slots ===\n");
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t dynamic_addr = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE) + CHAR_SLOT_DYNAMIC_OFFSET;
        uint32_t crc = Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
        fprintf(f, "  Slot[%zu] (0x%08X +%u): crc=0x%08X\n",
                i, (unsigned)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE, crc);
    }

    // Object pool summary (first 16 objects, 382 bytes each based on game structure)
    fprintf(f, "\n=== Object Pool Summary (first 16 objects) ===\n");
    constexpr size_t OBJ_SIZE = 382;
    for (int i = 0; i < 16; i++) {
        uintptr_t obj_addr = ADDR_OBJECT_POOL + (i * OBJ_SIZE);
        uint8_t first_byte = *(uint8_t*)obj_addr;
        uint32_t crc = Fletcher32((uint8_t*)obj_addr, OBJ_SIZE);
        fprintf(f, "  Obj[%2d] (0x%08X): active=%d crc=0x%08X\n",
                i, (unsigned)obj_addr, first_byte, crc);
    }

    fprintf(f, "\n=== How to Compare ===\n");
    fprintf(f, "Both players produce this file. Diff the per-region CRCs to find\n");
    fprintf(f, "the FIRST region with different CRC -- that's the desync source.\n");
    fprintf(f, "Small regions include hex dumps for direct byte comparison.\n");

    fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Desync diagnostic written to %s", filename);
}
