// savestate_fm95.cpp -- FM95 save/load/checksum/snapshot/diagnostic. Split (engine axis) from savestate.cpp; whole body gated to FM95Hook. The FM95 savestate layout is WIP -- this is its home.
#if defined(ENGINE_FM95)
#include "savestate.h"
#include "savestate_internal.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

// ============================================================================
// FM95 save/load — addresses sourced from docs/FM95_Savestate_Inventory.md
// (IDA xrefs + decomp on the live CPW IDB).
// ============================================================================
namespace fm95save {
    // Block A
    constexpr uintptr_t OBJECT_POOL          = FM2K::ADDR_OBJECT_POOL;            // 0x426A40
    // Block B
    constexpr uintptr_t GAME_TICK_COUNTER    = FM2K::ADDR_FRAME_COUNTER;          // 0x4DD7A8
    constexpr uintptr_t RNG_SEED             = FM2K::ADDR_RANDOM_SEED;            // 0x4243FC
    constexpr uintptr_t GAME_MODE            = FM2K::ADDR_GAME_MODE;              // 0x425558
    // Block C
    constexpr uintptr_t TIMER_BLOCKS         = 0x509080;
    constexpr size_t    TIMER_BLOCKS_SZ      = 0x30;
    // Block D
    constexpr uintptr_t INPUT_BUF_IDX        = FM2K::ADDR_INPUT_BUFFER_INDEX;     // 0x437700
    constexpr uintptr_t P1_INPUT_HISTORY     = FM2K::ADDR_P1_INPUT_HISTORY;       // 0x431720
    constexpr uintptr_t P2_INPUT_HISTORY     = FM2K::ADDR_P2_INPUT_HISTORY;       // 0x431B20
    constexpr uintptr_t INPUT_HISTORY_EXTRA  = 0x431320;
    constexpr size_t    HISTORY_RING_SZ      = 0x400;       // 256 × 4
    constexpr uintptr_t P1_INPUT_CURRENT     = FM2K::ADDR_P1_INPUT;               // 0x437750
    constexpr uintptr_t P2_INPUT_CURRENT     = FM2K::ADDR_P2_INPUT;               // 0x437754
    constexpr uintptr_t P1_INPUT_PERSISTENT  = 0x425500;
    constexpr uintptr_t INPUT_EDGE_STATE     = 0x4255A8;
    constexpr size_t    INPUT_EDGE_STATE_SZ  = 0x10;        // p1/p2 current + pressed, 4 dwords
    // Block E + F merged
    constexpr uintptr_t PLAYER_ROUND_STATE   = 0x5E98A0;
    constexpr size_t    PLAYER_ROUND_STATE_SZ = 0x1A0;      // through 0x5E9A40 inclusive of round_count_max+4
}

bool SaveState_Save(int frame) {
    if (frame < 0) frame = 0;
    const int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* s = &g_state_buffer[slot];

    s->frame_number         = (uint32_t)frame;
    s->rng_seed             = *(uint32_t*)fm95save::RNG_SEED;
    s->input_buffer_index   = *(uint32_t*)fm95save::INPUT_BUF_IDX;
    s->render_frame_counter = *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    s->game_mode            = *(uint32_t*)fm95save::GAME_MODE;

    std::memcpy(s->object_pool,         (const void*)fm95save::OBJECT_POOL,
                sizeof(s->object_pool));
    std::memcpy(s->timer_blocks,        (const void*)fm95save::TIMER_BLOCKS,
                fm95save::TIMER_BLOCKS_SZ);
    std::memcpy(s->p1_input_history,    (const void*)fm95save::P1_INPUT_HISTORY,
                fm95save::HISTORY_RING_SZ);
    std::memcpy(s->p2_input_history,    (const void*)fm95save::P2_INPUT_HISTORY,
                fm95save::HISTORY_RING_SZ);
    std::memcpy(s->input_history_extra, (const void*)fm95save::INPUT_HISTORY_EXTRA,
                fm95save::HISTORY_RING_SZ);
    s->p1_input_current     = *(uint32_t*)fm95save::P1_INPUT_CURRENT;
    s->p2_input_current     = *(uint32_t*)fm95save::P2_INPUT_CURRENT;
    s->p1_input_persistent  = *(uint32_t*)fm95save::P1_INPUT_PERSISTENT;
    std::memcpy(s->input_edge_state,    (const void*)fm95save::INPUT_EDGE_STATE,
                fm95save::INPUT_EDGE_STATE_SZ);
    std::memcpy(s->player_round_state,  (const void*)fm95save::PLAYER_ROUND_STATE,
                fm95save::PLAYER_ROUND_STATE_SZ);

    // Mike Z sound desired — engine-agnostic snapshot.
    SoundRollback::CaptureDesired(s->sound_desired);

    // Combined Fletcher32 over all captured regions for desync detection.
    s->checksum = Fletcher32((const uint8_t*)s->object_pool,
                             sizeof(s->object_pool));
    s->checksum ^= Fletcher32(s->p1_input_history, fm95save::HISTORY_RING_SZ);
    s->checksum ^= Fletcher32(s->p2_input_history, fm95save::HISTORY_RING_SZ);
    s->checksum ^= Fletcher32(s->player_round_state,
                              fm95save::PLAYER_ROUND_STATE_SZ);
    s->checksum ^= s->rng_seed ^ s->render_frame_counter ^ s->game_mode;

    g_last_saved_slot  = slot;
    g_last_saved_frame = frame;
    return true;
}

bool SaveState_Load(int frame) {
    if (frame < 0) frame = 0;
    const int slot = frame % MAX_ROLLBACK_FRAMES;
    const SaveStateData* s = &g_state_buffer[slot];

    if ((int)s->frame_number != frame) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_Load: requested frame=%d but slot %d holds frame=%u",
            frame, slot, s->frame_number);
        return false;
    }

    *(uint32_t*)fm95save::RNG_SEED          = s->rng_seed;
    *(uint32_t*)fm95save::INPUT_BUF_IDX     = s->input_buffer_index;
    *(uint32_t*)fm95save::GAME_TICK_COUNTER = s->render_frame_counter;
    *(uint32_t*)fm95save::GAME_MODE         = s->game_mode;

    std::memcpy((void*)fm95save::OBJECT_POOL,         s->object_pool,
                sizeof(s->object_pool));
    std::memcpy((void*)fm95save::TIMER_BLOCKS,        s->timer_blocks,
                fm95save::TIMER_BLOCKS_SZ);
    std::memcpy((void*)fm95save::P1_INPUT_HISTORY,    s->p1_input_history,
                fm95save::HISTORY_RING_SZ);
    std::memcpy((void*)fm95save::P2_INPUT_HISTORY,    s->p2_input_history,
                fm95save::HISTORY_RING_SZ);
    std::memcpy((void*)fm95save::INPUT_HISTORY_EXTRA, s->input_history_extra,
                fm95save::HISTORY_RING_SZ);
    *(uint32_t*)fm95save::P1_INPUT_CURRENT    = s->p1_input_current;
    *(uint32_t*)fm95save::P2_INPUT_CURRENT    = s->p2_input_current;
    *(uint32_t*)fm95save::P1_INPUT_PERSISTENT = s->p1_input_persistent;
    std::memcpy((void*)fm95save::INPUT_EDGE_STATE,    s->input_edge_state,
                fm95save::INPUT_EDGE_STATE_SZ);
    std::memcpy((void*)fm95save::PLAYER_ROUND_STATE,  s->player_round_state,
                fm95save::PLAYER_ROUND_STATE_SZ);

    SoundRollback::RestoreDesired(s->sound_desired);
    return true;
}

uint32_t SaveState_CalculateFingerprint() {
    // Lightweight gameplay-visible hash — hits the bits that two peers
    // running the same input stream MUST match: RNG seed, frame counter,
    // game mode, and the per-player score block (positions, HP, meters).
    uint32_t h = *(uint32_t*)fm95save::RNG_SEED;
    h ^= *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    h ^= *(uint32_t*)fm95save::GAME_MODE;
    h ^= Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                    fm95save::PLAYER_ROUND_STATE_SZ);
    return h;
}

uint32_t SaveState_CalculateFullChecksum() {
    uint32_t h = Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                            FM2K::SIZE_OBJECT_POOL);
    h ^= Fletcher32((const uint8_t*)fm95save::P1_INPUT_HISTORY,
                    fm95save::HISTORY_RING_SZ);
    h ^= Fletcher32((const uint8_t*)fm95save::P2_INPUT_HISTORY,
                    fm95save::HISTORY_RING_SZ);
    h ^= Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                    fm95save::PLAYER_ROUND_STATE_SZ);
    h ^= *(uint32_t*)fm95save::RNG_SEED;
    h ^= *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    h ^= *(uint32_t*)fm95save::GAME_MODE;
    return h;
}

StateSnapshot SaveState_CaptureSnapshot() {
    StateSnapshot snap{};
    snap.rng_seed           = *(uint32_t*)fm95save::RNG_SEED;
    snap.input_buffer_index = *(uint32_t*)fm95save::INPUT_BUF_IDX;
    snap.frame_counter      = *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    // Player positions live in the object pool; pull from slot 0 / slot 1
    // (player main objects) at offset +36 (pos_x) / +40 (pos_y) per the
    // FM95::ObjectSlot layout.
    const uint8_t* pool = (const uint8_t*)fm95save::OBJECT_POOL;
    snap.p1_x = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 36);
    snap.p1_y = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 40);
    snap.p2_x = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 36);
    snap.p2_y = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 40);
    // HP at slot offset +72 per FM95::ObjectSlot.
    snap.p1_hp = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 72);
    snap.p2_hp = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 72);
    snap.checksum = SaveState_CalculateFingerprint();
    return snap;
}

bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b,
                                char* diff_buf, size_t buf_size) {
    bool ok = (a.rng_seed == b.rng_seed)
           && (a.input_buffer_index == b.input_buffer_index)
           && (a.frame_counter == b.frame_counter)
           && (a.p1_x == b.p1_x) && (a.p1_y == b.p1_y)
           && (a.p2_x == b.p2_x) && (a.p2_y == b.p2_y)
           && (a.p1_hp == b.p1_hp) && (a.p2_hp == b.p2_hp);
    if (diff_buf && buf_size > 0) {
        if (ok) {
            std::snprintf(diff_buf, buf_size, "ok");
        } else {
            std::snprintf(diff_buf, buf_size,
                "rng %08X/%08X buf %u/%u tick %u/%u "
                "p1 (%u,%u) hp=%u / (%u,%u) hp=%u "
                "p2 (%u,%u) hp=%u / (%u,%u) hp=%u",
                a.rng_seed, b.rng_seed,
                a.input_buffer_index, b.input_buffer_index,
                a.frame_counter, b.frame_counter,
                a.p1_x, a.p1_y, a.p1_hp,
                b.p1_x, b.p1_y, b.p1_hp,
                a.p2_x, a.p2_y, a.p2_hp,
                b.p2_x, b.p2_y, b.p2_hp);
        }
    }
    return ok;
}

bool SaveState_TestRoundtrip() {
    // Save → mutate one byte of the object pool → Load → verify byte
    // restored. Cheap correctness check we can call on demand from a
    // dev-panel button.
    if (!SaveState_Save(0)) return false;
    uint8_t* pool = (uint8_t*)fm95save::OBJECT_POOL;
    const uint8_t before = pool[0];
    pool[0] = (uint8_t)~before;
    if (!SaveState_Load(0)) return false;
    const bool ok = (pool[0] == before);
    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_TestRoundtrip: object_pool[0] = %02X expected %02X",
            pool[0], before);
    }
    return ok;
}

uint32_t SaveState_GetLastChecksum(int frame) {
    if (frame < 0) frame = 0;
    return g_state_buffer[frame % MAX_ROLLBACK_FRAMES].checksum;
}

void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc,
                                    uint32_t remote_crc, int player_index) {
    char filename[MAX_PATH];
    char base[64];
    std::snprintf(base, sizeof(base),
                  "FM95_P%d_desync_f%d.log", player_index + 1, frame);
    if (!Fm2k_BuildLogPath(filename, sizeof(filename), base)) {
        std::snprintf(filename, sizeof(filename), "%s", base);
    }
    FILE* f = std::fopen(filename, "w");
    if (!f) return;
    std::fprintf(f, "=== FM95 Desync Diagnostic — frame %d ===\n", frame);
    std::fprintf(f, "Local CRC : 0x%08X\n", local_crc);
    std::fprintf(f, "Remote CRC: 0x%08X\n", remote_crc);
    std::fprintf(f, "RNG seed  : 0x%08X\n", *(uint32_t*)fm95save::RNG_SEED);
    std::fprintf(f, "Tick      : %u\n",
                 *(uint32_t*)fm95save::GAME_TICK_COUNTER);
    std::fprintf(f, "Mode      : %u\n", *(uint32_t*)fm95save::GAME_MODE);
    std::fprintf(f, "ObjPool   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                            FM2K::SIZE_OBJECT_POOL));
    std::fprintf(f, "P1 ring   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::P1_INPUT_HISTORY,
                            fm95save::HISTORY_RING_SZ));
    std::fprintf(f, "P2 ring   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::P2_INPUT_HISTORY,
                            fm95save::HISTORY_RING_SZ));
    std::fprintf(f, "PlrRound  : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                            fm95save::PLAYER_ROUND_STATE_SZ));
    std::fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "FM95 desync diagnostic written to %s", filename);
}

const RegionChecksums& SaveState_GetRegionChecksums() {
    static RegionChecksums z = {};
    z.rng         = *(uint32_t*)fm95save::RNG_SEED;
    z.object_pool = Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                               FM2K::SIZE_OBJECT_POOL);
    z.gameplay_fingerprint = SaveState_CalculateFingerprint();
    z.combined    = SaveState_CalculateFullChecksum();
    return z;
}
#endif  // engine guard

