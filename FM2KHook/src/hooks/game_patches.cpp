// Game patches for FM2K - multi-instance, boot to CSS
#include "game_patches.h"
#include "globals.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL_log.h>

// Bypass multi-instance check so multiple game instances can run
// FM2K uses FindWindowA("KGT2KGAME", 0) to check for existing instances
// If found, WinMain returns 1 and exits. We patch the conditional jump to always skip.
void BypassMultiInstanceCheck() {
    // At 0x405d05: jz loc_405D15 (0x74 0x0E) - jump if no window found
    // Change to: jmp loc_405D15 (0xEB 0x0E) - always jump (bypass instance check)
    uint8_t* jz_addr = (uint8_t*)0x405d05;

    DWORD old_protect;
    if (VirtualProtect(jz_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *jz_addr = 0xEB;  // jz -> jmp
        VirtualProtect(jz_addr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PATCH: Bypassed multi-instance check (jz -> jmp)");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PATCH: Failed to patch multi-instance check");
    }
}

// Boot directly to character select screen instead of title/splash screens
void ApplyBootToCharacterSelectPatches() {
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint16_t))) {
        DWORD old_protect;
        if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
            init_object_ptr[0] = 0x6A;  // push instruction
            init_object_ptr[1] = 0x0A;  // immediate value (CSS)
            VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PATCH: Boot to CSS enabled");
        }
    }
}

// Set character select mode flag - VS player (2P) mode instead of VS CPU (1P)
// g_game_mode_flag at 0x470058: 0=single player, 1-2=multiplayer
void ApplyCharacterSelectModePatches() {
    constexpr uintptr_t GAME_MODE_FLAG_ADDR = 0x470058;

    uint32_t* game_mode_flag_ptr = (uint32_t*)GAME_MODE_FLAG_ADDR;
    if (!IsBadWritePtr(game_mode_flag_ptr, sizeof(uint32_t))) {
        DWORD old_protect;
        if (VirtualProtect(game_mode_flag_ptr, sizeof(uint32_t), PAGE_READWRITE, &old_protect)) {
            *game_mode_flag_ptr = 1;  // 1 = VS player mode (2P)
            VirtualProtect(game_mode_flag_ptr, sizeof(uint32_t), old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PATCH: Set g_game_mode_flag=1 (VS player)");
        }
    }
}

// Defuse the case-200 t4-walk false positive in vs_round_function.
//
// vs_round_function's case 200 (active battle) walks the object pool for
// active type-4 fighter objects and transitions to state 300 (round end)
// if it finds <2. Vanilla main_game_loop runs `process_game_inputs +
// update_game` N times per outer iteration (frame-skip multi-tick), so
// fighters' KGT scripts complete their tick cycle and any transient
// alive_flag set/clear resolves before the round controller's case-200
// walk runs. Our trampoline does ONE update per outer iter, so case 200
// walks AFTER one fighter tick that may have left alive_flag transiently
// non-zero — case 200 sees t4 < 2 and false-fires the round-end transition.
//
// Confirmed via [CASE200-TRIP] diagnostic on StudioS Fighters / Strip
// Fighter Zero: pre_t4 oscillates 0/1/2 around update_game; post_t4
// always returns to 2. WW chars don't trigger this; StudioS chars do.
//
// asm at 0x408EC2-0x408ED8:
//   cmp esi, 2
//   jge short loc_408F18         ; 7D 51 — skip transition if t4 >= 2
//   mov edi, 12Ch
//   mov [ecx+156h], ebx           ; v2[342] = 0
//   mov [ecx+152h], edi           ; v2[338] = 300  ← false round-end
//   jmp short loc_408F1D
//
// Patch: change `jge short` (0x7D) to `jmp short` (0xEB) at 0x408EC5,
// so the transition is always skipped. Round-end still triggers via the
// legitimate paths (round_end_flag, score countdown crossing 0). The
// t4 walk becomes effectively a no-op.
//
// Safety: the t4 path is a redundant safety check — round_end_flag and
// the score timer are the engine's primary round-end triggers. NOPing
// the t4 transition only removes a false positive; it does not bypass
// any legitimate round-end path. WW behavior unchanged (its t4 walk
// never fires the false positive in the first place).
void PatchVsRoundCase200T4FalsePositive() {
    // Diagnostic: FM2K_SKIP_T4_PATCH=1 leaves vs_round_function's t4 walk
    // untouched. Used to bisect "is the t4 patch causing the StudioS
    // visual glitch?" If glitch persists with patch off → t4 isn't it.
    // If glitch goes away → patch is corrupting something we don't
    // understand yet.
    const char* env_skip = std::getenv("FM2K_SKIP_T4_PATCH");
    if (env_skip && std::strcmp(env_skip, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_T4_PATCH=1 — leaving vs_round_function "
            "case-200 t4 walk untouched. StudioS will likely bail to "
            "CSS shortly after entering battle.");
        return;
    }

    constexpr uintptr_t JGE_ADDR = 0x408EC5;
    uint8_t* jge_addr = (uint8_t*)JGE_ADDR;

    // Verify the EXACT instruction context. WW asm at 0x408EC2..0x408EC6:
    //   83 FE 02         cmp esi, 2      ; -3..-1
    //   7D 51            jge short +0x51 ;  0..+1  ← patch target
    //   BF 2C 01 00 00   mov edi, 12Ch   ; +2..+6
    // If the bytes at -3..+6 don't match this pattern, we're patching the
    // wrong function — different FM2K builds (StudioS Fighters, Strip
    // Fighter Zero, etc.) may have shifted code addresses even with the
    // same engine. Hitting a random 0x7D byte elsewhere corrupts unrelated
    // code and would explain odd visual / script glitches in StudioS.
    static const uint8_t EXPECTED[] = {
        0x83, 0xFE, 0x02,                 // -3..-1: cmp esi, 2
        0x7D, 0x51,                       //  0..+1: jge short loc_408F18
        0xBF, 0x2C, 0x01, 0x00, 0x00      // +2..+6: mov edi, 12Ch
    };
    const uint8_t* sig_base = jge_addr - 3;
    char hex[3 * sizeof(EXPECTED) + 1];
    for (size_t i = 0; i < sizeof(EXPECTED); ++i) {
        std::snprintf(hex + i * 3, 4, "%02X ", sig_base[i]);
    }

    bool sig_ok = true;
    for (size_t i = 0; i < sizeof(EXPECTED); ++i) {
        if (sig_base[i] != EXPECTED[i]) { sig_ok = false; break; }
    }

    if (!sig_ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: vs_round_function t4 patch — SIGNATURE MISMATCH at "
            "0x%08X. Bytes at -3..+6: [%s]. Expected: [83 FE 02 7D 51 BF "
            "2C 01 00 00]. This binary is not the WW build the patch was "
            "validated against — leaving t4 walk untouched to avoid "
            "corrupting random code.",
            (unsigned)JGE_ADDR, hex);
        return;
    }

    DWORD old_protect;
    if (VirtualProtect(jge_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *jge_addr = 0xEB;     // jge short -> jmp short
        VirtualProtect(jge_addr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: vs_round_function case-200 t4 walk neutered "
            "(jge -> jmp at 0x%08X, sig OK: [%s])",
            (unsigned)JGE_ADDR, hex);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: Failed to patch case-200 t4 walk");
    }
}
