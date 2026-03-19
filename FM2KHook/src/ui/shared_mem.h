#pragma once
#include <cstdint>

// ============================================================================
// MINIMAL SHARED MEMORY - Hook -> Launcher status reporting
// Config delivery: environment variables (FM2K_PLAYER_INDEX, FM2K_LOCAL_PORT, etc.)
// Save states: local to hook DLL (savestate.cpp)
// This struct is ~64 bytes. Previously was ~9.6 MB.
// ============================================================================

constexpr uint32_t FM2K_SHARED_MEM_MAGIC = 0x464D324B;  // "FM2K"
constexpr uint32_t FM2K_SHARED_MEM_VERSION = 2;

struct FM2KSharedMemData {
    uint32_t magic;           // FM2K_SHARED_MEM_MAGIC - validates mapping
    uint32_t version;         // Struct version

    uint8_t  player_index;    // 0 = P1/Host, 1 = P2/Client
    uint8_t  netplay_state;   // 0=disconnected, 1=connected, 2=battle
    uint8_t  session_ready;   // GekkoNet session synced
    uint8_t  _pad0;

    uint32_t game_mode;       // 2000=CSS, 3000+=Battle
    uint32_t frame_number;    // Current netplay frame

    uint32_t rollback_count;  // Total rollbacks this session
    uint32_t desync_count;    // Total desyncs detected
    float    frames_ahead;    // Frame advantage (gekko_frames_ahead)
    uint32_t ping_ms;         // RTT placeholder

    uint32_t rng_seed;        // Current RNG seed (for monitoring)
    uint32_t render_fps;      // Render FPS

    uint8_t  _reserved[8];   // Future use
};

static_assert(sizeof(FM2KSharedMemData) <= 64, "Keep shared mem small");

// Shared memory lifecycle
bool InitializeSharedMemory();
void CleanupSharedMemory();

// Update shared memory with current stats (call from frame loop)
void SharedMem_Update();

// Get pointer (nullptr if not initialized)
FM2KSharedMemData* GetSharedMemory();
