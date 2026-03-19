// Minimal shared memory: hook -> launcher status reporting
#include "shared_mem.h"
#include "globals.h"
#include "netplay.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

static HANDLE g_shared_mem_handle = nullptr;
static FM2KSharedMemData* g_shared_mem = nullptr;

bool InitializeSharedMemory() {
    char name[64];
    snprintf(name, sizeof(name), "FM2K_SharedMem_%d", GetCurrentProcessId());

    size_t size = sizeof(FM2KSharedMemData);

    g_shared_mem_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, (DWORD)size, name);

    if (!g_shared_mem_handle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SharedMem: CreateFileMapping failed: %lu", GetLastError());
        return false;
    }

    g_shared_mem = (FM2KSharedMemData*)MapViewOfFile(
        g_shared_mem_handle, FILE_MAP_ALL_ACCESS, 0, 0, size);

    if (!g_shared_mem) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SharedMem: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(g_shared_mem_handle);
        g_shared_mem_handle = nullptr;
        return false;
    }

    memset(g_shared_mem, 0, size);
    g_shared_mem->magic = FM2K_SHARED_MEM_MAGIC;
    g_shared_mem->version = FM2K_SHARED_MEM_VERSION;
    g_shared_mem->player_index = (uint8_t)g_player_index;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SharedMem: Created '%s' (%zu bytes)", name, size);
    return true;
}

void CleanupSharedMemory() {
    if (g_shared_mem) {
        UnmapViewOfFile(g_shared_mem);
        g_shared_mem = nullptr;
    }
    if (g_shared_mem_handle) {
        CloseHandle(g_shared_mem_handle);
        g_shared_mem_handle = nullptr;
    }
}

FM2KSharedMemData* GetSharedMemory() {
    return g_shared_mem;
}

void SharedMem_Update() {
    if (!g_shared_mem) return;

    g_shared_mem->player_index = (uint8_t)g_player_index;
    g_shared_mem->game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    g_shared_mem->frame_number = Netplay_GetFrame();
    g_shared_mem->rollback_count = Netplay_GetRollbackCount();
    g_shared_mem->desync_count = Netplay_GetDesyncCount();
    g_shared_mem->frames_ahead = Netplay_GetFramesAhead();
    g_shared_mem->rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;

    g_shared_mem->netplay_state = Netplay_IsActive() ? 2 :
                                  Netplay_IsConnected() ? 1 : 0;
    g_shared_mem->session_ready = Netplay_IsSessionReady() ? 1 : 0;
}
