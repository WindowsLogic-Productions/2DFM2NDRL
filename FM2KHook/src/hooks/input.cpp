// Input capture — taps FM2K's native input pipeline instead of polling
// keys with GetAsyncKeyState.
//
// Why: 2DFM has its own input binding system (per-player keyboard +
// joystick, configurable via the game's options dialog, persisted to
// config). Hardcoding a keymap in the hook forces users onto our layout
// and duplicates the game's polling code. Instead we call the game's
// own input reader directly through the MinHook trampoline
// (original_get_player_input) for player index 0 — this returns
// whatever key / joystick mapping FM2K has configured for P1.
//
// Every instance uses the P1 mapping regardless of its netplay slot:
// the user only ever configures one profile. The netplay layer routes
// this local input into the correct remote character slot via
// Hook_GetPlayerInput.
#include "input.h"
#include "../core/globals.h"           // original_get_player_input, g_player_index
#include "../netplay/savestate.h"      // CHAR_SLOT_BASE, CHAR_SLOT_SIZE
#include <windows.h>

// Player index we're capturing for (set externally)
extern int g_player_index;

// Cache our game window handle
static HWND g_our_window = NULL;

// Find our game window by matching process ID and class name
static BOOL CALLBACK FindOurWindowCallback(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            if (strcmp(class_name, "KGT2KGAME") == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;  // Found it, stop enumeration
            }
        }
    }
    return TRUE;
}

static HWND GetOurWindow() {
    // Re-check periodically in case window was recreated
    if (g_our_window && IsWindow(g_our_window)) {
        return g_our_window;
    }

    g_our_window = NULL;
    EnumWindows(FindOurWindowCallback, (LPARAM)&g_our_window);
    return g_our_window;
}

static bool IsOurWindowFocused() {
    HWND our_window = GetOurWindow();
    if (!our_window) {
        return false;
    }

    HWND focused = GetForegroundWindow();
    return (focused == our_window);
}

uint16_t Input_CaptureLocal() {
    // CRITICAL: Only capture input if our window is focused
    // This prevents cross-instance input bleeding
    if (!IsOurWindowFocused()) {
        return 0;  // Not focused, no input
    }

    // Ask the game directly for P1's input. original_get_player_input is
    // MinHook's trampoline to the real get_player_input @ 0x414340 — calling
    // it reads the game's configured keyboard + joystick binding for player 0
    // (the P1 mapping) regardless of which netplay slot this instance occupies.
    //
    // CRITICAL: the game's input function APPLIES FACING SWAP internally
    // before returning (LEFT/RIGHT bits get flipped when the referenced
    // character is facing the opposite direction). But gekko's job is to
    // deliver RAW inputs, and Hook_GetPlayerInput re-applies the swap
    // during sim. If we forwarded the game's pre-swapped value to gekko,
    // sim's hook would swap it AGAIN — net effect, directions reversed
    // whenever the character faces the non-default direction.
    //
    // Fix: detect the same swap condition Hook_GetPlayerInput uses (slot 0
    // active + state_flags bit 3 clear during battle mode) and UNDO the
    // game's swap. Gekko then receives the raw key press; sim applies the
    // swap exactly once; user's LEFT key maps to LEFT on-screen regardless
    // of which side they're standing on.
    if (!original_get_player_input) return 0;
    uint16_t input = (uint16_t)(original_get_player_input(0, 0) & 0x7FF);

    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode >= 3000 && game_mode < 4000) {
        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;
        // input_type=0 matches the first arg original_get_player_input got
        // above — same slot determines whether the swap was applied.
        uintptr_t slot_base = CHAR_SLOT_BASE + 0 * CHAR_SLOT_SIZE;
        uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
        if (char_active != 0) {
            uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
            if ((char_flags & 8) == 0) {
                // Game's fn swapped bit 0 (left) <-> bit 1 (right). Reverse.
                uint16_t left  = (input & 0x001);
                uint16_t right = (input & 0x002);
                input = (input & ~0x003) | (left << 1) | (right >> 1);
            }
        }
    }

    return input;
}
