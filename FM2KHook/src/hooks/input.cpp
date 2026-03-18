// Input capture with window focus check
// Only captures input when OUR game window is focused
// P1: Arrow keys + ZXC + QWE (non-overlapping with P2)
// P2: WASD + UIO + JKL
#include "input.h"
#include <windows.h>
#include <SDL3/SDL_log.h>

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

    uint16_t input = 0;

    if (g_player_index == 0) {
        // P1: Arrow keys + ZXC + QWE (buttons 1-6)
        // Direction
        if (GetAsyncKeyState(VK_LEFT) & 0x8000)  input |= 0x001;  // Left
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) input |= 0x002;  // Right
        if (GetAsyncKeyState(VK_UP) & 0x8000)    input |= 0x004;  // Up
        if (GetAsyncKeyState(VK_DOWN) & 0x8000)  input |= 0x008;  // Down
        // Buttons (ZXC row)
        if (GetAsyncKeyState('Z') & 0x8000)      input |= 0x010;  // Button 1 (A)
        if (GetAsyncKeyState('X') & 0x8000)      input |= 0x020;  // Button 2 (B)
        if (GetAsyncKeyState('C') & 0x8000)      input |= 0x040;  // Button 3 (C)
        // Buttons (QWE row - no overlap with P2's WASD!)
        if (GetAsyncKeyState('Q') & 0x8000)      input |= 0x080;  // Button 4 (D)
        if (GetAsyncKeyState('W') & 0x8000)      input |= 0x100;  // Button 5 (E)
        if (GetAsyncKeyState('E') & 0x8000)      input |= 0x200;  // Button 6 (F)
    } else {
        // P2: WASD + UIO + JKL (buttons 1-6)
        // Direction
        if (GetAsyncKeyState('A') & 0x8000)      input |= 0x001;  // Left
        if (GetAsyncKeyState('D') & 0x8000)      input |= 0x002;  // Right
        if (GetAsyncKeyState('W') & 0x8000)      input |= 0x004;  // Up
        if (GetAsyncKeyState('S') & 0x8000)      input |= 0x008;  // Down
        // Buttons (UIO row)
        if (GetAsyncKeyState('U') & 0x8000)      input |= 0x010;  // Button 1 (A)
        if (GetAsyncKeyState('I') & 0x8000)      input |= 0x020;  // Button 2 (B)
        if (GetAsyncKeyState('O') & 0x8000)      input |= 0x040;  // Button 3 (C)
        // Buttons (JKL row)
        if (GetAsyncKeyState('J') & 0x8000)      input |= 0x080;  // Button 4 (D)
        if (GetAsyncKeyState('K') & 0x8000)      input |= 0x100;  // Button 5 (E)
        if (GetAsyncKeyState('L') & 0x8000)      input |= 0x200;  // Button 6 (F)
    }

    // Debug: log non-zero input occasionally
    static uint32_t log_count = 0;
    if (input != 0 && log_count++ < 20) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Input: P%d captured 0x%03X", g_player_index + 1, input);
    }

    return input;
}
