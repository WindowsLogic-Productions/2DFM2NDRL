// ImGui overlay for FM2K - debug display
#include "imgui_overlay.h"
#include "globals.h"
#include "netplay.h"
#include "savestate.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <cstdio>

// D3D9 function typedefs
typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9 pDevice);
typedef HRESULT(APIENTRY* Reset_t)(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);

// Original function pointers
static EndScene_t EndScene_orig = nullptr;
static Reset_t Reset_orig = nullptr;
static WNDPROC oWndProc = nullptr;

// State
static bool g_overlay_visible = false;
static bool g_imgui_initialized = false;
static bool g_hooks_installed = false;
static HWND g_game_window = nullptr;
static bool g_f9_key_pressed = false;
static bool g_f10_key_pressed = false;

// Test results
static bool g_last_test_ran = false;
static bool g_last_test_passed = false;
static char g_last_test_diff[1024] = "";
static StateSnapshot g_last_snapshot_before;
static StateSnapshot g_last_snapshot_after;

// Forward declarations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Memory addresses for debug display
namespace DebugAddrs {
    // CSS cursor positions
    constexpr uintptr_t CSS_P1_CURSOR_X = 0x470020;  // stage_positions[0]
    constexpr uintptr_t CSS_P2_CURSOR_X = 0x470024;  // stage_positions[1]

    // Player positions in battle (object pool)
    constexpr uintptr_t OBJECT_POOL = 0x4701E0;
    constexpr size_t OBJECT_SIZE = 382;
    constexpr size_t OBJ_X_OFFSET = 8;
    constexpr size_t OBJ_Y_OFFSET = 12;

    // Input tracking
    constexpr uintptr_t INPUT_BUFFER_INDEX = 0x447EE0;

    // Timer
    constexpr uintptr_t ROUND_TIMER = 0x470068;
}

// Render debug overlay
void RenderDebugOverlay() {
    if (!g_overlay_visible) return;

    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("FM2K Debug [F9]", &g_overlay_visible);

    // Always show critical sync info at top
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    uint32_t rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    uint32_t buf_idx = *(uint32_t*)DebugAddrs::INPUT_BUFFER_INDEX;

    ImGui::TextColored(ImVec4(1,1,0,1), "=== SYNC STATUS ===");
    ImGui::Text("Player: P%d | Mode: %u | Frame: %u",
        g_player_index + 1, game_mode, g_frame_counter);
    ImGui::Text("RNG: 0x%08X | BufIdx: %u", rng, buf_idx);
    ImGui::Text("Netplay: %s | Active: %s",
        Netplay_IsConnected() ? "CONN" : "DISC",
        Netplay_IsActive() ? "YES" : "NO");
    ImGui::Separator();

    if (ImGui::BeginTabBar("DebugTabs")) {
        // CSS Tab - for debugging character select desync
        if (ImGui::BeginTabItem("CSS")) {
            uint32_t p1_cursor = *(uint32_t*)DebugAddrs::CSS_P1_CURSOR_X;
            uint32_t p2_cursor = *(uint32_t*)DebugAddrs::CSS_P2_CURSOR_X;
            uint16_t timer = *(uint16_t*)DebugAddrs::ROUND_TIMER;

            ImGui::TextColored(ImVec4(0,1,1,1), "Character Select State");
            ImGui::Text("P1 Cursor Position: %u", p1_cursor);
            ImGui::Text("P2 Cursor Position: %u", p2_cursor);
            ImGui::Text("Timer: %u", timer);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5,0,1), "CSS Input (from control channel)");
            ImGui::Text("Local CSS Input:  0x%03X", Netplay_GetCSSInput(g_player_index));
            ImGui::Text("Remote CSS Input: 0x%03X", Netplay_GetCSSInput(1 - g_player_index));

            ImGui::EndTabItem();
        }

        // Battle Tab - for debugging battle desync
        if (ImGui::BeginTabItem("Battle")) {
            // Read player positions from object pool
            uint8_t* obj_pool = (uint8_t*)DebugAddrs::OBJECT_POOL;

            int32_t p1_x = *(int32_t*)(obj_pool + 0 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_X_OFFSET);
            int32_t p1_y = *(int32_t*)(obj_pool + 0 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_Y_OFFSET);
            int32_t p2_x = *(int32_t*)(obj_pool + 1 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_X_OFFSET);
            int32_t p2_y = *(int32_t*)(obj_pool + 1 * DebugAddrs::OBJECT_SIZE + DebugAddrs::OBJ_Y_OFFSET);

            uint32_t* p1_hp = (uint32_t*)FM2K::ADDR_P1_HP;
            uint32_t* p2_hp = (uint32_t*)FM2K::ADDR_P2_HP;

            ImGui::TextColored(ImVec4(0,1,0,1), "Player Positions (CRITICAL FOR DESYNC)");
            ImGui::Columns(2);

            ImGui::Text("P1 Position:");
            ImGui::Text("  X: %d", p1_x);
            ImGui::Text("  Y: %d", p1_y);
            ImGui::Text("  HP: %u", *p1_hp);

            ImGui::NextColumn();

            ImGui::Text("P2 Position:");
            ImGui::Text("  X: %d", p2_x);
            ImGui::Text("  Y: %d", p2_y);
            ImGui::Text("  HP: %u", *p2_hp);

            ImGui::Columns(1);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5,0,1), "Synced Inputs (from GekkoNet)");
            ImGui::Text("P1 Input: 0x%03X", Netplay_GetInput(0));
            ImGui::Text("P2 Input: 0x%03X", Netplay_GetInput(1));

            ImGui::EndTabItem();
        }

        // Input Tab - raw input display
        if (ImGui::BeginTabItem("Input")) {
            uint16_t p1_input = *(uint16_t*)FM2K::ADDR_P1_INPUT;
            uint16_t p2_input = *(uint16_t*)FM2K::ADDR_P2_INPUT;

            ImGui::TextColored(ImVec4(1,1,0,1), "Game Memory Inputs");
            ImGui::Columns(2);

            ImGui::Text("P1: 0x%03X", p1_input);
            ImGui::Text("  L:%d R:%d U:%d D:%d",
                (p1_input>>0)&1, (p1_input>>1)&1, (p1_input>>2)&1, (p1_input>>3)&1);
            ImGui::Text("  A:%d B:%d C:%d D:%d",
                (p1_input>>4)&1, (p1_input>>5)&1, (p1_input>>6)&1, (p1_input>>7)&1);

            ImGui::NextColumn();

            ImGui::Text("P2: 0x%03X", p2_input);
            ImGui::Text("  L:%d R:%d U:%d D:%d",
                (p2_input>>0)&1, (p2_input>>1)&1, (p2_input>>2)&1, (p2_input>>3)&1);
            ImGui::Text("  A:%d B:%d C:%d D:%d",
                (p2_input>>4)&1, (p2_input>>5)&1, (p2_input>>6)&1, (p2_input>>7)&1);

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // Memory Tab - raw memory inspection
        if (ImGui::BeginTabItem("Memory")) {
            ImGui::TextColored(ImVec4(1,0,1,1), "Critical Addresses");

            ImGui::Text("GAME_MODE:    0x%08X = %u", FM2K::ADDR_GAME_MODE, *(uint32_t*)FM2K::ADDR_GAME_MODE);
            ImGui::Text("RANDOM_SEED:  0x%08X = 0x%08X", FM2K::ADDR_RANDOM_SEED, *(uint32_t*)FM2K::ADDR_RANDOM_SEED);
            ImGui::Text("INPUT_BUF_IDX:0x%08X = %u", DebugAddrs::INPUT_BUFFER_INDEX, *(uint32_t*)DebugAddrs::INPUT_BUFFER_INDEX);
            ImGui::Text("P1_INPUT:     0x%08X = 0x%03X", FM2K::ADDR_P1_INPUT, *(uint16_t*)FM2K::ADDR_P1_INPUT);
            ImGui::Text("P2_INPUT:     0x%08X = 0x%03X", FM2K::ADDR_P2_INPUT, *(uint16_t*)FM2K::ADDR_P2_INPUT);
            ImGui::Text("ROUND_TIMER:  0x%08X = %u", DebugAddrs::ROUND_TIMER, *(uint16_t*)DebugAddrs::ROUND_TIMER);

            ImGui::EndTabItem();
        }

        // Test Tab - savestate verification
        if (ImGui::BeginTabItem("Test")) {
            ImGui::TextColored(ImVec4(1,1,0,1), "Rollback Verification [F10]");
            ImGui::Separator();

            // Current state snapshot
            StateSnapshot current = SaveState_CaptureSnapshot();
            ImGui::Text("Current State:");
            ImGui::Text("  RNG: 0x%08X", current.rng_seed);
            ImGui::Text("  P1: (%d, %d) HP=%u", current.p1_x, current.p1_y, current.p1_hp);
            ImGui::Text("  P2: (%d, %d) HP=%u", current.p2_x, current.p2_y, current.p2_hp);
            ImGui::Text("  Full Checksum: 0x%08X", current.checksum);
            ImGui::Separator();

            // Run test button
            if (ImGui::Button("Run Roundtrip Test [F10]")) {
                g_last_snapshot_before = SaveState_CaptureSnapshot();
                g_last_test_passed = SaveState_TestRoundtrip();
                g_last_snapshot_after = SaveState_CaptureSnapshot();
                SaveState_CompareSnapshots(g_last_snapshot_before, g_last_snapshot_after,
                    g_last_test_diff, sizeof(g_last_test_diff));
                g_last_test_ran = true;
            }

            ImGui::SameLine();
            if (g_last_test_ran) {
                if (g_last_test_passed) {
                    ImGui::TextColored(ImVec4(0,1,0,1), "PASSED");
                } else {
                    ImGui::TextColored(ImVec4(1,0,0,1), "FAILED");
                }
            }

            // Show last test results
            if (g_last_test_ran) {
                ImGui::Separator();
                ImGui::Text("Last Test Results:");
                ImGui::Text("Before: RNG=0x%08X P1=(%d,%d) chk=0x%08X",
                    g_last_snapshot_before.rng_seed,
                    g_last_snapshot_before.p1_x, g_last_snapshot_before.p1_y,
                    g_last_snapshot_before.checksum);
                ImGui::Text("After:  RNG=0x%08X P1=(%d,%d) chk=0x%08X",
                    g_last_snapshot_after.rng_seed,
                    g_last_snapshot_after.p1_x, g_last_snapshot_after.p1_y,
                    g_last_snapshot_after.checksum);

                if (!g_last_test_passed && g_last_test_diff[0]) {
                    ImGui::TextColored(ImVec4(1,0.5,0,1), "Differences:");
                    ImGui::TextWrapped("%s", g_last_test_diff);
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Hook implementations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (g_overlay_visible) {
        if (!g_imgui_initialized) {
            g_imgui_initialized = true;
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

            if (!g_game_window) {
                g_game_window = FindWindowA(nullptr, "WonderfulWorld");
                if (!g_game_window) g_game_window = GetForegroundWindow();
            }

            // Hook WndProc
            if (g_game_window && !oWndProc) {
                oWndProc = (WNDPROC)SetWindowLongPtr(g_game_window, GWLP_WNDPROC, (LONG_PTR)Hook_WndProc);
            }

            ImGui_ImplWin32_Init(g_game_window);
            ImGui_ImplDX9_Init(pDevice);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui initialized");
        }

        ClipCursor(nullptr);
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderDebugOverlay();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    return EndScene_orig(pDevice);
}

HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    return Reset_orig(pDevice, pPresentationParameters);
}

LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_overlay_visible && g_imgui_initialized) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return true;
        }
    }
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

DWORD WINAPI DirectXInit(LPVOID lpParameter) {
    // Wait for d3d9.dll
    int attempts = 0;
    while (!GetModuleHandleA("d3d9.dll") && attempts < 50) {
        Sleep(100);
        attempts++;
    }

    if (!GetModuleHandleA("d3d9.dll")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "d3d9.dll not found");
        return 0;
    }

    // Create temp window
    HWND tmpWnd = CreateWindowA("BUTTON", "TempD3D", WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 300,
                                nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!tmpWnd) return 0;

    LPDIRECT3D9 d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(tmpWnd);
        return 0;
    }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = tmpWnd;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;

    LPDIRECT3DDEVICE9 d3ddev = nullptr;
    if (d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, tmpWnd,
                          D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &d3ddev) != D3D_OK) {
        d3d->Release();
        DestroyWindow(tmpWnd);
        return 0;
    }

    DWORD* vtable = *(DWORD**)d3ddev;

    // Hook EndScene and Reset
    MH_CreateHook((void*)vtable[42], (void*)Hook_EndScene, (void**)&EndScene_orig);
    MH_EnableHook((void*)vtable[42]);
    MH_CreateHook((void*)vtable[16], (void*)Hook_Reset, (void**)&Reset_orig);
    MH_EnableHook((void*)vtable[16]);

    d3ddev->Release();
    d3d->Release();
    DestroyWindow(tmpWnd);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "D3D9 hooks installed - Press F9 to toggle overlay");
    return 1;
}

bool InitializeImGuiOverlay() {
    HANDLE thread = CreateThread(nullptr, 0, DirectXInit, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        return true;
    }
    return false;
}

void ShutdownImGuiOverlay() {
    if (g_imgui_initialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_initialized = false;
    }
    if (g_game_window && oWndProc) {
        SetWindowLongPtr(g_game_window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    }
}

bool IsOverlayVisible() { return g_overlay_visible; }

void ToggleOverlay() {
    g_overlay_visible = !g_overlay_visible;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Overlay %s", g_overlay_visible ? "shown" : "hidden");
}

void CheckOverlayHotkey() {
    // F9 - Toggle overlay
    bool f9_current = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_current && !g_f9_key_pressed) {
        if (!g_hooks_installed) {
            if (InitializeImGuiOverlay()) {
                g_hooks_installed = true;
            }
        }
        ToggleOverlay();
    }
    g_f9_key_pressed = f9_current;

    // F10 - Run savestate roundtrip test
    bool f10_current = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_current && !g_f10_key_pressed) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "F10 pressed - running roundtrip test");
        g_last_snapshot_before = SaveState_CaptureSnapshot();
        g_last_test_passed = SaveState_TestRoundtrip();
        g_last_snapshot_after = SaveState_CaptureSnapshot();
        SaveState_CompareSnapshots(g_last_snapshot_before, g_last_snapshot_after,
            g_last_test_diff, sizeof(g_last_test_diff));
        g_last_test_ran = true;
    }
    g_f10_key_pressed = f10_current;
}
