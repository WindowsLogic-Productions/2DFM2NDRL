#pragma once
#include "shared_mem.h"
#include <cstdint>

// Minimal savestate API for GekkoNet rollback
void SaveState_Init();
bool SaveState_Save(int frame);
bool SaveState_Load(int frame);
uint32_t SaveState_GetLastChecksum(int frame);

// Per-region diagnostic checksums for desync investigation
struct RegionChecksums {
    uint32_t rng;            // RNG seed (4 bytes at 0x41FB1C)
    uint32_t game_state;     // Game state region (0x470020, 0x220)
    uint32_t object_pool;    // Object pool (0x4701E0, first 4KB for speed)
    uint32_t char_dynamic;   // All character dynamic regions combined
    uint32_t input_tracking; // Input tracking state (0x447EE0, 0xA0)
    // Unsaved regions - check if these diverge
    uint32_t effect_sys1;    // Effect system P1 (0x447D7D, 42 bytes)
    uint32_t effect_sys2;    // Effect system P2 (0x4456D0, 44 bytes)
    uint32_t shake_effects;  // Shake effect structures (0x447DA9, 40 bytes)
    uint32_t combined;       // XOR of saved regions (sent to GekkoNet)
};

// Get the last computed per-region checksums
const RegionChecksums& SaveState_GetRegionChecksums();

// =============================================================================
// TESTING / VERIFICATION
// =============================================================================

// Snapshot of critical game state for comparison
struct StateSnapshot {
    uint32_t rng_seed;
    uint32_t input_buffer_index;
    uint32_t p1_x, p1_y;
    uint32_t p2_x, p2_y;
    uint32_t p1_hp, p2_hp;
    uint32_t frame_counter;
    uint32_t checksum;  // Full state checksum
};

// Capture current game state into a snapshot
StateSnapshot SaveState_CaptureSnapshot();

// Compare two snapshots, returns true if identical
bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b, char* diff_buf, size_t buf_size);

// Run a save/load roundtrip test
// Saves at current frame, advances 1 frame, loads back, compares
// Returns true if state was restored correctly
bool SaveState_TestRoundtrip();

// Calculate full checksum of all saved state (for desync detection)
uint32_t SaveState_CalculateFullChecksum();
