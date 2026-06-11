// Per-game runtime patches — see header for what lives here.

#if !defined(ENGINE_FM95)

#include "per_game_patches.h"

#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include "MinHook.h"
#include <safetyhook.hpp>  // SafetyHook MidHook for KOF HP-init patch
#include "round_events.h"  // RoundEvents_KofSnapshot* accessors
#include "combat_state.h"  // combat_state::ShouldGuardP2 for state-driven Guard

namespace {

// =============================================================================
// DAMAGE MULTIPLIER
// =============================================================================
//
// health_damage_manager @ 0x40e7c0 — `int __cdecl(int char_data, int damage)`.
// `damage < 0` subtracts from HP; `damage > 0` heals (adds to HP).
//
// Six call sites total, only TWO of which are actual hit-damage:
//   (1) hit_detection_system @ 0x40f6de — opponent takes damage (signed neg)
//   (2) hit_detection_system @ 0x40f7f9 — opponent takes damage (signed neg)
//   (3) health_damage_manager self-recursion @ 0x40e978 — stage trigger
//   (4) health_damage_manager self-recursion @ 0x40e98e — stage trigger
//   (5) character_state_machine OP_CHECK_MOTION @ 0x41323a — script-side
//   (6) character_state_machine OP_CHECK_MOTION @ 0x413298 — script-side
//
// Sites 3-6 pass tightly-balanced script-encoded values where a multiplier
// breaks intent (e.g. self-damage paired with mirror damage to opponent —
// scaling only the self-side desyncs the scripted exchange). v0.2.40 applied
// the multiplier to ALL calls and broke Vanpri's stage-trigger scripts as a
// result.
//
// Fix: stack-walk the return address and only scale when the caller is
// inside hit_detection_system. Direct hit damage gets multiplied; script-
// side damage stays vanilla.

constexpr uintptr_t ADDR_HEALTH_DAMAGE_MANAGER = 0x0040E7C0;
constexpr uintptr_t ADDR_HIT_DETECTION_LO      = 0x0040F010;  // hit_detection_system entry
constexpr uintptr_t ADDR_HIT_DETECTION_HI      = 0x0040F90D;  // entry + size (0x8FD)

typedef int (__cdecl *HealthDmgFn)(int, int);
HealthDmgFn g_orig_health_damage_manager = nullptr;

// 100 = default (no scaling). 50 = halved damage. 200 = doubled. Range
// [1, 1000] enforced by the launcher UI.
std::atomic<int> g_damage_mult_pct{100};

int __cdecl Hook_health_damage_manager(int char_data, int damage) {
    const int mult = g_damage_mult_pct.load(std::memory_order_relaxed);

    // Stack-walk to find the caller. GCC builtin returns the PC of the
    // call site (caller's instruction after the `call`); we range-check
    // it against hit_detection_system's bounds to gate the multiplier.
    // MinGW doesn't ship MSVC's _ReturnAddress(), so use the builtin.
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const bool from_hit = (caller >= ADDR_HIT_DETECTION_LO &&
                           caller <  ADDR_HIT_DETECTION_HI);

    if (from_hit && mult != 100 && damage < 0) {
        // Scale magnitude. 64-bit intermediate so a rare (-30000 * 1000)
        // doesn't wrap. Floor at -1 so partial-damage rounds don't drop
        // to zero and break combo / chip / scaling logic.
        const int64_t scaled =
            (static_cast<int64_t>(damage) * static_cast<int64_t>(mult)) / 100;
        int s32 = static_cast<int>(scaled);
        if (s32 == 0) s32 = -1;
        damage = s32;
    }
    return g_orig_health_damage_manager
        ? g_orig_health_damage_manager(char_data, damage)
        : 0;
}

bool g_damage_hook_installed = false;

// =============================================================================
// OPTION-A KOF RETENTION — character_state_machine HP-init mid-hook
// =============================================================================
//
// The substate-edge KOF apply (in round_events.cpp) wrote winner's HP to
// g_charslot{0,1}_hp at 100→110 — AFTER the engine had already written
// max_hp via character_state_machine's CSMK_PLAYER init at 0x411CB1. The
// engine's brief "HP=max_hp then snap to retained value" caused a visual
// jitter between rounds.
//
// Option A intercepts the engine's HP-write instruction itself:
//   0x411CB1: 89 83 05 DF 00 00    mov [ebx+0xDF05], eax
//
// We use SafetyHook's MidHook which handles ALL register/flag/XMM
// preservation around our handler — no manual trampoline, no ECX/EDX
// clobber footgun. Inside the handler we read ctx.ebx (char_data) and
// modify ctx.eax to substitute the snapshotted HP; the original
// `mov [ebx+0xDF05], eax` then runs (via SafetyHook's trampoline) with
// our chosen value. Side-writes (meter) are direct memory writes.

constexpr uintptr_t KOF_PATCH_SITE       = 0x00411CB1;
constexpr uintptr_t CHAR_DATA_BASE_P1    = 0x004D1D80;  // g_character_data_base[0]
constexpr uintptr_t CHAR_DATA_BASE_P2    = 0x004DFDBF;  // g_character_data_base[1]
constexpr uintptr_t OFF_HP               = 0x0000DF05;  // char_data->hp
constexpr uintptr_t OFF_SUPER_METER      = 0x0000DF5D;  // char_data->super_meter
constexpr uintptr_t ADDR_GAME_MODE_FLAG  = 0x00470058;  // 0=story 1=VS 2=team

// MidHook lifetime owner. Static so it persists for process lifetime;
// destroying it would un-patch the binary. Empty until install.
SafetyHookMid g_kof_mid_hook{};

// =============================================================================
// STORY-INIT AI HIJACK — char_state_machine g_game_mode_flag read override
// =============================================================================
//
// For the 1P-entry → "1P VS CPU" / "Training" submodes, we hijack
// g_game_mode_flag = 1 during CSS so the engine dispatches the 2-cursor
// VS character-select (P1 picks both characters via the CSS pipe). But
// once battle starts, character_state_machine's CSMK_PLAYER branch reads
// the same flag to decide between story-mode P2 init (with stage-script
// AI fields: something_xor_mask, unknown_dfbb[80] = -1, hitstop, etc.)
// and VS-mode P2 init (no AI markers). With flag=1, P2 gets the VS
// init → no AI fields → P2 just stands there with zero input.
//
// Fix: MidHook the dispatch site so character_state_machine sees flag=0
// while every other consumer (vs_round_function for round flow, etc.)
// still sees the global at 1.
//
// Site: 0x411C8F `cmp eax, ebp` — the compare right AFTER `mov eax,
// g_game_mode_flag` @ 0x411C8A. At this instruction eax already holds
// the loaded flag, the upcoming cmp sets flags, and the jz at 0x411C94
// dispatches: ZF=1 → story init @ 0x411D0A (slot-aware: P2 gets AI
// init, P1 gets player init); ZF=0 → VS/team init path.
//
// Hooking the cmp (not the preceding mov) means we run AFTER eax holds
// the real flag; overriding ctx.eax=0 here makes the cmp set ZF=1 so
// jz takes the story branch. SafetyHook relocates `cmp eax, ebp` +
// `mov [esi+4], ecx` (5 bytes) into the trampoline; flags from cmp
// flow through the mov (no flag-touching) into the jz at 0x411C94.
//
// The story branch ALSO covers per-frame AI tick (gated by [esi+0x156]
// inside the branch — zero means init, non-zero means tick). So once
// init has fired, subsequent character_state_machine calls each frame
// take the AI tick path — exactly 1P arcade's per-character behavior.
//
// Caveat: for CPU vs CPU submode this only AI's P2 (slot != 0). The
// story init's slot-0 branch sets up "player char" fields, not AI.
// Making both sides CPU would need an additional intervention; out of
// scope for this hijack.

constexpr uintptr_t STORY_INIT_HIJACK_SITE = 0x00411C8F;  // cmp eax, ebp
constexpr uintptr_t ADDR_GAME_MODE         = 0x00470054;  // current game_mode

SafetyHookMid g_story_init_mid_hook{};

// MidHook handler — invoked BEFORE the original instruction runs.
// SafetyHook saves & restores all registers + flags + XMM around us.
//
// On entry:
//   ctx.ebx = char_data ptr (engine's slot pointer)
//   ctx.eax = max_hp value (just-read at 0x411CA5 from props.life_gauge_max)
//
// We substitute ctx.eax for the winner's slot so the original
// `mov [ebx+0xDF05], eax` ends up writing our snapshotted value.
void OnKofHpInit(SafetyHookContext& ctx) {
    // Pass-through fast path: KOF off, not team mode, or no snapshot.
    if (!RoundEvents_KofRetentionEnabled() ||
        *(const uint32_t*)ADDR_GAME_MODE_FLAG != 2u ||
        !RoundEvents_KofSnapshotPending()) {
        return;
    }

    // Determine which slot this char_data belongs to. Only the two
    // active P1/P2 slots are relevant; team backing slots live elsewhere.
    int slot;
    if      (ctx.ebx == CHAR_DATA_BASE_P1) slot = 0;
    else if (ctx.ebx == CHAR_DATA_BASE_P2) slot = 1;
    else return;

    if (slot != RoundEvents_KofSnapshotWinnerIdx()) return;

    // Winner: substitute snapshotted HP into ctx.eax (engine writes it
    // instead of max_hp), and side-write meter directly. Mark snapshot
    // applied so duplicate fires (rollback resim) become no-ops.
    const uint32_t snap_hp    = RoundEvents_KofSnapshotWinnerHp();
    const uint32_t snap_meter = RoundEvents_KofSnapshotWinnerMeter();
    const uint32_t orig_eax   = ctx.eax;
    ctx.eax = snap_hp;
    *(uint32_t*)(ctx.ebx + OFF_SUPER_METER) = snap_meter;
    RoundEvents_KofSnapshotMarkApplied();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[KOF-RETAIN-A] HpInit: winner=P%d intercepted "
        "(engine wanted hp=%u, substituted snap hp=%u meter=%u)",
        slot + 1, orig_eax, snap_hp, snap_meter);
}

}  // namespace

bool PerGamePatches_InstallDamageMultiplierHook() {
    // Read env at install time. If absent or default 100, skip the hook
    // entirely so we don't pay the trampoline cost on every damage event
    // for the 99% of users who don't change this.
    const char* e = std::getenv("FM2K_DAMAGE_MULT_PCT");
    if (!e || !*e) return true;
    int pct = std::atoi(e);
    if (pct < 1)    pct = 1;
    if (pct > 1000) pct = 1000;
    if (pct == 100) return true;

    g_damage_mult_pct.store(pct, std::memory_order_relaxed);

    if (MH_CreateHook((void*)ADDR_HEALTH_DAMAGE_MANAGER,
                      (void*)Hook_health_damage_manager,
                      (void**)&g_orig_health_damage_manager) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_CreateHook(health_damage_manager) failed");
        return false;
    }
    if (MH_QueueEnableHook((void*)ADDR_HEALTH_DAMAGE_MANAGER) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_QueueEnableHook(health_damage_manager) failed");
        return false;
    }
    g_damage_hook_installed = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: damage multiplier hook installed (%d%%)", pct);
    return true;
}

bool PerGamePatches_InstallKofHpInitPatch() {
    // Skip if KOF retention is disabled at install time. Avoids touching
    // the binary at all when the user isn't using the feature — toggle
    // off + restart leaves vanilla code intact at 0x411CB1.
    const char* e = std::getenv("FM2K_TEAM_KOF_RETENTION");
    if (!e || std::strcmp(e, "1") != 0) return true;

    // SafetyHook MidHook handles ALL register/flag/XMM preservation,
    // trampoline allocation near the patch site, and instruction
    // relocation. Replaces the manual trampoline + push/pop ECX/EDX
    // dance that bit us in v0.2.42 (ECX clobber → corrupted
    // player_slot_id_mirror → hits passed through opponents).
    g_kof_mid_hook = safetyhook::create_mid((void*)KOF_PATCH_SITE, OnKofHpInit);
    if (!g_kof_mid_hook) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: safetyhook::create_mid(KOF HP-init) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: KOF Option-A SafetyHook MidHook installed @ 0x%X",
        (unsigned)KOF_PATCH_SITE);
    return true;
}

// =============================================================================
// MODE TOGGLES (VS CPU / CPU-vs-CPU / Training / OPTION)
// =============================================================================
//
// FM2K's "AI" is script-driven: every character's action_table contains
// scripts that branch via OP_CHECK_MOTION (passes if matching motion in
// input history), OP_RANDOM (passes randomly), OP_LIFE_GAUGE (HP
// threshold), OP_GAUGE_BRANCH (meter threshold), etc. For a HUMAN
// player, OP_CHECK_MOTION matches when the player has performed a
// motion. For a "CPU" player, we feed ZERO input — OP_CHECK_MOTION
// can never match (no inputs to scan), so the script falls through to
// OP_RANDOM / OP_LIFE_GAUGE branches, which is the engine's natural AI
// path.
//
// So vs_cpu / cpu_vs_cpu reduces to "return 0 from Hook_GetPlayerInput
// for the AI side(s) during battle". CSS gets P1 input piped to P2 so
// P1 picks both characters.

namespace {
std::atomic<bool> g_vs_cpu_mode{false};
std::atomic<bool> g_cpu_vs_cpu_mode{false};
std::atomic<bool> g_training_mode{false};
std::atomic<bool> g_option_mode_selector{false};

// Training-mode P2 behavior. Cycled by PerGamePatches_CycleTrainingP2Behavior
// (called from F2 hotkey handler + ImGui overlay button).
//   0 = player (no override)
//   1 = CPU   (zero input)
//   2 = imitate (mirror P1)
//   3 = guard (back direction)
//   4 = jump-up
std::atomic<int> g_training_p2_behavior{1};

// OPTION-button cycle state. 0=VS 2P, 1=VS CPU, 2=CPU vs CPU, 3=Training.
std::atomic<int>  g_vs_submode{0};
std::atomic<bool> g_option_was_pressed{false};   // rising-edge tracker
std::atomic<bool> g_f2_was_pressed{false};       // training cycle hotkey
std::atomic<uint32_t> g_prev_game_mode{0};       // for title→CSS edge detection

// Team size override stored at hook init. 0 = no override (engine default).
// Read each frame by css_autoconfirm.cpp's game_state_manager detour and
// re-applied to g_team_round_setting AFTER the engine's natural read.
std::atomic<int>  g_team_size_override{0};
}  // namespace

// Forward decl — original_get_player_input is the MinHook trampoline,
// declared in core/globals.h. We use it from the imitate path so we can
// READ P1's current input even when called for P2.
extern "C" {
typedef int (__cdecl *GetPlayerInputFunc)(int, int);
extern GetPlayerInputFunc original_get_player_input;
}

int PerGamePatches_TryOverrideInput(int player_id, uint32_t game_mode) {
    const bool battle = (game_mode >= 3000u && game_mode < 4000u);
    const bool css    = (game_mode == 2000u);

    // CSS takeover: when any "single-player driver" mode is active, route
    // P1's input to P2's cursor during CSS — but ONLY after P1 has
    // confirmed their own character. Before P1 confirms, P2 cursor stays
    // put (gated to 0). After P1 confirms + releases attack, the pipe
    // engages so P1 navigates P2's cursor and confirms the dummy.
    const bool any_solo_driver_mode =
        g_vs_cpu_mode.load(std::memory_order_relaxed) ||
        g_cpu_vs_cpu_mode.load(std::memory_order_relaxed) ||
        g_training_mode.load(std::memory_order_relaxed);
    // Fast-out before any input read. With no solo-driver mode engaged
    // (the default for normal play) there's nothing to override -- and we
    // must NOT pay for the extra original_get_player_input(0,0) DirectInput
    // poll the battle branch below would otherwise do EVERY frame. That
    // redundant per-frame poll is the 0.2.46 fps regression (#63); its cost
    // scales with the machine's HID/driver stack, which is why it dropped
    // some users to ~95fps and not others.
    if (!any_solo_driver_mode) return -1;
    if (css && player_id == 1 && original_get_player_input) {
        const uint16_t p1_input =
            (uint16_t)(original_get_player_input(0, 0) & 0x7FFu);
        return (int)PerGamePatches_GatedP2CssInput(p1_input);
    }

    // Battle overrides delegated to the shared helper so the binder
    // path can call it too (binder returns from Hook_GetPlayerInput
    // before TryOverrideInput, so any-battle override has to live in
    // a helper both paths can invoke).
    if (battle) {
        const uint16_t p1_input = original_get_player_input
            ? (uint16_t)(original_get_player_input(0, 0) & 0x7FFu)
            : 0;
        const int over = PerGamePatches_BattleInputOverride(player_id, p1_input);
        if (over >= 0) return over;
    }

    return -1;  // no override
}

int PerGamePatches_BattleInputOverride(int player_id, uint16_t p1_input) {
    // CPU vs CPU: zero both. Engine's script-driven AI takes over via
    // OP_RANDOM / OP_LIFE_GAUGE / OP_GAUGE_BRANCH branches in each
    // character's action_table.
    if (g_cpu_vs_cpu_mode.load(std::memory_order_relaxed)) {
        return 0;
    }
    // VS CPU: P2 zero input → AI scripts fire.
    if (g_vs_cpu_mode.load(std::memory_order_relaxed) && player_id == 1) {
        return 0;
    }
    // Training: per-behavior override on P2 only.
    if (g_training_mode.load(std::memory_order_relaxed) && player_id == 1) {
        const int beh = g_training_p2_behavior.load(std::memory_order_relaxed);
        switch (beh) {
            case 0: return -1;     // player — no override
            case 1: return 0;      // CPU — zero input
            case 2: return p1_input;  // imitate — mirror P1
            case 3: {
                // Guard — state-driven with 12-frame latch, side-aware
                // back-direction, crouch-block default, and P1 attack-
                // press anticipation. See combat_state::GuardP2Input for
                // the full predicate. Key changes from earlier iterations:
                //   * Returns DOWN+back (crouch-block) instead of just
                //     back — covers LOW + MID attacks via the engine's
                //     crouching-block path (char_flags & 8 at +0x7CB6)
                //   * Triggers on P1's attack-button press in addition
                //     to state signals — closes the 1-frame state-
                //     detection delay since the button press leads
                //     active hitbox by 3+ frames on typical attacks
                //   * Picks 0x001 (LEFT) vs 0x002 (RIGHT) per P1/P2
                //     positions so cross-ups work
                return (int)combat_state::GuardP2Input(p1_input);
            }
            case 4: return 0x004;  // jump-up (FM2K bit 2 = UP; bypassed in
                                   // practice — engine's ai_input_processor
                                   // case 4 handles Jump via ai_mode=4)
            default: break;
        }
    }
    return -1;
}

uint16_t PerGamePatches_GatedP2CssInput(uint16_t p1_input) {
    // Phase machine driven by P1's action_state and the held-attack
    // tracker. Resets to phase 0 whenever we're not in CSS.
    //
    //   Phase 0 (P1 hasn't confirmed): return 0, P2 idle
    //   Phase 1 (P1 confirmed, attack still held from confirm press):
    //          return 0, waiting for attack release so the same press
    //          doesn't instantly fire P2's confirm
    //   Phase 2 (P1 confirmed AND attack released at least once):
    //          return p1_input — full pipe, P1 drives P2 cursor
    static std::atomic<int> s_phase{0};

    const uint32_t game_mode = *(const uint32_t*)0x00470054;
    if (game_mode != 2000u) {
        s_phase.store(0, std::memory_order_relaxed);
        return 0;
    }

    const uint32_t p1_action = *(const uint32_t*)0x0047019C;  // g_p1_action_state
    int phase = s_phase.load(std::memory_order_relaxed);

    if (phase == 0) {
        if (p1_action != 0u) {
            phase = 1;
            s_phase.store(phase, std::memory_order_relaxed);
        } else {
            return 0;
        }
    }
    if (phase == 1) {
        // Wait for P1's attack-confirm press to release before piping.
        if ((p1_input & 0x3F0u) == 0u) {
            phase = 2;
            s_phase.store(phase, std::memory_order_relaxed);
        } else {
            return 0;
        }
    }
    // Phase 2: full pipe.
    return p1_input;
}

void PerGamePatches_CycleTrainingP2Behavior() {
    int next = (g_training_p2_behavior.load(std::memory_order_relaxed) + 1) % 5;
    g_training_p2_behavior.store(next, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: training P2 behavior → %d (%s)",
        next, PerGamePatches_TrainingP2BehaviorLabel(next));
}

int PerGamePatches_GetTrainingP2Behavior() {
    return g_training_p2_behavior.load(std::memory_order_relaxed);
}

const char* PerGamePatches_TrainingP2BehaviorLabel(int behavior) {
    switch (behavior) {
        case 0: return "Player";
        case 1: return "CPU";
        case 2: return "Imitate P1";
        case 3: return "Guard";
        case 4: return "Jump up";
        default: return "?";
    }
}

void PerGamePatches_OnOptionPressed() {
    // 3-slot cycle (0=Default, 1=alt-1, 2=alt-2). Label is context-aware
    // and resolved against the current title menu cursor at log time.
    int next = (g_vs_submode.load(std::memory_order_relaxed) + 1) % 3;
    g_vs_submode.store(next, std::memory_order_relaxed);
    const int ctx = PerGamePatches_GetVsMenuContext();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: VS submode cycled → %d (%s)  [menu_ctx=%s]",
        next, PerGamePatches_VsSubmodeLabel(next, ctx),
        ctx == 0 ? "1P/Story" : "VS");
}

int PerGamePatches_GetVsSubmode() {
    return g_vs_submode.load(std::memory_order_relaxed);
}

int PerGamePatches_GetVsMenuContext() {
    // Read g_menu_selection @ 0x424780. 0 = 1P/Story entry, 1 = VS entry.
    // Other values fall back to "VS" so a non-standard menu layout still
    // shows something reasonable in the badge.
    const uint32_t sel = *(const uint32_t*)0x00424780;
    return (sel == 0u) ? 0 : 1;
}

const char* PerGamePatches_VsSubmodeLabel(int submode, int menu_context) {
    // Cycle is per-entry. Slot 0 = vanilla / no override; slots 1-2 =
    // entry-specific alternates. See OnFrameTick title→CSS apply for the
    // matching mode-flag writes.
    if (menu_context == 0) {  // 1P / Story entry
        switch (submode) {
            case 0: return "1P Arcade (Default)";
            case 1: return "1P VS CPU";
            case 2: return "Training";
        }
    } else {  // VS entry (any non-zero menu selection)
        switch (submode) {
            case 0: return "VS 2P (Default)";
            case 1: return "Team Versus";
            case 2: return "CPU vs CPU";
        }
    }
    return "?";
}

bool PerGamePatches_IsOptionModeSelectorActive() {
    return g_option_mode_selector.load(std::memory_order_relaxed);
}

bool PerGamePatches_IsTrainingModeActive() {
    return g_training_mode.load(std::memory_order_relaxed);
}

bool PerGamePatches_IsVsCpuModeActive() {
    return g_vs_cpu_mode.load(std::memory_order_relaxed);
}

bool PerGamePatches_IsCpuVsCpuModeActive() {
    return g_cpu_vs_cpu_mode.load(std::memory_order_relaxed);
}

int PerGamePatches_GetTeamSizeOverride() {
    return g_team_size_override.load(std::memory_order_relaxed);
}

void PerGamePatches_OnTitleInputTick(uint16_t raw_input, uint32_t game_mode) {
    if (!g_option_mode_selector.load(std::memory_order_relaxed)) return;
    if (game_mode != 1000u) {
        // Reset rising-edge tracker when leaving title.
        g_option_was_pressed.store(false, std::memory_order_relaxed);
        return;
    }
    // OPTION = bit 11 (0x800) — dedicated meta-button, separate from
    // START (0x400) so pressing START to confirm a title menu entry
    // doesn't also cycle the submode. Default-bound to Tab via the
    // input binder; user can rebind.
    const bool now = (raw_input & 0x800u) != 0u;
    const bool was = g_option_was_pressed.load(std::memory_order_relaxed);
    g_option_was_pressed.store(now, std::memory_order_relaxed);
    if (now && !was) {
        PerGamePatches_OnOptionPressed();
    }
}

void PerGamePatches_OnFrameTick() {
    // ---- F2 hotkey: cycle training-mode P2 behavior ----
    // Only meaningful when training_mode is active. Rising-edge so a
    // held key doesn't spin through behaviors. GetAsyncKeyState is
    // safe to call without window focus — the hotkey works even if the
    // launcher is in the foreground (the user is actively configuring),
    // which we want.
    if (g_training_mode.load(std::memory_order_relaxed)) {
        const bool now = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        const bool was = g_f2_was_pressed.load(std::memory_order_relaxed);
        g_f2_was_pressed.store(now, std::memory_order_relaxed);
        if (now && !was) {
            PerGamePatches_CycleTrainingP2Behavior();
        }
    }

    // ---- CSS→title reset only ----
    // The title→CSS apply lives in OnGameStateManagerEntry below (it
    // needs to run on the logic frame BEFORE CSS STATE 0; this
    // per-render-frame tick fires after, which crashed 1P-to-2P hijack
    // because STATE 0 had already created one cursor object expecting
    // 1P flow). Returning to title still clears flags here — that path
    // doesn't have a strict ordering constraint.
    const uint32_t mode = *(const uint32_t*)0x00470054;  // FM2K::ADDR_GAME_MODE
    const uint32_t prev = g_prev_game_mode.exchange(mode, std::memory_order_relaxed);

    if (g_option_mode_selector.load(std::memory_order_relaxed)) {
        if (prev != 1000u && mode == 1000u) {
            g_vs_cpu_mode.store(false, std::memory_order_relaxed);
            g_cpu_vs_cpu_mode.store(false, std::memory_order_relaxed);
            g_training_mode.store(false, std::memory_order_relaxed);
        }
    }
}

void PerGamePatches_OnGameStateManagerEntry() {
    // Called from css_autoconfirm.cpp's Hook_GameStateManager BEFORE the
    // original body runs. Applies the OPTION-cycle submode override
    // (game_mode_flag + mode-driver flags) ONLY when STATE 0 of CSS is
    // about to dispatch — that's the exact moment the engine reads
    // g_game_mode_flag to decide single-cursor vs 2-cursor init.
    //
    // STATE 0 dispatch detection via substate (obj->unknown_152, offset
    // 0x152 on the round-state object pointed to by g_object_data_ptr):
    // substate==0 on entry means STATE 0 is about to run. After STATE 0
    // it transitions to substate=1 and the engine stops re-running init.
    //
    // Prior latch-based approach was wrong: GSM is the tick function for
    // the CSS object created by title on confirm — the FIRST GSM call
    // already runs STATE 0 with the engine's original g_game_mode_flag,
    // and STATE 0 itself sets g_game_mode = 2000 near its end. So
    // "first call at mode==2000" fires the frame AFTER STATE 0, too
    // late. Substate-based detection fires at the right instant.
    if (!g_option_mode_selector.load(std::memory_order_relaxed)) return;

    const uint32_t mode = *(const uint32_t*)0x00470054;
    if (mode != 2000u && mode != 1000u) return;  // only title/CSS phase

    // Read substate from g_object_data_ptr->unknown_152. The CSS object's
    // substate starts at 0 when title spawns it; STATE 0 init body runs
    // when substate is observed as 0 and then transitions it to 1.
    void* obj = *(void**)0x004CFA00;  // g_object_data_ptr
    if (!obj) return;
    const int substate = *(const int*)((const char*)obj + 0x152);
    if (substate != 0) return;  // STATE 0 already ran for this CSS visit

    const int sub = g_vs_submode.load(std::memory_order_relaxed);
    const int ctx = PerGamePatches_GetVsMenuContext();
    // Clear all submode-driven flags first so re-cycling works.
    g_vs_cpu_mode.store(false, std::memory_order_relaxed);
    g_cpu_vs_cpu_mode.store(false, std::memory_order_relaxed);
    g_training_mode.store(false, std::memory_order_relaxed);

    volatile uint32_t* gmf = (uint32_t*)0x00470058;
    if (ctx == 0) {  // 1P / Story entry
        switch (sub) {
            case 0: break;  // 1P Arcade — vanilla flow (no override)
            case 1:         // 1P VS CPU — hijack to 2P CSS
                *gmf = 1u;
                g_vs_cpu_mode.store(true, std::memory_order_relaxed);
                break;
            case 2:         // Training — hijack to 2P CSS
                *gmf = 1u;
                g_training_mode.store(true, std::memory_order_relaxed);
                break;
        }
    } else {  // VS entry
        switch (sub) {
            case 0: break;  // 2P VS — vanilla flow
            case 1:         // Team Versus
                *gmf = 2u;
                break;
            case 2:         // CPU vs CPU — stay in VS (flag=1) + both AI
                g_cpu_vs_cpu_mode.store(true, std::memory_order_relaxed);
                break;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: GSM title→CSS edge — ctx=%s sub=%d (%s) → "
        "game_mode_flag=%u vs_cpu=%d cpu_vs_cpu=%d training=%d",
        ctx == 0 ? "1P/Story" : "VS", sub,
        PerGamePatches_VsSubmodeLabel(sub, ctx),
        (unsigned)*gmf,
        (int)g_vs_cpu_mode.load(std::memory_order_relaxed),
        (int)g_cpu_vs_cpu_mode.load(std::memory_order_relaxed),
        (int)g_training_mode.load(std::memory_order_relaxed));

    // (Title-confirm attack leak no longer needs a seed/clear — the
    // GatedP2CssInput phase machine returns 0 until P1 has confirmed
    // AND released attack, so the carried-over press never reaches
    // P2's input as a rising edge.)
}

// =============================================================================
// AI FIELD WRITES — replicate vs_round_function's story-mode RSS_BATTLE_INIT
// =============================================================================
//
// `ai_input_processor` (0x411270) is the engine's per-frame AI driver, called
// from `character_action_controller` for every fighter object every frame. It
// dispatches based on three char_data fields:
//
//   [slot + 0xDF5D]  AI gate — must be non-zero or the whole function bails.
//                    Story init writes 1; meter logic may also write here.
//   [slot + 0xDF61]  AI difficulty (clamped 0..100). Controls how aggressively
//                    the AI picks moves vs. waits. Story init reads it from
//                    `stage_fighter_entry[-2]` per the stage script.
//   [slot + 0xDF65]  AI mode (switch index, dec then 0..3):
//                       1 = full script-driven AI (the normal CPU opponent)
//                       2 = mirror P1's input
//                       4 = force-up input (jump constantly)
//                       other = no-op (chars act on real player input)
//
// `vs_round_function`'s story-mode RSS_BATTLE_INIT branch (flag==0) sets all
// three for every spawned fighter via the loop at 0x408B72:
//     mov dword ptr [esi-4], 1     ; gate enable
//     mov [esi], <stage difficulty>
//     mov dword ptr [esi+4], 1     ; AI mode = full script
//
// The VS-mode branch (flag==1) and team-mode branch (flag==2) skip those
// writes — they only spawn the char objects. Our story-init MidHook at
// 0x411C8F flips `character_state_machine`'s flag-read view to 0, but
// vs_round_function still sees flag=1 (we don't override there because the
// VS round flow — single fight, no stage progression — is what we WANT), so
// the AI-field loop never runs and ai_input_processor's gate stays at 0.
//
// Patch: at the RSS_BATTLE_INIT → RSS_ROUND_ANNOUNCE_WAIT (100 → 110)
// substate transition, write the three fields directly for the AI-driven
// slot(s). One-shot per round entry; the engine's natural HP/meter resets
// between rounds don't touch these offsets so they persist. round_events.
// cpp's Hook_vs_round_function calls PerGamePatches_OnBattleInitComplete
// after orig runs when it detects pre==100 && post!=100.

constexpr uintptr_t CHAR_SLOT0_BASE       = 0x004D1D80;
constexpr uintptr_t CHAR_SLOT_STRIDE      = 0xE03F;     // 57407 bytes per slot
constexpr uintptr_t OFF_AI_GATE           = 0x0000DF5D;
constexpr uintptr_t OFF_AI_DIFFICULTY     = 0x0000DF61;
constexpr uintptr_t OFF_AI_MODE           = 0x0000DF65;
constexpr uint32_t  AI_DEFAULT_DIFFICULTY = 50;  // mid; story uses stage data

void ApplyAiFieldsForSlot(int slot, uint32_t difficulty) {
    const uintptr_t base = CHAR_SLOT0_BASE + (uintptr_t)slot * CHAR_SLOT_STRIDE;
    *(volatile uint32_t*)(base + OFF_AI_GATE)       = 1;
    *(volatile uint32_t*)(base + OFF_AI_DIFFICULTY) = difficulty;
    *(volatile uint32_t*)(base + OFF_AI_MODE)       = 1;
}

// Story-init AI hijack handler — see big comment near the MidHook decl
// (top of file). Fires AFTER `mov eax, g_game_mode_flag` so ctx.eax
// already holds the loaded flag; we override to 0 when a hijack mode
// is active and we're in battle, which makes the trampoline's cmp set
// ZF and the upcoming jz take the story-init / 1P-arcade-AI branch.
void OnCharStateMachineFlagDispatch(SafetyHookContext& ctx) {
    const bool any_hijack =
        g_vs_cpu_mode.load(std::memory_order_relaxed) ||
        g_training_mode.load(std::memory_order_relaxed) ||
        g_cpu_vs_cpu_mode.load(std::memory_order_relaxed);
    if (!any_hijack) return;

    // Only override during battle. CSS (game_mode==2000) still needs
    // flag=1 visible to the cursor-creation dispatch so 2P CSS appears;
    // we already wrote flag=1 to the global at title→CSS edge.
    const uint32_t mode = *(const uint32_t*)ADDR_GAME_MODE;
    if (mode < 3000u || mode >= 4000u) return;

    ctx.eax = 0;
}

bool PerGamePatches_InstallStoryInitHijack() {
    // DISABLED — was counterproductive in practice. Story-init branch in
    // character_state_machine reads `g_stage_script_index[g_css_active_-
    // player]` to derive char_data->something_xor_mask (0xDFB7), which
    // character_facing_controller uses as the opponent bitmask. In our
    // hijacked flow that index is 0/stale, so the resulting xor_mask
    // didn't include P1's slot bit → character_facing_controller failed
    // to find an opponent → P2 faced default direction and AI scripts
    // ran without an opponent reference.
    //
    // Without this MidHook the engine takes the VS-init branch, which
    // writes the correct xor_mask = -1 - (1 << slot_id). The AI fields
    // we WANT are now written directly by ApplyAiFieldsForSlot — gate,
    // difficulty, and mode — invoked every battle frame from round_-
    // events.cpp's Hook_vs_round_function. This separates "tell the
    // engine to run AI for this slot" (direct field writes) from "let
    // the engine think the round is 1P arcade" (story-init hijack);
    // we only need the former.
    //
    // The handler and constants are kept in case we revisit, but the
    // install is a no-op.
    (void)STORY_INIT_HIJACK_SITE;
    (void)&OnCharStateMachineFlagDispatch;
    return true;
}

// Map a training-mode P2 behavior selector value to the ai_input_processor
// switch case it should drive. The engine has built-in cases for our
// Imitate (case 2 — mirror P1's history into P2's) and Jump (case 4 —
// force the UP bit each frame), so we just point ai_mode at them; for
// Player and Guard we keep ai_mode=0 so the input pipeline runs raw
// (and Guard's "force back" is applied by PerGamePatches_BattleInputOverride).
static uint32_t TrainingBehaviorToAiMode(int behavior) {
    switch (behavior) {
        case 0: return 0;  // Player — raw input, no engine override
        case 1: return 1;  // CPU — full script AI
        case 2: return 2;  // Imitate — engine mirrors P1's history to P2
        case 3: return 0;  // Guard — handled by BattleInputOverride (back dir)
        case 4: return 4;  // Jump-up — engine writes UP each frame
        default: return 0;
    }
}

void ApplyAiFieldsForSlotWithMode(int slot, uint32_t difficulty, uint32_t ai_mode) {
    const uintptr_t base = CHAR_SLOT0_BASE + (uintptr_t)slot * CHAR_SLOT_STRIDE;
    *(volatile uint32_t*)(base + OFF_AI_GATE)       = 1;
    *(volatile uint32_t*)(base + OFF_AI_DIFFICULTY) = difficulty;
    *(volatile uint32_t*)(base + OFF_AI_MODE)       = ai_mode;
}

void PerGamePatches_OnBattleInitComplete() {
    // Called every frame from round_events.cpp's Hook_vs_round_function
    // (the round-state object's per-frame tick). The function name is a
    // misnomer — it was originally a one-shot at RSS_BATTLE_INIT → 110
    // but health_damage_manager has a write at 0x40EA63 that clobbers
    // [slot+0xDF65] when the CPU takes damage, and char_state_machine
    // init runs late enough that a one-shot pre-init write gets stomped.
    // Per-frame writes are robust: ~3 dword writes per active AI slot,
    // negligible cost.
    //
    // Slot + ai_mode selection by submode:
    //   vs_cpu_mode      → P2 ai_mode=1 (full script AI)
    //   cpu_vs_cpu_mode  → P1+P2 ai_mode=1 (both AI-driven; P1 still gets
    //                      VS-init "player char" markers but ai_input_-
    //                      processor's case-1 doesn't care about slot id)
    //   training_mode    → P2 ai_mode mapped from g_training_p2_behavior
    //                      via TrainingBehaviorToAiMode; F2 cycles update
    //                      the atomic and the next frame reflects it.
    const bool vs_cpu   = g_vs_cpu_mode.load(std::memory_order_relaxed);
    const bool cpu_cpu  = g_cpu_vs_cpu_mode.load(std::memory_order_relaxed);
    const bool training = g_training_mode.load(std::memory_order_relaxed);

    if (!vs_cpu && !cpu_cpu && !training) return;

    // Determine P2's desired ai_mode. 0 means "don't drive the slot via
    // the engine" — input override handles it (Player/Guard).
    uint32_t p2_ai_mode = 0;
    if (vs_cpu || cpu_cpu) {
        p2_ai_mode = 1;
    } else if (training) {
        const int beh = g_training_p2_behavior.load(std::memory_order_relaxed);
        p2_ai_mode = TrainingBehaviorToAiMode(beh);
    }

    // P1 gets case-1 AI only in cpu_vs_cpu.
    if (cpu_cpu) {
        ApplyAiFieldsForSlotWithMode(0, AI_DEFAULT_DIFFICULTY, 1);
    }

    if (p2_ai_mode != 0) {
        // Engine drives P2: write gate + difficulty + mode every frame.
        ApplyAiFieldsForSlotWithMode(1, AI_DEFAULT_DIFFICULTY, p2_ai_mode);
    } else {
        // Player / Guard: input pipeline + BattleInputOverride drive P2.
        //
        // We MUST clear BOTH the AI gate (0xDF5D) AND the mode (0xDF65) —
        // not just the mode. The gate field doubles as a discriminator in
        // hit_detection_system @ 0x40F3DA:
        //
        //     if (*(target_char_data + 0xDF5D)) {          // AI gate
        //         if (game_rand() % 100 >=
        //               *(target_char_data + 0xDF61))      // AI difficulty
        //             goto LABEL_71;  // hit lands (RNG-fail to block)
        //     }
        //
        // Leaving gate=1 (residual from a prior CPU/Imitate/Jump phase)
        // makes the engine RNG-block at difficulty% instead of running
        // the actual input-based block check. With difficulty=50 that's
        // a 50/50 block rate — exactly the symptom that motivated this
        // patch. Clearing the gate forces the engine to fall through to
        // the section-4 block path (input-driven, deterministic).
        //
        // Trade-off: this clobbers char_data+0xDF5D every frame. If that
        // field is also used as P2's super meter, P2 loses meter
        // accumulation during Player/Guard mode. Per the combat-state
        // analysis (docs/analysis/fm2k_combat_state.md section A.6) the
        // true super meter is at +0xDF55 (`pre_super_meter`); 0xDF5D is
        // the AI-gate dedicated field. Confirmed via vs_round_function
        // story-init writing a literal `1` here, not a meter ramp value.
        const uintptr_t slot1_base = CHAR_SLOT0_BASE + CHAR_SLOT_STRIDE;
        *(volatile uint32_t*)(slot1_base + OFF_AI_GATE) = 0;
        *(volatile uint32_t*)(slot1_base + OFF_AI_MODE) = 0;
    }
}

// =============================================================================
// TEAM SIZE OVERRIDE + ENV VAR INTAKE
// =============================================================================

void PerGamePatches_ApplyRuntime() {
    // ---- Team size override (FM2K_TEAM_SIZE) ----
    // Stash the value as an atomic; css_autoconfirm.cpp's game_state_manager
    // detour writes it to g_team_round_setting (0x470064) every frame
    // AFTER the engine's natural copy from g_team_round (0x430128). We
    // can't write at hook init because hit_judge_set_function runs later
    // in boot and re-loads the INI default into g_team_round, blowing
    // away any direct write we did.
    //
    // Range clamp [2, 4]: the engine's CSS state machine indexes its
    // character-data pool as `4 * player_idx + round_count` (hardcoded
    // *4 stride in game_state_manager @ 0x406fc0), and the pool itself
    // is exactly 8 statically-allocated 57407-byte slots based at
    // g_character_data_base @ 0x4d1d80. P1 fills slots 0..N-1, P2 fills
    // slots 4..4+N-1 — so N>4 collides P1 into P2's slot 4. The
    // g_team_char_objects portrait array @ 0x424e80 is similarly only
    // 8 dwords before bumping into g_team_cursor_array @ 0x424e90.
    if (const char* e = std::getenv("FM2K_TEAM_SIZE"); e && *e) {
        int n = std::atoi(e);
        if (n >= 2 && n <= 4) {
            g_team_size_override.store(n, std::memory_order_relaxed);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: team size override armed → %d "
                "(applied per-frame in game_state_manager detour)", n);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: FM2K_TEAM_SIZE=%s out of range [2,4] — ignored", e);
        }
    }

    // ---- Mode toggles ----
    auto on = [](const char* env_var) -> bool {
        const char* e = std::getenv(env_var);
        return e && std::strcmp(e, "1") == 0;
    };

    g_vs_cpu_mode.store(on("FM2K_VS_CPU_MODE"), std::memory_order_relaxed);
    g_cpu_vs_cpu_mode.store(on("FM2K_CPU_VS_CPU_MODE"), std::memory_order_relaxed);
    g_training_mode.store(on("FM2K_TRAINING_MODE"), std::memory_order_relaxed);
    g_option_mode_selector.store(on("FM2K_OPTION_MODE_SELECTOR"),
                                 std::memory_order_relaxed);

    if (g_vs_cpu_mode.load() || g_cpu_vs_cpu_mode.load() ||
        g_training_mode.load() || g_option_mode_selector.load()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: modes — vs_cpu=%d cpu_vs_cpu=%d training=%d "
            "option_selector=%d",
            (int)g_vs_cpu_mode.load(),
            (int)g_cpu_vs_cpu_mode.load(),
            (int)g_training_mode.load(),
            (int)g_option_mode_selector.load());
    }
}

// =============================================================================
// BOOT-TO-BATTLE PRIME — populate g_iniFile_nameOverride for /F path
// =============================================================================
//
// The engine has a built-in dev shortcut: pass /F on the command line and
// WinMain sets g_debug_mode=3. The slot-0 boot dispatcher (Initialize-
// GameFromCommandLine @ 0x409a60) then skips splash/title/CSS and fires
// `create_game_object(14, 127, 0, 0)` straight into battle.
//
// The catch: with g_debug_mode set, the dispatcher SKIPS the cmdline
// parser and reads the kgt filename from g_iniFile_nameOverride @
// 0x43012c. The FM2K editor populates that global via shared state
// before invoking `KGT2nd_GAME.exe /F`; shipped games don't.
//
// We can't just write the global from DllMain because hit_judge_set_-
// function (called by InitializeMainWindow during boot, AFTER our DllMain
// but BEFORE the slot-0 dispatcher fires) reads the kgt's [File].Filename
// from the editor INI (which doesn't exist for shipped games) and writes
// the default (empty string) into the same global — clobbering our prime.
//
// So we hook the boot dispatcher itself and write the global at entry,
// after the stomp, before the original body reads it. Single hook fires
// exactly once on the first main-loop tick.

constexpr uintptr_t ADDR_INIT_FROM_CMD       = 0x00409A60;
constexpr uintptr_t ADDR_INI_NAME_OVERRIDE   = 0x0043012C;
constexpr size_t    INI_NAME_BUF_SIZE        = 256;

typedef int (__cdecl *InitGameFromCmdFn)();
InitGameFromCmdFn g_orig_init_game_from_cmd = nullptr;

// Derive "<exe_basename>.kgt" from GetModuleFileNameA(NULL) and write it
// to g_iniFile_nameOverride. Returns true on success. Game-agnostic; any
// FM2K title whose .kgt sits next to the .exe (the standard shipped
// layout — WonderfulWorld, vanpri, etc.) works.
bool WriteKgtNameToOverride() {
    char exe_path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (n == 0 || n >= sizeof(exe_path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: WriteKgtName — GetModuleFileNameA failed (%lu)",
            GetLastError());
        return false;
    }

    const char* basename = exe_path;
    for (const char* p = exe_path; *p; ++p) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    char kgt_name[INI_NAME_BUF_SIZE] = {};
    std::strncpy(kgt_name, basename, sizeof(kgt_name) - 1);
    if (char* dot = std::strrchr(kgt_name, '.')) *dot = '\0';
    if (std::strlen(kgt_name) + 4 >= sizeof(kgt_name)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: WriteKgtName — basename too long ('%s')",
            basename);
        return false;
    }
    std::strcat(kgt_name, ".kgt");

    auto* dst = reinterpret_cast<char*>(ADDR_INI_NAME_OVERRIDE);
    std::memset(dst, 0, INI_NAME_BUF_SIZE);
    std::memcpy(dst, kgt_name, std::strlen(kgt_name));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: WriteKgtName — '%s' → g_iniFile_nameOverride "
        "(0x%08lX)", kgt_name, (unsigned long)ADDR_INI_NAME_OVERRIDE);
    return true;
}

// Runtime BTB overrides — preferred over FM2K_BTB_* env vars when set.
// Spec hook populates these from SPEC_JOIN_ACK so the slot-0 dispatcher
// loads the host's actual chars instead of the launcher's placeholder.
// 0xFF sentinel = "not set, fall through to env var".
static uint8_t g_runtime_btb_p1_char = 0xFF;
static uint8_t g_runtime_btb_p2_char = 0xFF;
static uint8_t g_runtime_btb_stage   = 0xFF;
uint8_t g_runtime_btb_p1_color = 0xFF;
static volatile bool g_btb_natural_boot_abort = false;
uint8_t g_runtime_btb_p2_color = 0xFF;

void PerGamePatches_AbortBtbNaturalBoot() {
    g_btb_natural_boot_abort = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: BTB abort -> natural boot (host pre-battle; "
        "viewer will replay from frame 0)");
}

void PerGamePatches_SetRuntimeBtbOverrides(uint8_t p1_char,
                                           uint8_t p2_char,
                                           uint8_t stage,
                                           uint8_t p1_color,
                                           uint8_t p2_color) {
    g_runtime_btb_p1_char  = p1_char;
    g_runtime_btb_p2_char  = p2_char;
    g_runtime_btb_stage    = stage;
    g_runtime_btb_p1_color = p1_color;
    g_runtime_btb_p2_color = p2_color;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: runtime BTB overrides set (p1=%u/c%u p2=%u/c%u stage=%u)",
        (unsigned)p1_char, (unsigned)p1_color,
        (unsigned)p2_char, (unsigned)p2_color, (unsigned)stage);
}

// Char / stage / meter overrides applied at the same hook entry point.
// These globals are populated by hit_judge_set_function from kgt INI
// defaults (all 0 for shipped games with no editor INI). We re-stamp
// them here, after hit_judge_set_function and before the slot-0
// dispatcher reads them.
//
//   g_config_value1 @ 0x4300e0 — P1 char grid index (Player0Nb)
//   g_config_value2 @ 0x4300e4 — P1 super-meter init (Player0Cpu)
//   g_config_value3 @ 0x4300f0 — P2 char grid index (Player1Nb)
//   g_config_value4 @ 0x4300f4 — P2 super-meter init (Player1Cpu)
//   wParam         @ 0x43010c — stage index (StageNb); also written
//                                to g_fm2k_game_mode @ 0x470040 by
//                                the original dispatcher
//
// Env vars: FM2K_BTB_P1_CHAR / _P2_CHAR / _STAGE / _P1_METER / _P2_METER.
// Unset env var means no override (engine keeps the kgt-default value).
// Char/stage range-checked [0, 49] to match engine's grid-slot bound;
// out-of-range values are ignored. Meter values pass through unchecked.
void ApplyBootToBattleStateOverrides() {
    auto get_env_int = [](const char* name, int& out) -> bool {
        const char* e = std::getenv(name);
        if (!e || !*e) return false;
        out = std::atoi(e);
        return true;
    };

    int v;
    // Runtime override wins over env var — see PerGamePatches_SetRuntimeBtbOverrides.
    if (g_runtime_btb_p1_char != 0xFF && g_runtime_btb_p1_char < 50) {
        *(uint32_t*)0x004300E0 = (uint32_t)g_runtime_btb_p1_char;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 char → %u (runtime override)",
            (unsigned)g_runtime_btb_p1_char);
    } else if (get_env_int("FM2K_BTB_P1_CHAR", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x004300E0 = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 char → %d", v);
    }
    if (g_runtime_btb_p2_char != 0xFF && g_runtime_btb_p2_char < 50) {
        *(uint32_t*)0x004300F0 = (uint32_t)g_runtime_btb_p2_char;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 char → %u (runtime override)",
            (unsigned)g_runtime_btb_p2_char);
    } else if (get_env_int("FM2K_BTB_P2_CHAR", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x004300F0 = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 char → %d", v);
    }
    if (g_runtime_btb_stage != 0xFF && g_runtime_btb_stage < 50) {
        *(uint32_t*)0x0043010C = (uint32_t)g_runtime_btb_stage;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB stage → %u (runtime override)",
            (unsigned)g_runtime_btb_stage);
    } else if (get_env_int("FM2K_BTB_STAGE", v) && v >= 0 && v < 50) {
        *(uint32_t*)0x0043010C = (uint32_t)v;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB stage → %d", v);
    }
    if (get_env_int("FM2K_BTB_P1_METER", v)) {
        *(uint32_t*)0x004300E4 = (uint32_t)v;  // g_config_value2
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P1 meter init → %d", v);
    }
    if (get_env_int("FM2K_BTB_P2_METER", v)) {
        *(uint32_t*)0x004300F4 = (uint32_t)v;  // g_config_value4
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: BTB P2 meter init → %d", v);
    }
}

// MinHook detour for InitializeGameFromCommandLine. Restamps the kgt
// name into g_iniFile_nameOverride and applies char/stage/meter
// overrides at function entry, then calls original. The detour body is
// fall-through fast; only fires on the first main-loop tick (the
// function gates itself on g_object_data_ptr[338] and runs exactly
// once per process).
int __cdecl Hook_InitializeGameFromCommandLine() {
    // Spectator BTB hold (Phase F): a viewer's /F boot must load the
    // HOST'S character files, which arrive via the JOIN_ACK runtime
    // overrides -- but this dispatcher fires within ~1s of boot, long
    // before any ACK can land when the host is still at CSS (its first
    // ACK says kind=CSS with no chars; the battle-entry re-broadcast
    // carries the real ones). Booting early loaded default chars and the
    // later snapshot applied onto the wrong .player data (join-during-CSS
    // = ryu/ryu garbage, 2026-06-11).
    //
    // The hold must wait IN PLACE: the engine does NOT reliably re-call
    // this function -- an early-return hold left the game at mode 0
    // (black screen, queue piling up) when the ACK missed the boot
    // window. We pump the control channel ourselves so the JOIN_ACK can
    // arrive and seed the overrides while we wait.
    {
        static int s_is_spec = -1;
        if (s_is_spec < 0) {
            const char* v = std::getenv("FM2K_SPECTATOR_MODE");
            s_is_spec = (v && v[0] == '1') ? 1 : 0;
        }
        if (s_is_spec == 1 && g_runtime_btb_p1_char == 0xFF) {
            extern void ControlChannel_Poll();
            const uint64_t start = GetTickCount64();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: holding /F dispatch in place -- pumping "
                "control channel until JOIN_ACK seeds battle chars");
            uint64_t last_log = start;
            extern void SpectatorNode_KickJoin();
            for (;;) {
                SpectatorNode_KickJoin();  // hold may have beaten the
                                           // init path's JOIN_REQ send
                ControlChannel_Poll();   // delivers JOIN_ACK -> overrides
                if (g_btb_natural_boot_abort) {
                    *(uint32_t*)0x424744 = 0;  // g_debug_mode: drop /F
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch aborted after %llums "
                        "-- natural boot (host pre-battle)",
                        (unsigned long long)(GetTickCount64() - start));
                    break;
                }
                if (g_runtime_btb_p1_char != 0xFF) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch released after %llums "
                        "(battle chars seeded)",
                        (unsigned long long)(GetTickCount64() - start));
                    break;
                }
                const uint64_t now = GetTickCount64();
                if (now - start >= 8000) {
                    // Natural boot is the DEFAULT; /F is the exception
                    // that only the host's explicit "battle in progress"
                    // answer (kind=2 ACK seeding the chars) may engage.
                    // The old fallthrough booted /F with env-default
                    // chars (ryu/ryu garbage) whenever the ACK was slow
                    // or lost -- a spectator joining players at CSS must
                    // never battle-boot.
                    *(uint32_t*)0x424744 = 0;  // g_debug_mode: drop /F
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: /F dispatch hold timed out (8s) "
                        "-- defaulting to NATURAL boot (no ACK = no "
                        "battle authority)");
                    break;
                }
                if (now - last_log > 2000) {
                    last_log = now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PerGamePatches: still holding /F dispatch (%llums)",
                        (unsigned long long)(now - start));
                }
                Sleep(10);
            }
        }
    }

    WriteKgtNameToOverride();
    ApplyBootToBattleStateOverrides();
    int result = g_orig_init_game_from_cmd ? g_orig_init_game_from_cmd() : 0;

    // Stamp the host's real confirm-button colors over the /F hardcodes
    // (original sets slot0=0 / slot1=1 at 0x409CB7/0x409CBD; the type-14
    // battle-init object consumes them on a later tick, and
    // character_state_machine reads them live during battle). Button
    // choice IS the color in this engine -- /F never presses one.
    {
        constexpr uintptr_t kSlotColor0 = 0x4DFD8Bu;
        constexpr size_t    kSlotStride = 0xE03Fu;
        if (g_runtime_btb_p1_color != 0xFF && g_runtime_btb_p1_color < 8) {
            *(int32_t*)(kSlotColor0 + 0 * kSlotStride) = g_runtime_btb_p1_color;
        }
        if (g_runtime_btb_p2_color != 0xFF && g_runtime_btb_p2_color < 8) {
            *(int32_t*)(kSlotColor0 + 1 * kSlotStride) = g_runtime_btb_p2_color;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PerGamePatches: BTB colors stamped (p1=c%u p2=c%u)",
                (unsigned)g_runtime_btb_p1_color,
                (unsigned)g_runtime_btb_p2_color);
        }
    }

    // /F leaves g_debug_mode (0x424744) == 3 for the entire process
    // lifetime. The engine's LoadGameSystemFile (called from inside
    // the original we just returned from) sprintfs a "%s -テストプレイ-"
    // suffix into the window title and probes for ".t" variants of
    // every file (kgt, stage, player) — both are editor-only artifacts
    // that bleed into the spectator view. Zero the flag now so any
    // FUTURE engine code that branches on debug_mode takes the
    // production path (per-round LoadGameSystemFile, etc.). The one-
    // shot title leak from this initial call is overwritten within
    // 500ms by the launcher's BBBR-style stats updater.
    //
    // Note: tempted to also SetWindowTextA here to skip the 500ms
    // flash, but the engine's hWnd global address differs across FM2K
    // games (wanwan IDA shows 0x4246F8, pkmncc may differ) and writing
    // through a wrong-pointer-cast HWND can corrupt heap and cause
    // delayed crashes inside the engine's battle init. Trust the
    // launcher's title updater instead.
    *(uint32_t*)0x424744 = 0;

    return result;
}

bool PerGamePatches_InstallBootToBattleHook() {
    const char* e = std::getenv("FM2K_BOOT_TO_BATTLE");
    if (!e || std::strcmp(e, "1") != 0) return true;  // disabled — no-op

    if (MH_CreateHook((void*)ADDR_INIT_FROM_CMD,
                      (void*)Hook_InitializeGameFromCommandLine,
                      (void**)&g_orig_init_game_from_cmd) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_CreateHook(InitGameFromCmd) failed");
        return false;
    }
    if (MH_QueueEnableHook((void*)ADDR_INIT_FROM_CMD) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PerGamePatches: MH_QueueEnableHook(InitGameFromCmd) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "PerGamePatches: /F boot-to-battle hook installed @ 0x%08lX "
        "(re-stamps g_iniFile_nameOverride at slot-0 dispatcher entry)",
        (unsigned long)ADDR_INIT_FROM_CMD);
    return true;
}

#else  // ENGINE_FM95 — these patches are FM2K-runtime-specific (different
       // engine, different addresses, different round-state plumbing).

#include "per_game_patches.h"

bool PerGamePatches_InstallDamageMultiplierHook()              { return true; }
bool PerGamePatches_InstallKofHpInitPatch()                    { return true; }
void PerGamePatches_ApplyRuntime()                             {}
int  PerGamePatches_TryOverrideInput(int, uint32_t)            { return -1; }
uint16_t PerGamePatches_GatedP2CssInput(uint16_t)              { return 0; }
int      PerGamePatches_BattleInputOverride(int, uint16_t)     { return -1; }
void PerGamePatches_CycleTrainingP2Behavior()                  {}
int  PerGamePatches_GetTrainingP2Behavior()                    { return 0; }
const char* PerGamePatches_TrainingP2BehaviorLabel(int)        { return "?"; }
void PerGamePatches_OnOptionPressed()                          {}
int  PerGamePatches_GetVsSubmode()                             { return 0; }
int  PerGamePatches_GetVsMenuContext()                         { return 0; }
const char* PerGamePatches_VsSubmodeLabel(int, int)            { return "?"; }
bool PerGamePatches_IsOptionModeSelectorActive()               { return false; }
bool PerGamePatches_IsTrainingModeActive()                     { return false; }
bool PerGamePatches_IsVsCpuModeActive()                        { return false; }
bool PerGamePatches_IsCpuVsCpuModeActive()                     { return false; }
int  PerGamePatches_GetTeamSizeOverride()                      { return 0; }
void PerGamePatches_OnFrameTick()                              {}
void PerGamePatches_OnTitleInputTick(uint16_t, uint32_t)       {}
void PerGamePatches_OnGameStateManagerEntry()                  {}
bool PerGamePatches_InstallBootToBattleHook()                  { return true; }
bool PerGamePatches_InstallStoryInitHijack()                   { return true; }
void PerGamePatches_OnBattleInitComplete()                     {}
void PerGamePatches_SetRuntimeBtbOverrides(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void PerGamePatches_AbortBtbNaturalBoot()                      {}

#endif
