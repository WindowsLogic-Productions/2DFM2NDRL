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

// Per-region checksums for desync investigation (forward-declared here so
// SaveState_Save can snapshot it into per-frame saved_region_crcs; full
// definition + accessors live further down next to the calculator).
static RegionChecksums g_region_checksums = {};

// Replay-save shadow buffer (stress-mode forward-vs-replay diff).
// SaveState_Save runs once per save-event. In stress mode (and during real
// online rollback), GekkoNet may issue a SaveEvent for the SAME frame twice:
// first when the forward sim reaches it, second after a Load+Advance replays
// it. We use the FIRST save's per-region CRCs as "forward" (kept in
// g_state_buffer[slot].saved_region_crcs), and the SECOND save's as
// "replay" (stored here). Diffing these in the desync dump pinpoints which
// region replayed nondeterministically.
struct ReplaySaveSnapshot {
    int32_t  frame_number;        // -1 if no replay save for this slot
    bool     valid;
    SaveStateData::SavedRegionCRCs crcs;
};
static ReplaySaveSnapshot g_replay_saves[64 /* MAX_ROLLBACK_FRAMES */] = {};

// ============================================================================
// RNGTRACE RING BUFFER
// Records {frame, rng, fingerprint} per authoritative save into a fixed-size
// ring in memory. Console logging every save tanks framerate (~100 Hz logs
// through quill/SDL), so we write bytes into memory at ~100 B/frame and only
// flush to a file on demand (desync event, session end). Stores the last
// ~10 minutes of gameplay at 100 fps = 60000 entries × 12 B = 720 KB.
// ============================================================================
struct RngTraceEntry {
    int32_t  frame;
    uint32_t rng;
    uint32_t fingerprint;
};
static constexpr size_t RNGTRACE_CAPACITY = 60000;  // ~10 min at 100 fps
static RngTraceEntry g_rngtrace_ring[RNGTRACE_CAPACITY];
static size_t g_rngtrace_head   = 0;   // next write slot (mod capacity)
static size_t g_rngtrace_count  = 0;   // total pushed (clamped at capacity for wrap)

void SaveState_PushRngTrace(int frame, uint32_t rng, uint32_t fp) {
    g_rngtrace_ring[g_rngtrace_head] = { frame, rng, fp };
    g_rngtrace_head = (g_rngtrace_head + 1) % RNGTRACE_CAPACITY;
    if (g_rngtrace_count < RNGTRACE_CAPACITY) g_rngtrace_count++;
}

void SaveState_FlushRngTrace(int player_index, const char* reason) {
    char filename[256];
    snprintf(filename, sizeof(filename), "FM2K_P%d_rngtrace.csv", player_index + 1);
    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "rngtrace: failed to open %s", filename);
        return;
    }
    fprintf(f, "# rngtrace flush reason=%s count=%zu\n", reason ? reason : "",
            g_rngtrace_count);
    fprintf(f, "frame,rng,fingerprint\n");
    // Walk from oldest to newest
    size_t start = (g_rngtrace_count < RNGTRACE_CAPACITY)
                   ? 0
                   : g_rngtrace_head;  // head points at the oldest when full
    for (size_t i = 0; i < g_rngtrace_count; i++) {
        const auto& e = g_rngtrace_ring[(start + i) % RNGTRACE_CAPACITY];
        fprintf(f, "%d,0x%08X,0x%08X\n", e.frame, e.rng, e.fingerprint);
    }
    fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "rngtrace: wrote %zu entries to %s (%s)",
                g_rngtrace_count, filename, reason ? reason : "");
}

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

// Single replay-diff log per battle session. Truncated in SaveState_Init,
// then appended every time a replay save diverges from its forward save.
// Replaces the old one-file-per-frame scheme that flooded the disk.
constexpr const char* REPLAY_DIFF_LOG = "FM2K_replay_diffs.log";

void SaveState_Init() {
    memset(g_state_buffer, 0, sizeof(g_state_buffer));
    // Reset replay-shadow buffer so stale forward-vs-replay diffs from a
    // previous battle don't show up on the first desync of a new session.
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        g_replay_saves[i].frame_number = -1;
        g_replay_saves[i].valid = false;
    }
    // Truncate the consolidated replay-diff log so each battle session starts
    // with a fresh file.
    if (FILE* f = fopen(REPLAY_DIFF_LOG, "w")) {
        fprintf(f, "# replay-diff log — appends one block per replay save that\n"
                   "# diverges from its forward save. fopen+fclose per write.\n");
        fclose(f);
    }
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

    // Initial-sync step: force a handful of frame-index / edge-detection
    // counters to a deterministic value so input-change detection produces
    // the same local input stream on both peers. We intentionally do NOT
    // wipe object_pool, char_dynamic, or any other "game state" memory —
    // the GekkoNet checksum is now a gameplay fingerprint (HP/pos/rng/
    // timer), so per-process memory residue is allowed to differ and no
    // longer triggers desync. Wiping pre-battle objects here only broke
    // stage props / HUD sprites that the game had already populated.
    if (!g_initial_sync_done) {
        *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = 0;         // Reset buf_idx
        *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = 0;       // Reset render frame counter
        // Clear input-related state ONLY - do NOT clear screen dimensions!
        // 0x447EE0-0x447F80 contains wDest, hDest, g_screen_x, g_screen_y
        // which render_frame and camera_manager need. Zeroing them kills rendering.
        memset((void*)0x447F00, 0, 0x20);   // g_prev_input_state
        memset((void*)0x447F40, 0, 0x20);   // g_processed_input
        memset((void*)0x447F60, 0, 0x20);   // g_input_changes
        memset((void*)ADDR_INPUT_HISTORY, 0, SIZE_INPUT_HISTORY);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState: Initial sync - reset buf_idx/render_fc/input edge state only");
        g_initial_sync_done = true;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    // Detect: is this a re-save of a frame we already saved (= REPLAY save
    // following a Load+Advance), or the first save (= FORWARD-sim save)?
    // The slot is keyed by (frame % MAX), so a true re-save has a matching
    // frame_number AND a valid prior CRC snapshot.
    bool is_replay_save = (state->frame_number == (uint32_t)frame
                        && state->saved_region_crcs.valid);

    // Byte-level first-diff detector + side-file writer. MUST run BEFORE the
    // memcpy block below overwrites state->object_pool / afterimage_pool /
    // char_dynamic with live (replay) bytes — otherwise fwd and cur become
    // identical and the scan reports nothing. On a replay save,
    // state->object_pool still holds the FORWARD bytes from the first save.
    //
    // Replay-diff entries get appended to a SINGLE consolidated file
    // (REPLAY_DIFF_LOG = "FM2K_replay_diffs.log") to avoid flooding the disk
    // with one file per frame. Each entry is delimited by a header line so
    // they're easy to grep/separate.
    if (is_replay_save) {
        const auto& fwd_crcs = state->saved_region_crcs;

        // Compute current (replay-side) region CRCs inline so we can gate the
        // diff log on actual divergence. Per-region only here — full
        // this_save_crcs is built AFTER the memcpy below.
        uint32_t cur_obj_crc = Fletcher32((uint8_t*)ADDR_OBJECT_POOL,
                                          SIZE_OBJECT_POOL);
        uint32_t cur_ai_crc  = Fletcher32((uint8_t*)WaveCAddrs::AFTERIMAGE_POOL,
                                          WaveCAddrs::AFTERIMAGE_POOL_SZ);
        uint32_t cur_char_dyn = 0;
        for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
            uintptr_t da = CHAR_SLOT_BASE + i * CHAR_SLOT_SIZE + CHAR_SLOT_DYNAMIC_OFFSET;
            cur_char_dyn ^= Fletcher32((uint8_t*)da, CHAR_SLOT_DYNAMIC_SIZE);
        }

        bool any_region_diff = (fwd_crcs.object_pool     != cur_obj_crc)
                            || (fwd_crcs.afterimage_pool != cur_ai_crc)
                            || (fwd_crcs.char_dynamic    != cur_char_dyn);

        FILE* df = nullptr;
        if (any_region_diff) {
            df = fopen(REPLAY_DIFF_LOG, "a");  // append to the consolidated log
            if (df) {
                fprintf(df, "\n========================================\n");
                fprintf(df, "REPLAY DIFF for frame %d\n", frame);
                fprintf(df, "========================================\n");
                fprintf(df, "  forward.combined=0x%08X\n", fwd_crcs.combined);
                fprintf(df, "  forward.rng=0x%08X\n", fwd_crcs.rng);
                fprintf(df, "  ObjectPool:     fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.object_pool, cur_obj_crc,
                        fwd_crcs.object_pool == cur_obj_crc ? "MATCH" : "DIFF");
                fprintf(df, "  AfterimagePool: fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.afterimage_pool, cur_ai_crc,
                        fwd_crcs.afterimage_pool == cur_ai_crc ? "MATCH" : "DIFF");
                fprintf(df, "  CharDynamic:    fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.char_dynamic, cur_char_dyn,
                        fwd_crcs.char_dynamic == cur_char_dyn ? "MATCH" : "DIFF");
            }
        }

        // ObjectPool byte scan — pinpoints first divergent object + field.
        if (fwd_crcs.object_pool != cur_obj_crc) {
            const uint8_t* fwd = state->object_pool;
            const uint8_t* cur = (const uint8_t*)ADDR_OBJECT_POOL;
            for (size_t i = 0; i < SIZE_OBJECT_POOL; i++) {
                if (fwd[i] != cur[i]) {
                    int obj_idx = (int)(i / 382);
                    int field_off = (int)(i % 382);
                    uintptr_t obj_base = ADDR_OBJECT_POOL + obj_idx * 382;
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "REPLAY DIFF f=%d ObjectPool: obj[%d] @ 0x%X +0x%X (=%d) "
                        "forward=0x%02X replay=0x%02X",
                        frame, obj_idx, (unsigned)obj_base, field_off, field_off,
                        fwd[i], cur[i]);
                    if (df) {
                        fprintf(df, "\nFirst differing BYTE in ObjectPool:\n"
                                "  obj[%d] @ 0x%08X +0x%X (= +%d)\n"
                                "  forward = 0x%02X  replay = 0x%02X\n"
                                "  fwd ctx (16 B): ",
                                obj_idx, (unsigned)obj_base, field_off, field_off,
                                fwd[i], cur[i]);
                        for (int k = 0; k < 16 && i+k < SIZE_OBJECT_POOL; k++)
                            fprintf(df, "%02X ", fwd[i+k]);
                        fprintf(df, "\n  replay ctx     : ");
                        for (int k = 0; k < 16 && i+k < SIZE_OBJECT_POOL; k++)
                            fprintf(df, "%02X ", cur[i+k]);
                        fprintf(df,
                            "\n  KgtRuntimeObject field map:\n"
                            "    +0..3 state | +4..7 facing | +8..15 xPos/yPos (q16)\n"
                            "    +40 flags40 | +44 itemIdx | +48 scriptId | +52 prevScriptId\n"
                            "    +60 wait | +88 gravBase | +92 facing | +137..216 obj-mgr A\n"
                            "    +217..296 box-ptr array | +338 stateInit | +342 pid\n"
                            "    +346 role | +350 flags350\n");
                    }
                    break;
                }
            }
        }
        // AfterimagePool byte scan
        if (fwd_crcs.afterimage_pool != cur_ai_crc) {
            const uint8_t* fwd = state->afterimage_pool;
            const uint8_t* cur = (const uint8_t*)WaveCAddrs::AFTERIMAGE_POOL;
            for (size_t i = 0; i < WaveCAddrs::AFTERIMAGE_POOL_SZ; i++) {
                if (fwd[i] != cur[i]) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "REPLAY DIFF f=%d AfterimagePool: +0x%X fwd=0x%02X replay=0x%02X",
                        frame, (unsigned)i, fwd[i], cur[i]);
                    if (df) {
                        fprintf(df,
                            "\nFirst differing BYTE in AfterimagePool:\n"
                            "  +0x%X (= byte %zu) fwd=0x%02X replay=0x%02X\n",
                            (unsigned)i, i, fwd[i], cur[i]);
                    }
                    break;
                }
            }
        }
        // CharDynamic byte scan (per slot)
        if (fwd_crcs.char_dynamic != cur_char_dyn) {
            for (size_t s = 0; s < NUM_CHAR_SLOTS; s++) {
                const uint8_t* fwd = state->char_dynamic[s];
                const uint8_t* cur = (const uint8_t*)(CHAR_SLOT_BASE + s * CHAR_SLOT_SIZE);
                bool found = false;
                for (size_t i = 0; i < CHAR_SLOT_DYNAMIC_SIZE; i++) {
                    if (fwd[i] != cur[i]) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                            "REPLAY DIFF f=%d CharSlot[%zu]: +0x%X fwd=0x%02X replay=0x%02X",
                            frame, s, (unsigned)i, fwd[i], cur[i]);
                        if (df) {
                            fprintf(df,
                                "\nFirst differing BYTE in CharSlot[%zu]:\n"
                                "  +0x%X fwd=0x%02X replay=0x%02X\n",
                                s, (unsigned)i, fwd[i], cur[i]);
                        }
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }

        if (df) fclose(df);  // explicit flush — survives TerminateProcess
    }

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

    // Wave C additions: afterimage pool + object list topology.
    memcpy(state->afterimage_pool,         (void*)WaveCAddrs::AFTERIMAGE_POOL,      WaveCAddrs::AFTERIMAGE_POOL_SZ);
    state->current_object_ptr            = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;
    memcpy(state->object_list_heads_tails, (void*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    memcpy(state->object_node_pool,        (void*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);

    // Calculate checksum for desync detection using full Fletcher32
    // Covers RNG, input tracking, character dynamic state, object pool, game state
    state->checksum = SaveState_CalculateFullChecksum();
    uint32_t checksum = state->checksum;

    // Build a fresh per-region CRC snapshot for THIS save (forward or replay).
    SaveStateData::SavedRegionCRCs this_save_crcs = {};
    {
        const auto& rc = g_region_checksums;
        this_save_crcs.rng                    = rc.rng;
        this_save_crcs.game_state             = rc.game_state;
        this_save_crcs.object_pool            = rc.object_pool;
        this_save_crcs.char_dynamic           = rc.char_dynamic;
        this_save_crcs.input_tracking         = rc.input_tracking;
        this_save_crcs.effect_sys1            = rc.effect_sys1;
        this_save_crcs.effect_sys2            = rc.effect_sys2;
        this_save_crcs.shake_effects          = rc.shake_effects;
        this_save_crcs.gameplay_fingerprint   = rc.gameplay_fingerprint;
        this_save_crcs.combined               = rc.combined;
        this_save_crcs.afterimage_pool        = Fletcher32((uint8_t*)WaveCAddrs::AFTERIMAGE_POOL,      WaveCAddrs::AFTERIMAGE_POOL_SZ);
        this_save_crcs.list_heads_tails       = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
        this_save_crcs.node_pool              = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);
        this_save_crcs.current_object_ptr_val = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;

        // Per-object-slot CRCs. Cheap (~400 KB of Fletcher32/tick) and lets
        // the desync dump report the exact slot index that diverged without
        // needing to scrape the SDL console for the REPLAY DIFF line.
        for (size_t i = 0; i < SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT; i++) {
            const uint8_t* p = (const uint8_t*)(ADDR_OBJECT_POOL + i * SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE);
            this_save_crcs.object_slot_crcs[i] = Fletcher32(p, SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE);
        }

        this_save_crcs.valid = true;
    }

    if (is_replay_save) {
        // Second save for this frame = REPLAY save. Keep forward (already in
        // state->saved_region_crcs) and stash this snapshot in the replay
        // shadow keyed by slot. The desync dump diffs the two.
        // (The byte-level + side-file diff was already written EARLIER, before
        // the memcpy block, so state->object_pool still held forward's bytes.)
        g_replay_saves[slot].frame_number = frame;
        g_replay_saves[slot].crcs         = this_save_crcs;
        g_replay_saves[slot].valid        = true;
    } else {
        // First save for this frame = FORWARD save. Stash here, clear any
        // stale replay shadow for this slot from a previous match.
        state->saved_region_crcs       = this_save_crcs;
        g_replay_saves[slot].frame_number = -1;
        g_replay_saves[slot].valid        = false;
    }

    // Per-save logging.
    //   First 10 frames: verbose (combined + fingerprint + rng + buf_idx).
    //   After that: push into the in-memory rngtrace ring. Writing a console
    //   line every save at 100 fps tanks framerate and is user-visible.
    //   The ring is flushed to FM2K_P<N>_rngtrace.bin when SaveState_FlushRngTrace
    //   is called (on desync detection or session end). Non-rollback saves
    //   only — replay ticks are not part of the authoritative per-frame trace.
    static int save_log_count = 0;
    if (save_log_count++ < 10) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SAVE f=%d: combined=0x%08X fingerprint=0x%08X (rng=0x%08X) buf_idx=%u",
            frame, checksum, SaveState_GetRegionChecksums().gameplay_fingerprint,
            state->rng_seed, state->input_buffer_index);
    } else if (!g_is_rolling_back) {
        SaveState_PushRngTrace(frame, state->rng_seed,
            SaveState_GetRegionChecksums().gameplay_fingerprint);
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

    // Wave C additions: afterimage pool + object list topology.
    memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,      state->afterimage_pool,         WaveCAddrs::AFTERIMAGE_POOL_SZ);
    *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR    = state->current_object_ptr;
    memcpy((void*)WaveCAddrs::OBJECT_LIST_HEADS,    state->object_list_heads_tails, WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    memcpy((void*)WaveCAddrs::OBJECT_NODE_POOL,     state->object_node_pool,        WaveCAddrs::OBJECT_NODE_POOL_SZ);

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

// (g_region_checksums defined earlier for forward-reference in SaveState_Save)

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

    // Object pool: hash the FULL pool, not just the first 4KB.
    // Previous 4 KB covered only ~10 objects; stress-mode desync at frame 273
    // showed Obj[7] CRC changing without the hashed region moving, meaning the
    // 4KB hash was silent to divergence past object 10. Hash the full 391 KB
    // so ANY object mutation shows up in the combined checksum immediately.
    g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, SIZE_OBJECT_POOL);

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
    // History ring buffers: 1024 frames × 2 bytes per player. Must match across
    // peers byte-for-byte — if a facing/slot swap mutates the stored input on
    // one side and not the other, these CRCs diverge on frame 1 and give us
    // an early desync signal instead of waiting for the cascade into game_state.
    uint32_t it4 = Fletcher32((uint8_t*)0x4280E0, 0x800);                 // g_p1_input_history (uint16[1024])
    uint32_t it5 = Fletcher32((uint8_t*)0x4290E0, 0x800);                 // g_p2_input_history (uint16[1024])
    g_region_checksums.input_tracking = it1 ^ it2 ^ it3 ^ it4 ^ it5;

    // Effect/shake regions (now saved for rollback)
    g_region_checksums.effect_sys1 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    g_region_checksums.effect_sys2 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    g_region_checksums.shake_effects = Fletcher32((uint8_t*)EffectAddrs::SHAKE_EFFECTS, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Wave C regions — PREVIOUSLY SAVED BUT NOT HASHED.
    // These silently diverged until their effects leaked into the object pool
    // or slot state. Hashing them now means any divergence trips the combined
    // checksum at its source frame instead of cascading.
    uint32_t afterimage_ck = Fletcher32((uint8_t*)WaveCAddrs::AFTERIMAGE_POOL,      WaveCAddrs::AFTERIMAGE_POOL_SZ);
    uint32_t list_heads_ck = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    uint32_t node_pool_ck  = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);
    uint32_t cur_obj_ptr   = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;

    // Combined CRC for GekkoNet desync detection.
    // Principle (matches GekkoNet examples): hash every byte you save. Any
    // saved-but-not-hashed region silently diverges until it leaks elsewhere.
    // Only render_fc and g_hInstance are legitimately process-local and stay
    // out of the hash (render_fc: render-only counter; g_hInstance: excluded
    // upstream from the game_state sub-hash).
    //
    // If this triggers an immediate-start desync, the CSS residue theory that
    // justified the old exclusions is real and must be fixed at the residue
    // source (zero residue deterministically on both peers before
    // SaveState_Init), NOT by re-excluding regions here.
    {
        uint32_t combined = 0x811C9DC5;  // FNV offset basis
        auto mix = [&](uint32_t v) {
            combined ^= v;
            combined *= 0x01000193;
        };
        mix(g_region_checksums.rng);
        mix(g_region_checksums.game_state);
        mix(g_region_checksums.input_tracking);
        mix(g_region_checksums.object_pool);
        mix(g_region_checksums.char_dynamic);
        mix(g_region_checksums.effect_sys1);
        mix(g_region_checksums.effect_sys2);
        mix(g_region_checksums.shake_effects);
        // Wave C regions — now properly included
        mix(afterimage_ck);
        mix(list_heads_ck);
        mix(node_pool_ck);
        mix(cur_obj_ptr);
        g_region_checksums.combined = combined;
    }

    // Gameplay fingerprint: a curated hash over only game-visible fields.
    // Addresses VERIFIED against live IDA symbols (2026-04-23):
    //   - RNG seed:   g_rand_seed @ 0x41FB1C
    //   - P1 HP:      g_p1_hp     @ 0x4DFC85        (per Wave C audit)
    //   - P2 HP:      g_p2_hp     @ g_p1_hp + 57407 = 0x4EDCC4
    //   - game/round: g_game_timer @ 0x470044, g_round_timer @ 0x470060
    //   - P1/P2 current input: g_p1_input_history[buf_idx], g_p2_input_history[buf_idx]
    // PREVIOUS BUG: the fingerprint read 0x47010C / 0x470110 / 0x47030C / 0x470310
    // thinking those were HP fields. They are actually g_demo_mode_player_id and
    // g_demo_mode_hp (per IDA). The fingerprint was hashing demo-mode state, not
    // gameplay HP. During intro (HP=0/0) it coincidentally matched; during
    // actual combat the HP mismatch would hide behind an always-equal demo field.
    //
    // Stage positions are intentionally NOT in the fingerprint because
    // g_player_stage_positions is a 36-byte block with slot sentinels (0xFF
    // fills) that are legitimately process-local CSS residue. Player world
    // position is instead captured via the object pool (obj[0]/obj[1] +0x8/+0xC)
    // which is part of char_dynamic saves.
    {
        uint32_t buf_idx = *(uint32_t*)0x447EE0;     // g_input_buffer_index
        uint32_t idx     = buf_idx & 0x3FF;          // 1024-frame ring
        // Per-frame sampled inputs
        uint16_t p1_in = ((const uint16_t*)0x4280E0)[idx];  // g_p1_input_history
        uint16_t p2_in = ((const uint16_t*)0x4290E0)[idx];  // g_p2_input_history
        struct __attribute__((packed)) Fingerprint {
            uint32_t rng;
            uint32_t p1_hp;
            uint32_t p2_hp;
            uint32_t round_timer;
            uint32_t game_timer;
            uint16_t p1_input;
            uint16_t p2_input;
        } fp = {
            *(uint32_t*)0x41FB1C,   // g_rand_seed
            *(uint32_t*)0x4DFC85,   // g_p1_hp          (Wave C)
            *(uint32_t*)0x4EDCC4,   // g_p2_hp = g_p1_hp + 57407
            *(uint32_t*)0x470060,   // g_round_timer
            *(uint32_t*)0x470044,   // g_game_timer
            p1_in,
            p2_in,
        };
        g_region_checksums.gameplay_fingerprint =
            Fletcher32((const uint8_t*)&fp, sizeof(fp));
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
    // Object pool windows — hex dumps for small ones (<=256B), CRCs for large.
    // Combined CRC now covers the FULL 391 KB pool; these windows localize any
    // divergence to specific object-index ranges in the diagnostic log.
    { 0x4701E0, 0x100, "ObjectPool_First256",          false },  // Obj[0] first 256 B
    { 0x4702E0, 0x100, "ObjectPool_Second256",         false },  // Obj[0] tail + Obj[1] first 130 B
    { 0x470C52, 0x100, "ObjectPool_Obj7_first256",     false },  // Obj[7] — was changing invisibly pre-fix
    { 0x4703E0, 0x800, "ObjectPool_Obj_2_10_CRC",      false },  // Obj[2..10] aggregate CRC (no hex)
    { 0x471000, 0x800, "ObjectPool_Obj_11_20_CRC",     false },  // Obj[11..20] aggregate CRC
    { 0x4720C2, 0x2000,"ObjectPool_Obj_21_42_CRC",     false },  // Obj[21..42] aggregate CRC (approx)
    { 0x447D7D,  0x2A, "EffectSystem1",                false },  // saved + hashed
    { 0x4456D0,  0x2C, "EffectSystem2",                false },  // saved + hashed
    { 0x447DA9,  0x28, "ShakeEffects",                 false },  // saved + hashed
    // Wave C regions — now saved AND hashed in combined CRC. Showing their
    // per-region CRCs here lets us pinpoint which of these 4 newly-hashed
    // regions is the divergent one when the next stress desync fires.
    { 0x4259A8,   0x4, "CurrentObjectPtr",             false },  // g_current_object_ptr
    { 0x430240, 0x400, "ObjectListHeadsTails",         false },  // 256 heads + 256 tails
    { 0x4CFA20,0x2000, "ObjectNodePool",               false },  // 1024 × 8-byte nodes
    { 0x447930,0x27D90,"AfterimagePool",               false },  // 162,704 B (includes preamble)
};
static constexpr int NUM_DIAG_REGIONS = sizeof(g_diag_regions) / sizeof(g_diag_regions[0]);

void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc, uint32_t remote_crc, int player_index) {
    char filename[256];
    if (g_stress_mode) {
        // Stress-mode desyncs are single-instance determinism failures —
        // tag the file so they're distinguishable from online desyncs.
        snprintf(filename, sizeof(filename), "FM2K_stress_desync_f%d.log", frame);
    } else {
        snprintf(filename, sizeof(filename), "FM2K_P%d_desync_f%d.log", player_index + 1, frame);
    }

    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open desync diagnostic file: %s", filename);
        return;
    }

    fprintf(f, "=== FM2K DESYNC DIAGNOSTIC%s ===\n", g_stress_mode ? " (STRESS TEST)" : "");
    fprintf(f, "Player: P%d%s\n", player_index + 1,
            g_stress_mode ? " (stress mode - single instance determinism failure)" : "");
    fprintf(f, "Frame: %d\n", frame);
    fprintf(f, "Local CRC: 0x%08X\n", local_crc);
    fprintf(f, "Remote CRC: 0x%08X\n", remote_crc);
    // Gameplay fingerprint: if this matches across peers the desync is a
    // memory-residue false positive. Dump raw values so the diff points
    // directly at the first gameplay field that differs, if any.
    fprintf(f, "\n=== Gameplay Fingerprint (HP/rng/timer/current-input ONLY) ===\n");
    fprintf(f, "  fingerprint_crc = 0x%08X\n", g_region_checksums.gameplay_fingerprint);
    fprintf(f, "  rng        = 0x%08X  (g_rand_seed @ 0x41FB1C)\n", *(uint32_t*)0x41FB1C);
    {
        uint32_t buf_idx = *(uint32_t*)0x447EE0 & 0x3FF;
        uint16_t p1_in = ((const uint16_t*)0x4280E0)[buf_idx];
        uint16_t p2_in = ((const uint16_t*)0x4290E0)[buf_idx];
        fprintf(f, "  buf_idx    = %u  (slot %u of 1024)\n",
                *(uint32_t*)0x447EE0, buf_idx);
        fprintf(f, "  p1_input   = 0x%04X  (g_p1_input_history[buf_idx])\n", p1_in);
        fprintf(f, "  p2_input   = 0x%04X  (g_p2_input_history[buf_idx])\n", p2_in);
    }
    fprintf(f, "  p1_hp      = %d  (g_p1_hp @ 0x4DFC85)\n", *(int32_t*)0x4DFC85);
    fprintf(f, "  p2_hp      = %d  (g_p2_hp @ 0x4EDCC4, = g_p1_hp + 57407)\n", *(int32_t*)0x4EDCC4);
    fprintf(f, "  game_timer = %u  (g_game_timer @ 0x470044)\n", *(uint32_t*)0x470044);
    fprintf(f, "  round_timer= %u  (g_round_timer @ 0x470060)\n\n", *(uint32_t*)0x470060);
    // Legacy demo-mode fields — retain for backward diff compatibility. These
    // used to be (mis-)labeled as p1_hp / p1_max_hp / p2_hp / p2_max_hp in
    // pre-audit dumps; they are actually g_demo_mode_player_id / g_demo_mode_hp
    // etc. Keep them visible so pre-existing log comparison scripts don't
    // break, but make clear these are NOT gameplay HP.
    fprintf(f, "  [legacy-aliases, not HP]\n");
    fprintf(f, "    g_demo_mode_player_id @ 0x47010C = %u\n", *(uint32_t*)0x47010C);
    fprintf(f, "    g_demo_mode_hp        @ 0x470110 = %u\n", *(uint32_t*)0x470110);
    fprintf(f, "\n");

    // ============================================================================
    // FORWARD-VS-REPLAY PER-REGION CRC DIFF
    // Compares the per-region CRCs captured at TWO save events for the same
    // frame: the forward-sim save (first time we reached this frame) and the
    // replay save (after Load+Advance re-simulated this frame). Any region
    // whose CRC differs between these two snapshots is a region whose state
    // was NOT deterministically reproduced from the saved state — that IS
    // the bug source. Same logical moment on both sides — no timing artifacts.
    // ============================================================================
    {
        int saved_frame = frame < 0 ? 0 : frame;
        int slot = saved_frame % MAX_ROLLBACK_FRAMES;
        const auto& fwd    = g_state_buffer[slot].saved_region_crcs;
        const auto& replay = g_replay_saves[slot].crcs;
        const bool have_replay = (g_replay_saves[slot].valid &&
                                  g_replay_saves[slot].frame_number == saved_frame);

        fprintf(f, "\n=== FORWARD-vs-REPLAY Per-Region Diff ===\n");
        if (!fwd.valid) {
            fprintf(f, "  (no forward snapshot for frame %d — rollback buffer stale)\n", saved_frame);
        } else if (!have_replay) {
            fprintf(f, "  (no replay snapshot for frame %d — desync may have fired before\n"
                       "   GekkoNet executed a Load+Advance for this frame; only forward shown)\n", saved_frame);
            auto fwd_row = [&](const char* n, uint32_t v) {
                fprintf(f, "  %-24s  0x%08X (forward only)\n", n, v);
            };
            fwd_row("RNG_Seed",             fwd.rng);
            fwd_row("GameState",            fwd.game_state);
            fwd_row("ObjectPool(full)",     fwd.object_pool);
            fwd_row("CharDynamic(8 slots)", fwd.char_dynamic);
            fwd_row("InputTracking",        fwd.input_tracking);
            fwd_row("AfterimagePool",       fwd.afterimage_pool);
            fwd_row("ObjectListHeadsTails", fwd.list_heads_tails);
            fwd_row("ObjectNodePool",       fwd.node_pool);
            fwd_row("CurrentObjectPtr",     fwd.current_object_ptr_val);
            fwd_row("GameplayFingerprint",  fwd.gameplay_fingerprint);
            fwd_row("CombinedCRC",          fwd.combined);
        } else {
            fprintf(f, "  %-24s  %-10s  %-10s  %s\n",
                    "region", "forward", "replay", "status");
            auto row = [&](const char* name, uint32_t a, uint32_t b) {
                const char* status = (a == b) ? "MATCH" : "*** DIFF ***";
                fprintf(f, "  %-24s  0x%08X  0x%08X  %s\n", name, a, b, status);
            };
            row("RNG_Seed",             fwd.rng,                    replay.rng);
            row("GameState",            fwd.game_state,             replay.game_state);
            row("ObjectPool(full)",     fwd.object_pool,            replay.object_pool);
            row("CharDynamic(8 slots)", fwd.char_dynamic,           replay.char_dynamic);
            row("InputTracking",        fwd.input_tracking,         replay.input_tracking);
            row("EffectSystem1",        fwd.effect_sys1,            replay.effect_sys1);
            row("EffectSystem2",        fwd.effect_sys2,            replay.effect_sys2);
            row("ShakeEffects",         fwd.shake_effects,          replay.shake_effects);
            row("AfterimagePool",       fwd.afterimage_pool,        replay.afterimage_pool);
            row("ObjectListHeadsTails", fwd.list_heads_tails,       replay.list_heads_tails);
            row("ObjectNodePool",       fwd.node_pool,              replay.node_pool);
            row("CurrentObjectPtr",     fwd.current_object_ptr_val, replay.current_object_ptr_val);
            row("GameplayFingerprint",  fwd.gameplay_fingerprint,   replay.gameplay_fingerprint);
            row("CombinedCRC",          fwd.combined,               replay.combined);
            fprintf(f,
                "\n  NOTE: forward = CRCs at the FIRST save for frame %d (forward-sim).\n"
                "        replay  = CRCs at the SECOND save for frame %d (after Load+Advance).\n"
                "        Both captured at save time — no timing artifacts from later mutation.\n"
                "        Any *** DIFF *** row is the bug: that region replayed nondeterministically.\n",
                saved_frame, saved_frame);

            // Per-object-slot breakdown. If ObjectPool(full) differed, this
            // pinpoints the exact slot indices. Cross-reference obj_base with
            // IDA (ADDR_OBJECT_POOL + idx*382) to find the writer code.
            if (fwd.object_pool != replay.object_pool) {
                fprintf(f, "\n=== Object Slot Divergence ===\n");
                int printed = 0;
                for (size_t i = 0; i < SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT; i++) {
                    if (fwd.object_slot_crcs[i] != replay.object_slot_crcs[i]) {
                        uintptr_t obj_base =
                            0x4701E0 + i * SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE;
                        fprintf(f, "  obj[%4zu] base=0x%08X  forward_crc=0x%08X  replay_crc=0x%08X\n",
                                i, (unsigned)obj_base,
                                fwd.object_slot_crcs[i], replay.object_slot_crcs[i]);
                        if (++printed >= 32) {
                            fprintf(f, "  ... (truncated; %zu total diverging slots scanned)\n",
                                    SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT);
                            break;
                        }
                    }
                }
                if (printed == 0) {
                    fprintf(f, "  (aggregate object_pool CRC differs but no per-slot CRC does —\n"
                               "   divergence is in the trailing bytes past slot %zu × 382 = 0x%X)\n",
                            SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT,
                            (unsigned)(SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT *
                                       SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE));
                }
            }
        }
        fprintf(f, "\n");
    }

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

    // Input history per-frame table. The histories are 1024-frame rings
    // indexed by g_input_buffer_index at 0x447EE0. We print `window` slots
    // ending at the current buf_idx so both peers dump the same logical
    // range — diff the two files side-by-side and the first differing
    // frame line points directly at the asymmetric input write.
    {
        const uint32_t buf_idx = *(uint32_t*)0x447EE0;
        const uint16_t* p1_hist = (const uint16_t*)0x4280E0;
        const uint16_t* p2_hist = (const uint16_t*)0x4290E0;
        const int window = 256;  // last ~2.5s at 100fps
        fprintf(f, "\n=== Input History Tail (last %d frames up to buf_idx=%u) ===\n",
                window, buf_idx);
        fprintf(f, "  rel  slot  p1_input  p2_input\n");
        for (int i = window - 1; i >= 0; i--) {
            uint32_t slot = (buf_idx - (uint32_t)i) & 0x3FF;  // 1024-slot ring
            int rel = -i;
            fprintf(f, "  %+04d  %4u     0x%04X    0x%04X\n",
                    rel, slot, p1_hist[slot], p2_hist[slot]);
        }
    }

    // Character slots: dump BOTH static and dynamic CRCs.
    //
    // Static region (bytes 0..CHAR_SLOT_DYNAMIC_OFFSET) is the character
    // template loaded from .kgt during CSS. If this region's CRC differs
    // peer-to-peer for the same slot index, the two peers put DIFFERENT
    // characters in the SAME logical slot — CSS character assignment is
    // perspective-based (local vs remote) rather than left vs right.
    // That's the root cause of post-residue-wipe desyncs, because the
    // game's first advance tick reads the template and writes dynamic
    // state derived from it.
    //
    // Dynamic region (bytes CHAR_SLOT_DYNAMIC_OFFSET..end) is runtime
    // state: position, health, move counters. This is what's in the CRC.
    fprintf(f, "\n=== Character Slots (static template + dynamic runtime) ===\n");
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        uint32_t static_crc  = Fletcher32((uint8_t*)slot_base, CHAR_SLOT_DYNAMIC_OFFSET);
        uint32_t dynamic_crc = Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
        fprintf(f, "  Slot[%zu] base=0x%08X  static_crc=0x%08X  dynamic_crc=0x%08X\n",
                i, (unsigned)slot_base, static_crc, dynamic_crc);

        // First 32 bytes of the static region — character ID / name lives at
        // the start of the template for most FM2K games, so a visual diff
        // here immediately says "same char in this slot?".
        const uint8_t* p = (const uint8_t*)slot_base;
        fprintf(f, "    static+000:");
        for (int j = 0; j < 32; j++) fprintf(f, " %02X", p[j]);
        fprintf(f, "\n");
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
