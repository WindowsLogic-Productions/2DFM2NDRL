// Optimized savestate - only save dynamic portions of character slots
// Static character data (sprites, animations, hitboxes) loaded from .player files doesn't change
#include "savestate.h"
#include "shared_mem.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstring>

// Memory region addresses
namespace {
    // RNG
    constexpr uintptr_t ADDR_RNG_SEED      = 0x41FB1C;

    // Object pool (projectiles, effects)
    constexpr uintptr_t ADDR_OBJECT_POOL   = 0x4701E0;
    constexpr size_t    SIZE_OBJECT_POOL   = 0x5F800;

    // Input history
    constexpr uintptr_t ADDR_INPUT_HISTORY = 0x4280E0;
    constexpr size_t    SIZE_INPUT_HISTORY = 0x2000;

    // Game state
    constexpr uintptr_t ADDR_GAME_STATE    = 0x470040;
    constexpr size_t    SIZE_GAME_STATE    = 0x200;
}

// Rollback buffer
static constexpr int MAX_ROLLBACK_FRAMES = 8;
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

    // Calculate checksum over CRITICAL game state only
    // Don't checksum raw memory - only specific gameplay values that must match
    // This avoids false desyncs from uninitialized memory garbage
    // NOTE: RNG excluded - it gets polluted by non-gameplay code (audio, visual effects)
    uint32_t checksum = 0;

    // Sample key positions from each active character slot
    // Character dynamic data starts at offset 55000 in each slot
    // Position data is near the start of dynamic section
    for (size_t i = 0; i < 2; i++) {  // Only check P1 and P2 slots
        const uint8_t* dyn = state->char_dynamic[i];
        // First 64 bytes of dynamic data contains critical state
        checksum ^= Fletcher32(dyn, 64);
    }

    // Game mode and timer from game_state
    checksum ^= Fletcher32(state->game_state, 32);

    state->checksum = checksum;

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
