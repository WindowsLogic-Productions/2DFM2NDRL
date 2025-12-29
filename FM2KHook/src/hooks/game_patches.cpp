// Game patches for FM2K - multi-instance, boot to CSS
#include "game_patches.h"
#include "globals.h"
#include <windows.h>
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
