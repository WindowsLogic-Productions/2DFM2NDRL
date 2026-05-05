#include "FM2K_DLLInjector.h"
#include <SDL3/SDL.h>

namespace FM2K {

bool DLLInjector::InjectAndInit(HANDLE process, const std::wstring& dll_path) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Inject: starting (pid=%lu, dll=%ls)",
        (unsigned long)GetProcessId(process), dll_path.c_str());

    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle for injection");
        return false;
    }

    // Get LoadLibraryW address
    LPVOID load_lib_addr = GetLoadLibraryAddr(process);
    if (!load_lib_addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Inject: GetLoadLibraryW failed — kernel32.dll missing or stripped (rare). "
            "If this triggered, paste this log and your antivirus name on Discord.");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Inject: LoadLibraryW @ %p", load_lib_addr);

    HANDLE thread = CreateLoadLibraryThread(process, load_lib_addr, dll_path);
    if (!thread) {
        DWORD error = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Inject: CreateRemoteThread failed (err=%lu). "
            "Common causes: antivirus blocking remote thread injection (Windows "
            "Defender, Bitdefender, Kaspersky), or running launcher without admin "
            "while game has elevated process protection. Try running launcher "
            "as administrator.", error);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Inject: remote LoadLibraryW thread spawned");

    // Wait for DLL to signal — but if it times out, dig into WHY.
    if (!WaitForDLLInit(5000)) {
        // Was the LoadLibrary thread itself successful?
        DWORD lib_handle = 0;
        DWORD wait2 = WaitForSingleObject(thread, 100);
        if (wait2 == WAIT_OBJECT_0) {
            GetExitCodeThread(thread, &lib_handle);
            if (lib_handle == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Inject: LoadLibraryW returned NULL — the DLL itself failed to "
                    "load. Most common causes: (a) DLL file is blocked by Windows "
                    "(right-click FM2KHook.dll → Properties → tick Unblock), "
                    "(b) missing Visual C++ runtime / mingw runtime in target process, "
                    "(c) DLL path contains characters Windows can't pass to "
                    "LoadLibrary. DLL was: %ls", dll_path.c_str());
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Inject: LoadLibraryW returned a module handle (0x%lx) but the "
                    "DLL never signaled FM2KHook_Initialized. Hook DllMain probably "
                    "ran but hung or crashed BEFORE finishing init. Check the game's "
                    "FM2K_P*_Debug.log next to the EXE — DllMain prints the first "
                    "few init steps. If that log is empty, MinHook_Initialize most "
                    "likely failed (incompatible game or 32/64-bit mismatch).",
                    lib_handle);
            }
        } else {
            // LoadLibrary thread is still running after 5.1s — really hung.
            DWORD pid_alive = WaitForSingleObject(process, 0);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Inject: LoadLibraryW thread is still running after 5.1s "
                "(process %s). DllMain is hung or doing very heavy work. "
                "Check FM2K_P*_Debug.log; if the game window appears but logs "
                "stop part-way, MinHook setup may be deadlocking on a hook target.",
                pid_alive == WAIT_TIMEOUT ? "alive" : "dead");
        }
        CloseHandle(thread);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Inject: DLL signaled init complete");
    CloseHandle(thread);
    return true;
}

bool DLLInjector::Uninject(HANDLE process, const std::wstring& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Uninjecting DLL: %ls", dll_path.c_str());
    // TODO: Implement DLL uninjection
    return true;
}

LPVOID DLLInjector::GetLoadLibraryAddr(HANDLE process) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to get kernel32.dll handle. Error: %lu", GetLastError());
        return nullptr;
    }
    
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to get LoadLibraryW address. Error: %lu", GetLastError());
    }
    return addr;
}

HANDLE DLLInjector::CreateLoadLibraryThread(HANDLE process, LPVOID load_lib_addr, const std::wstring& dll_path) {
    // Allocate memory in target process for DLL path
    SIZE_T path_size = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_size, 
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to allocate memory in target process. Error: %lu", GetLastError());
        return nullptr;
    }

    // Write DLL path to target process memory
    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(), path_size, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to write DLL path to target process. Error: %lu", GetLastError());
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    // Create remote thread to call LoadLibraryW
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib_addr),
        remote_path, 0, nullptr);
    
    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to create remote thread. Error: %lu", GetLastError());
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    return thread;
}

bool DLLInjector::WaitForDLLInit(DWORD timeout_ms) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "Waiting up to %lu ms for DLL initialization...", timeout_ms);
    
    // Create/open a named event that the DLL will signal when initialized
    HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
    if (!init_event) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to create initialization event. Error: %lu", GetLastError());
        return false;
    }

    // Wait for the DLL to signal initialization
    DWORD wait_result = WaitForSingleObject(init_event, timeout_ms);
    CloseHandle(init_event);

    if (wait_result == WAIT_OBJECT_0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "DLL initialization completed successfully");
        return true;
    } else if (wait_result == WAIT_TIMEOUT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "DLL initialization timed out after %lu ms", timeout_ms);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Wait for DLL initialization failed. Error: %lu", GetLastError());
    }
    return false;
}

} // namespace FM2K     