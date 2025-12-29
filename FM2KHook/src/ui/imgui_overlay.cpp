// ImGui overlay for FM2K - debug display
#include "imgui_overlay.h"
#include "globals.h"
#include "netplay.h"
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

// Forward declarations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Render debug overlay
void RenderDebugOverlay() {
    if (!g_overlay_visible) return;

    ImGui::Begin("FM2K Debug", &g_overlay_visible);

    if (ImGui::BeginTabBar("DebugTabs")) {
        // Network Tab
        if (ImGui::BeginTabItem("Network")) {
            ImGui::Text("Player Index: %d (P%d)", g_player_index, g_player_index + 1);
            ImGui::Text("Netplay Active: %s", Netplay_IsActive() ? "YES" : "NO");
            ImGui::Text("Frame Counter: %u", g_frame_counter);

            ImGui::Separator();

            // Game mode
            uint32_t* game_mode = (uint32_t*)FM2K::ADDR_GAME_MODE;
            ImGui::Text("Game Mode: %u", *game_mode);
            if (*game_mode < 3000) {
                ImGui::Text("State: CSS/Menu");
            } else if (*game_mode >= 3000 && *game_mode < 4000) {
                ImGui::Text("State: Battle");
            } else {
                ImGui::Text("State: Other");
            }

            ImGui::EndTabItem();
        }

        // Input Tab
        if (ImGui::BeginTabItem("Input")) {
            uint16_t* p1_input = (uint16_t*)FM2K::ADDR_P1_INPUT;
            uint16_t* p2_input = (uint16_t*)FM2K::ADDR_P2_INPUT;

            ImGui::Columns(2);

            ImGui::Text("P1 Input: 0x%03X", *p1_input);
            ImGui::Text("  LEFT:  %s", (*p1_input & 0x001) ? "[X]" : "[ ]");
            ImGui::Text("  RIGHT: %s", (*p1_input & 0x002) ? "[X]" : "[ ]");
            ImGui::Text("  UP:    %s", (*p1_input & 0x004) ? "[X]" : "[ ]");
            ImGui::Text("  DOWN:  %s", (*p1_input & 0x008) ? "[X]" : "[ ]");
            ImGui::Text("  A:     %s", (*p1_input & 0x010) ? "[X]" : "[ ]");
            ImGui::Text("  B:     %s", (*p1_input & 0x020) ? "[X]" : "[ ]");
            ImGui::Text("  C:     %s", (*p1_input & 0x040) ? "[X]" : "[ ]");

            ImGui::NextColumn();

            ImGui::Text("P2 Input: 0x%03X", *p2_input);
            ImGui::Text("  LEFT:  %s", (*p2_input & 0x001) ? "[X]" : "[ ]");
            ImGui::Text("  RIGHT: %s", (*p2_input & 0x002) ? "[X]" : "[ ]");
            ImGui::Text("  UP:    %s", (*p2_input & 0x004) ? "[X]" : "[ ]");
            ImGui::Text("  DOWN:  %s", (*p2_input & 0x008) ? "[X]" : "[ ]");
            ImGui::Text("  A:     %s", (*p2_input & 0x010) ? "[X]" : "[ ]");
            ImGui::Text("  B:     %s", (*p2_input & 0x020) ? "[X]" : "[ ]");
            ImGui::Text("  C:     %s", (*p2_input & 0x040) ? "[X]" : "[ ]");

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // Battle Tab
        if (ImGui::BeginTabItem("Battle")) {
            uint32_t* p1_hp = (uint32_t*)FM2K::ADDR_P1_HP;
            uint32_t* p2_hp = (uint32_t*)FM2K::ADDR_P2_HP;
            uint32_t* rng = (uint32_t*)FM2K::ADDR_RANDOM_SEED;

            ImGui::Text("P1 HP: %u", *p1_hp);
            ImGui::Text("P2 HP: %u", *p2_hp);
            ImGui::Separator();
            ImGui::Text("RNG Seed: 0x%08X", *rng);

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
}
