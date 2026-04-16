#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================================
// CHARACTER SLOT CONSTANTS - OPTIMIZED FOR ROLLBACK
// Static data (sprites, animations, hitboxes) loaded from .player files doesn't change
// Only save the dynamic portion for rollback (96% smaller!)
// ============================================================================
constexpr size_t CHAR_SLOT_SIZE = 57407;           // Full slot size from IDA
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 55000; // Where dynamic data starts
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE = CHAR_SLOT_SIZE - CHAR_SLOT_DYNAMIC_OFFSET; // 2407 bytes
constexpr size_t NUM_CHAR_SLOTS = 8;
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D80;    // g_character_data_base

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
    uint8_t char_dynamic[NUM_CHAR_SLOTS][CHAR_SLOT_DYNAMIC_SIZE]; // ~19KB
    uint8_t object_pool[0x5F800];                          // ~391KB
    uint8_t input_history[0x2008];                         // ~8KB
    uint8_t game_state[0x220];                             // 544 bytes
};

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
