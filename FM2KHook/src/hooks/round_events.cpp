// C3.5 — vs_round_function detour for ROUND_START / ROUND_END emit.
//
// FM2K-only. The function lives at 0x004086A0 in WonderfulWorld_ver_0946 and
// is the central round state machine for vs/story/team modes. Each call
// reads/writes a substate field at obj+0x152 (= 338) on the current
// round-state object (g_object_data_ptr → slot). We snapshot the substate
// pre-call and post-call, detect the two relevant edges, and emit
// SessionEvent ops via the host SpectatorNode_Append* helpers.
//
// IDA hand-off: docs/c3.5_round_events_ida_handoff.md.

#if !defined(ENGINE_FM95)

#include "round_events.h"

#include <SDL3/SDL_log.h>
#include <cstdint>
#include <windows.h>

#include "MinHook.h"

#include "../core/globals.h"           // g_is_rolling_back, g_player_index
#include "../netplay/spectator_node.h" // SpectatorNode_AppendRound{Start,End}

// vs_round_function dispatcher (FM2K)
constexpr uintptr_t ADDR_VS_ROUND_FUNCTION = 0x004086A0;

// g_object_data_ptr — pointer to current round-state slot
constexpr uintptr_t ADDR_G_OBJECT_DATA_PTR = 0x004CFA00;
constexpr ptrdiff_t OFF_ROUND_SUBSTATE     = 0x152;  // 338

// Substate values for edge detection
constexpr int RSS_BATTLE_INIT      = 100;
constexpr int RSS_ROUND_ANNOUNCE   = 101;  // post value after BATTLE_INIT body
constexpr int RSS_ROUND_END_BANNER = 900;

// ROUND_START payload sources
constexpr uintptr_t ADDR_P1_HP_MAX  = 0x004DFC91;
constexpr uintptr_t ADDR_P2_HP_MAX  = 0x004EDCD0;
constexpr uintptr_t ADDR_SCORE_VAL  = 0x00470050;  // (g_score_value + 1) / 100 → seconds

// ROUND_END payload sources
constexpr uintptr_t ADDR_P1_HP             = 0x004DFC85;
constexpr uintptr_t ADDR_P2_HP             = 0x004EDCC4;
constexpr uintptr_t ADDR_P1_RESULT_KIND    = 0x004DFD87;  // 1=win 2=draw 3=loss
constexpr uintptr_t ADDR_P2_RESULT_KIND    = 0x004EDDC6;

typedef char (__cdecl *VsRoundFunc_t)();
static VsRoundFunc_t orig_vs_round_function = nullptr;

// 1-based intra-match round counter. Reset by RoundEvents_OnMatchStart at
// every Netplay_StartBattle so the first ROUND_START of each match emits
// idx=1. NOT derived from result_kind/round_wins because draws bump both
// win counters and break the formula.
static uint8_t s_round_idx_counter = 0;

static int ReadSubstate() {
    void* slot = *(void**)ADDR_G_OBJECT_DATA_PTR;
    if (!slot) return -1;
    return *(int*)((char*)slot + OFF_ROUND_SUBSTATE);
}

static char __cdecl Hook_vs_round_function() {
    const int pre = ReadSubstate();

    char ret = orig_vs_round_function ? orig_vs_round_function() : 0;

    // Only emit from the authoritative host path. Both peers + the spectator's
    // local sim run vs_round_function; if we appended on every node we'd
    // double-broadcast on relay nodes. Daisy-chain replay is handled in the
    // Hop-1 relay branch of HandleSpecData::EVENT_BATCH instead.
    //
    // g_is_rolling_back guards against re-emit during a GekkoNet replay
    // window — the same logical edge would fire multiple times across
    // forward + replay sims and produce duplicate events.
    if (g_player_index != 0 || g_is_rolling_back) {
        return ret;
    }

    const int post = ReadSubstate();

    // ROUND_START — RSS_BATTLE_INIT body just ran, advanced to RSS_ROUND_ANNOUNCE.
    if (pre == RSS_BATTLE_INIT && post == RSS_ROUND_ANNOUNCE) {
        ++s_round_idx_counter;
        const uint16_t p1_hp_max     = (uint16_t)*(uint32_t*)ADDR_P1_HP_MAX;
        const uint16_t p2_hp_max     = (uint16_t)*(uint32_t*)ADDR_P2_HP_MAX;
        const int32_t  score         = *(int32_t*)ADDR_SCORE_VAL;
        const uint16_t timer_seconds = (score >= 0)
            ? (uint16_t)((score + 1) / 100)
            : (uint16_t)0;
        SpectatorNode_AppendRoundStart(
            s_round_idx_counter, p1_hp_max, p2_hp_max, timer_seconds);
    }

    // ROUND_END — substate just transitioned to RSS_ROUND_END_BANNER from
    // any of the win-tail paths. result_kind / HP are already populated.
    if (pre != RSS_ROUND_END_BANNER && post == RSS_ROUND_END_BANNER) {
        const uint32_t r1_kind = *(uint32_t*)ADDR_P1_RESULT_KIND;
        const uint32_t r2_kind = *(uint32_t*)ADDR_P2_RESULT_KIND;
        const uint8_t winner_idx = (r1_kind == 1) ? 0
                                : (r2_kind == 1) ? 1
                                : 2;  // draw / double-KO / unrecognized
        const uint16_t p1_hp = (uint16_t)*(uint32_t*)ADDR_P1_HP;
        const uint16_t p2_hp = (uint16_t)*(uint32_t*)ADDR_P2_HP;
        SpectatorNode_AppendRoundEnd(winner_idx, p1_hp, p2_hp);
    }

    return ret;
}

bool RoundEvents_Install() {
    if (MH_CreateHook((void*)ADDR_VS_ROUND_FUNCTION,
                      (void*)Hook_vs_round_function,
                      (void**)&orig_vs_round_function) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "RoundEvents: MH_CreateHook(vs_round_function @ %p) failed",
            (void*)ADDR_VS_ROUND_FUNCTION);
        return false;
    }
    if (MH_EnableHook((void*)ADDR_VS_ROUND_FUNCTION) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "RoundEvents: MH_EnableHook(vs_round_function) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RoundEvents: hooked vs_round_function @ 0x%08X for ROUND_START/END emit",
        (unsigned)ADDR_VS_ROUND_FUNCTION);
    return true;
}

void RoundEvents_OnMatchStart() {
    s_round_idx_counter = 0;
}

#else  // ENGINE_FM95

#include "round_events.h"

bool RoundEvents_Install()      { return true; }  // FM95 emit out of scope (separate hand-off)
void RoundEvents_OnMatchStart() {}

#endif
