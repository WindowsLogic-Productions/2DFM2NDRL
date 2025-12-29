#include "input.h"
#include <windows.h>
#include <cstring>

// Cache our game window handle (found by PID + class name)
static HWND g_our_game_window = NULL;

static BOOL CALLBACK FindOurWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            if (strcmp(class_name, "KGT2KGAME") == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}

static HWND GetOurGameWindow() {
    // Cache the window handle
    if (g_our_game_window && IsWindow(g_our_game_window)) {
        return g_our_game_window;
    }
    g_our_game_window = NULL;
    EnumWindows(FindOurWindowProc, (LPARAM)&g_our_game_window);
    return g_our_game_window;
}

uint16_t Input_CaptureLocal() {
    uint16_t input = 0;

    // Only capture when OUR game window has focus
    // This prevents both P1 and P2 from capturing the same keyboard input
    HWND focused = GetForegroundWindow();
    HWND our_window = GetOurGameWindow();

    if (our_window == NULL || focused != our_window) {
        return 0;  // Not our window or not focused - return no input
    }

    // Key layout: TFBH for directions, ASDQWE for buttons
    if (GetAsyncKeyState('F') & 0x8000)       input |= 0x001;  // Left
    if (GetAsyncKeyState('H') & 0x8000)       input |= 0x002;  // Right
    if (GetAsyncKeyState('T') & 0x8000)       input |= 0x004;  // Up
    if (GetAsyncKeyState('B') & 0x8000)       input |= 0x008;  // Down
    if (GetAsyncKeyState('A') & 0x8000)       input |= 0x010;  // Button 1
    if (GetAsyncKeyState('S') & 0x8000)       input |= 0x020;  // Button 2
    if (GetAsyncKeyState('D') & 0x8000)       input |= 0x040;  // Button 3
    if (GetAsyncKeyState('Q') & 0x8000)       input |= 0x080;  // Button 4
    if (GetAsyncKeyState('W') & 0x8000)       input |= 0x100;  // Button 5
    if (GetAsyncKeyState('E') & 0x8000)       input |= 0x200;  // Button 6
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) input |= 0x400;  // Pause

    return input;
}
