// CSS auto-lock-and-confirm hook — see header for design rationale and
// docs/analysis/css_state_machine.md for the IDA-pass that maps the state
// machine and globals.

#if !defined(ENGINE_FM95)

#include "css_autoconfirm.h"

#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdint>
#include <windows.h>

#include "MinHook.h"

#include "../core/globals.h"  // FM2K::ADDR_GAME_MODE, ADDR_*

namespace {

// FM2K addresses (verified 2026-05-08 via IDA decompile of game_state_manager
// @ 0x406FC0; full xref + flow doc in docs/analysis/css_state_machine.md).
constexpr uintptr_t ADDR_GAME_STATE_MANAGER = 0x00406FC0;
constexpr uintptr_t ADDR_P1_CURSOR_POS      = 0x00424E50;  // i32×2 {x, y}
constexpr uintptr_t ADDR_P2_CURSOR_POS      = 0x00424E58;
constexpr uintptr_t ADDR_SELECTED_CHAR      = 0x00470020;  // i32×2 [p1, p2]
constexpr uintptr_t ADDR_INPUT_CHANGES      = 0x00447F60;  // u32 per-player
constexpr uintptr_t ADDR_PROCESSED_INPUT    = 0x00447F40;  // u32 per-player
constexpr uintptr_t ADDR_P1_ACTION_STATE    = 0x0047019C;
constexpr uintptr_t ADDR_P2_ACTION_STATE    = 0x004701A0;
constexpr uintptr_t ADDR_STAGE_WIDTH        = 0x004452B8;  // u16
constexpr uintptr_t ADDR_STAGE_HEIGHT       = 0x004452BA;  // u16
constexpr uintptr_t ADDR_SELECTED_STAGE     = 0x0043010C;
constexpr uintptr_t ADDR_GAME_MODE          = 0x00470054;

// AssignPlayerColor @ 0x406F20 reads `input_changes & 0x3E0` (bits 5..9) to
// pick color slot 1..5; if none of those bits set, the function falls
// through to color 0. The caller in game_state_manager triggers the call
// only when `input_changes & 0x3F0` (bits 4..9) is non-zero — so bit 4
// (0x10) on its own confirms with color 0.
//
// Earlier versions of this file used a fixed `INJECTED_CONFIRM_BIT = 0x10`
// for both players, which always landed on color 0 regardless of what the
// recorded match used. That made offline replays look like every fight
// confirmed with palette 0 even when the original players picked colors
// 1..5 — visible as wrong palette in the replay's CSS portrait + battle
// sprites.
//
// Replacement: per-player target bit derived from the MATCH_START header's
// p1_color / p2_color, mapped through ColorBitForSlot:
//
//   color 0 → 0x10  (bit 4 — confirm-only, falls through to default 0)
//   color 1 → 0x20  (bit 5)
//   color 2 → 0x40  (bit 6)
//   color 3 → 0x80  (bit 7)
//   color 4 → 0x100 (bit 8)
//   color 5 → 0x200 (bit 9)
//
// We still OVERWRITE the 0x3F0 bits rather than OR — pb_queue's leaked
// battle-input bits could otherwise set a different attack button and shift
// AssignPlayerColor's bit ladder onto the wrong color.
inline uint32_t ColorBitForSlot(uint8_t color) {
    if (color == 0u || color > 5u) return 0x10u;
    return 0x10u << color;  // 0x20, 0x40, 0x80, 0x100, 0x200
}

typedef char (__cdecl *GameStateManagerFn)();
GameStateManagerFn g_orig = nullptr;

// Cached MATCH_START targets. Set by CssAutoConfirm_OnReplayMatchStart, read
// every frame by the detour while active.
std::atomic<bool>    g_active{false};
std::atomic<uint8_t> g_target_p1_char{0};
std::atomic<uint8_t> g_target_p2_char{0};
std::atomic<uint8_t> g_target_p1_color{0};
std::atomic<uint8_t> g_target_p2_color{0};
std::atomic<uint8_t> g_target_stage_id{0};

// Tick counter for diag — log first few frames of pinning then stay quiet so
// CSS doesn't blast the log file with 100+ "pinning chars" lines.
std::atomic<uint32_t> g_pin_tick{0};

char __cdecl Hook_GameStateManager() {
    if (g_active.load(std::memory_order_relaxed)) {
        const uint32_t game_mode = *(uint32_t*)ADDR_GAME_MODE;
        if (game_mode == 2000u) {
            const uint8_t p1c = g_target_p1_char.load(std::memory_order_relaxed);
            const uint8_t p2c = g_target_p2_char.load(std::memory_order_relaxed);
            const uint8_t stg = g_target_stage_id.load(std::memory_order_relaxed);
            const uint16_t sw = *(uint16_t*)ADDR_STAGE_WIDTH;

            if (sw > 0) {
                int*      p1_cur     = (int*)ADDR_P1_CURSOR_POS;
                int*      p2_cur     = (int*)ADDR_P2_CURSOR_POS;
                int*      selected   = (int*)ADDR_SELECTED_CHAR;
                uint32_t* in_changes = (uint32_t*)ADDR_INPUT_CHANGES;
                uint32_t* processed  = (uint32_t*)ADDR_PROCESSED_INPUT;
                const uint32_t p1_act = *(uint32_t*)ADDR_P1_ACTION_STATE;
                const uint32_t p2_act = *(uint32_t*)ADDR_P2_ACTION_STATE;

                // Pin stage. The natural battle-init read site is 0x43010C;
                // writing here ensures recorded stage matches when type-14
                // battle-init spawns post-CSS-timer.
                *(uint32_t*)ADDR_SELECTED_STAGE = stg;

                // Mask out d-pad bits in g_processed_input — pb_queue's
                // leaked LRUD bits would otherwise drift the cursor away
                // from our pin via WrapPositionToStageBounds before
                // game_state_manager computes grid_pos.
                processed[0] &= ~0xFu;
                processed[1] &= ~0xFu;

                // Pin cursor positions.
                p1_cur[0] = static_cast<int>(p1c % sw);
                p1_cur[1] = static_cast<int>(p1c / sw);
                p2_cur[0] = static_cast<int>(p2c % sw);
                p2_cur[1] = static_cast<int>(p2c / sw);

                // Two-phase per-player drive:
                //
                //  Phase A (selected != target): force selected = -1 so the
                //  natural state-1 d-pad branch sees prev_pos==-1 + new grid
                //  pos == target_idx → triggers player_data_file_loader to
                //  load the right .player file. We MUST NOT inject confirm
                //  here — mode-1's CSS code is if/else: confirm path AND
                //  d-pad/file-load path are mutually exclusive per frame,
                //  and the confirm path is checked first. Without this,
                //  state-0 init's stale selected[player]=0 (cursor was at
                //  (0,0)) would lock player to char 0 before we ever loaded
                //  the right file.
                //
                //  Phase B (selected == target): file already loaded by
                //  Phase A; inject confirm bit, game's confirm branch fires,
                //  action_state locks. Stop pinning on next frame
                //  (action_state==1 means d-pad/confirm code is skipped).
                //
                // pb_queue's natural attack-button bits are ALSO masked out
                // during Phase A so they can't accidentally trigger an early
                // confirm with the wrong selected value.
                const uint8_t p1col = g_target_p1_color.load(std::memory_order_relaxed);
                const uint8_t p2col = g_target_p2_color.load(std::memory_order_relaxed);
                const uint32_t p1_bit = ColorBitForSlot(p1col);
                const uint32_t p2_bit = ColorBitForSlot(p2col);

                if (p1_act == 0u) {
                    if (selected[0] != static_cast<int>(p1c)) {
                        selected[0] = -1;
                        in_changes[0] &= ~0x3F0u;
                    } else {
                        in_changes[0] = (in_changes[0] & ~0x3F0u) | p1_bit;
                    }
                }
                if (p2_act == 0u) {
                    if (selected[1] != static_cast<int>(p2c)) {
                        selected[1] = -1;
                        in_changes[1] &= ~0x3F0u;
                    } else {
                        in_changes[1] = (in_changes[1] & ~0x3F0u) | p2_bit;
                    }
                }

                const uint32_t tick = g_pin_tick.fetch_add(1, std::memory_order_relaxed);
                if (tick < 8u || (tick % 30u) == 0u) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "CssAutoConfirm: tick=%u p1=%u/c%u@(%d,%d)[sel=%d,bit=0x%X] "
                        "p2=%u/c%u@(%d,%d)[sel=%d,bit=0x%X] act=%u/%u stage=%u sw=%u",
                        tick,
                        p1c, p1col, p1_cur[0], p1_cur[1], selected[0], p1_bit,
                        p2c, p2col, p2_cur[0], p2_cur[1], selected[1], p2_bit,
                        p1_act, p2_act, stg, sw);
                }
            }
        } else if (game_mode >= 3000u) {
            // Battle entered — disengage automatically.
            CssAutoConfirm_Disengage();
        }
    }

    return g_orig ? g_orig() : 0;
}

}  // namespace

bool CssAutoConfirm_Install() {
    if (MH_CreateHook((void*)ADDR_GAME_STATE_MANAGER,
                      (void*)Hook_GameStateManager,
                      (void**)&g_orig) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: MH_CreateHook(game_state_manager @ 0x%08X) failed",
            (unsigned)ADDR_GAME_STATE_MANAGER);
        return false;
    }
    // Queue only — InitializeHooks flushes all queued hooks with a single
    // MH_ApplyQueued (one thread-freeze for the whole boot path).
    if (MH_QueueEnableHook((void*)ADDR_GAME_STATE_MANAGER) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: MH_QueueEnableHook(game_state_manager) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: queued game_state_manager @ 0x%08X (idle until "
        "OnReplayMatchStart fires)",
        (unsigned)ADDR_GAME_STATE_MANAGER);
    return true;
}

void CssAutoConfirm_OnReplayMatchStart(uint8_t p1_char, uint8_t p1_color,
                                       uint8_t p2_char, uint8_t p2_color,
                                       uint8_t stage_id) {
    g_target_p1_char.store(p1_char, std::memory_order_relaxed);
    g_target_p2_char.store(p2_char, std::memory_order_relaxed);
    g_target_p1_color.store(p1_color, std::memory_order_relaxed);
    g_target_p2_color.store(p2_color, std::memory_order_relaxed);
    g_target_stage_id.store(stage_id, std::memory_order_relaxed);
    g_pin_tick.store(0, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: armed for replay — p1=%u/c%u p2=%u/c%u stage=%u",
        p1_char, p1_color, p2_char, p2_color, stage_id);
}

void CssAutoConfirm_Disengage() {
    if (g_active.exchange(false, std::memory_order_acq_rel)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: disengaged (battle entered or match ended)");
    }
}

#else  // ENGINE_FM95 — separate state machine, separate hand-off

#include "css_autoconfirm.h"

bool CssAutoConfirm_Install()                              { return true; }
void CssAutoConfirm_OnReplayMatchStart(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void CssAutoConfirm_Disengage()                            {}

#endif
