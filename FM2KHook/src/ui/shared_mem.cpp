// Minimal shared memory implementation for launcher communication
#include "shared_mem.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

static HANDLE g_shared_mem_handle = nullptr;
static SharedInputData* g_shared_mem = nullptr;

bool InitializeSharedMemory() {
    // Create shared memory with PID-based name (launcher expects this format)
    char name[64];
    snprintf(name, sizeof(name), "FM2K_InputSharedMemory_%d", GetCurrentProcessId());

    size_t size = sizeof(SharedInputData);

    g_shared_mem_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        (DWORD)size,
        name
    );

    if (!g_shared_mem_handle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: CreateFileMapping failed: %lu", GetLastError());
        return false;
    }

    g_shared_mem = (SharedInputData*)MapViewOfFile(
        g_shared_mem_handle,
        FILE_MAP_ALL_ACCESS,
        0, 0, size
    );

    if (!g_shared_mem) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(g_shared_mem_handle);
        g_shared_mem_handle = nullptr;
        return false;
    }

    // Initialize to zero
    memset(g_shared_mem, 0, size);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: Created '%s' (%zu KB)", name, size / 1024);
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: Cleaned up");
}

SharedInputData* GetSharedMemory() {
    return g_shared_mem;
}

// Stubs for functions declared in header but not needed for minimal implementation
void ProcessDebugCommands() {}
bool CheckConfigurationUpdates() { return false; }
void UpdateRollbackStats(uint32_t) {}
void UpdateEnhancedActionData() {}

// Forward declare DetailedObject if needed
namespace FM2K { namespace ObjectPool { struct DetailedObject; } }
void PopulateEnhancedActionInfo(const FM2K::ObjectPool::DetailedObject&, SharedInputData::EnhancedActionData&) {}
void AnalyzeScriptCommand(const FM2K::ObjectPool::DetailedObject&, SharedInputData::EnhancedActionData&) {}
