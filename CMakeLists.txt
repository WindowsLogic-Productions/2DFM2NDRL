cmake_minimum_required(VERSION 3.20)

# Set up cross-compilation for Windows target from WSL
# set(CMAKE_SYSTEM_NAME Windows)
# set(CMAKE_SYSTEM_PROCESSOR i686)
# set(CMAKE_CROSSCOMPILING TRUE)

# Configure compilers before project()
# set(CMAKE_C_COMPILER /usr/bin/i686-w64-mingw32-gcc)
# set(CMAKE_CXX_COMPILER /usr/bin/i686-w64-mingw32-g++)
# set(CMAKE_RC_COMPILER /usr/bin/i686-w64-mingw32-windres)
# set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})

# Configure paths
# set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Define project
project(FM2K_RollbackLauncher VERSION 1.0.0 LANGUAGES CXX C RC)

# Set C++ standard early
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinGW-specific flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")

# Force static libraries for everything
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# Force skip GLIBC check
set(CMAKE_REQUIRED_DEFINITIONS "-D__GLIBC__=0")

# Pre-set GCC feature test results
set(HAVE_GDWARF_4 TRUE)
set(HAVE_GCC_WALL TRUE)
set(HAVE_GCC_WUNDEF TRUE)
set(HAVE_GCC_WFLOAT_CONVERSION TRUE)
set(HAVE_GCC_NO_STRICT_ALIASING TRUE)
set(HAVE_GCC_WDOCUMENTATION FALSE)
set(HAVE_GCC_WDOCUMENTATION_UNKNOWN_COMMAND FALSE)
set(HAVE_GCC_COMMENT_BLOCK_COMMANDS FALSE)
set(HAVE_CLANG_COMMENT_BLOCK_COMMANDS FALSE)
set(HAVE_GCC_WSHADOW TRUE)
set(HAVE_GCC_WUNUSED_LOCAL_TYPEDEFS TRUE)
set(HAVE_GCC_WIMPLICIT_FALLTHROUGH TRUE)
set(COMPILER_SUPPORTS_FDIAGNOSTICS_COLOR_ALWAYS TRUE)
set(LINKER_SUPPORTS_VERSION_SCRIPT TRUE)
set(COMPILER_SUPPORTS_GCC_ATOMICS TRUE)
set(HAVE_GCC_FVISIBILITY TRUE)
set(LINKER_SUPPORTS_WL_NO_UNDEFINED TRUE)
set(COMPILER_SUPPORTS_MMX TRUE)
set(COMPILER_SUPPORTS_SSE TRUE)
set(COMPILER_SUPPORTS_SSE2 TRUE)
set(COMPILER_SUPPORTS_SSE3 TRUE)
set(COMPILER_SUPPORTS_SSE4_1 TRUE)
set(COMPILER_SUPPORTS_SSE4_2 TRUE)
set(COMPILER_SUPPORTS_AVX TRUE)
set(COMPILER_SUPPORTS_AVX2 TRUE)
set(COMPILER_SUPPORTS_AVX512F TRUE)

# Pre-set system checks
set(__GLIBC__ FALSE)

# Pre-set header checks
set(HAVE_ALLOCA_H FALSE)
set(HAVE_FLOAT_H TRUE)
set(HAVE_ICONV_H TRUE)
set(HAVE_INTTYPES_H TRUE)
set(HAVE_LIMITS_H TRUE)
set(HAVE_MALLOC_H TRUE)
set(HAVE_MATH_H TRUE)
set(HAVE_MEMORY_H TRUE)
set(HAVE_SIGNAL_H TRUE)
set(HAVE_STDARG_H TRUE)
set(HAVE_STDBOOL_H TRUE)

# Configure symbol checks
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_REQUIRED_LIBRARIES "")
set(CMAKE_REQUIRED_QUIET TRUE)

# MinGW-GCC specific options
add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

# SDL3 Configuration - Force static building
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Force static libraries for everything
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SDL_SHARED OFF CACHE BOOL "Build SDL3 as shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL3 as static library" FORCE)
set(SDL3IMAGE_INSTALL OFF CACHE BOOL "Install SDL3_image" FORCE)
set(SDL3IMAGE_BUILD_SHARED_LIBS OFF CACHE BOOL "Build SDL3_image shared" FORCE)
set(SDL3IMAGE_VENDORED ON CACHE BOOL "Use vendored libraries in SDL3_image" FORCE)
set(SDL3IMAGE_AVIF OFF CACHE BOOL "Disable AVIF support" FORCE)
set(SDL3IMAGE_JXL OFF CACHE BOOL "Disable JXL support" FORCE)
set(SDL3IMAGE_WEBP OFF CACHE BOOL "Disable WEBP support" FORCE)

# Use vendored SDL3 sources
FetchContent_Declare(
    SDL3
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL
)

# Use vendored SDL_image sources
FetchContent_Declare(
    SDL3_image
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL_image
)

# Use vendored SDL_net sources
FetchContent_Declare(
    SDL3_net
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL_net
)

# Make SDL3 available
FetchContent_MakeAvailable(SDL3)

# Make SDL_image available after SDL3
FetchContent_MakeAvailable(SDL3_image)

# Make SDL_net available after SDL3
FetchContent_MakeAvailable(SDL3_net)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/minhook/include
    # Stock miniz lives in its own dir so its `#include "miniz.h"` resolves
    # without colliding with SDL_image's patched copy at vendored/SDL_image/src/.
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/miniz
)

# ImGui core sources (shared between launcher and hook)
set(IMGUI_CORE_SOURCES
    vendored/imgui/imgui.cpp
    vendored/imgui/imgui_demo.cpp
    vendored/imgui/imgui_draw.cpp
    vendored/imgui/imgui_tables.cpp
    vendored/imgui/imgui_widgets.cpp
)

# ImGui library for launcher (SDL backends)
set(IMGUI_SDL_SOURCES
    ${IMGUI_CORE_SOURCES}
    vendored/imgui/backends/imgui_impl_sdl3.cpp
    vendored/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

add_library(imgui_sdl STATIC ${IMGUI_SDL_SOURCES})
target_include_directories(imgui_sdl PUBLIC 
    vendored/imgui
    vendored/imgui/backends
)
target_link_libraries(imgui_sdl PUBLIC SDL3::SDL3-static)

# ImGui library for hook DLL (DirectX backends)
set(IMGUI_DX_SOURCES
    ${IMGUI_CORE_SOURCES}
    vendored/imgui/backends/imgui_impl_win32.cpp
    vendored/imgui/backends/imgui_impl_dx9.cpp
)

add_library(imgui_dx STATIC ${IMGUI_DX_SOURCES})
target_include_directories(imgui_dx PUBLIC 
    vendored/imgui
    vendored/imgui/backends
)
target_link_libraries(imgui_dx PUBLIC d3d9 dwmapi)

# GekkoNet library - Build as static
add_library(GekkoNet STATIC
    vendored/GekkoNet/GekkoLib/src/gekkonet.cpp
    vendored/GekkoNet/GekkoLib/src/backend.cpp
    vendored/GekkoNet/GekkoLib/src/event.cpp
    vendored/GekkoNet/GekkoLib/src/game_session.cpp
    vendored/GekkoNet/GekkoLib/src/spectator_session.cpp
    vendored/GekkoNet/GekkoLib/src/stress_session.cpp
    vendored/GekkoNet/GekkoLib/src/input.cpp
    vendored/GekkoNet/GekkoLib/src/net.cpp
    vendored/GekkoNet/GekkoLib/src/player.cpp
    vendored/GekkoNet/GekkoLib/src/storage.cpp
    vendored/GekkoNet/GekkoLib/src/sync.cpp
)

target_include_directories(GekkoNet PUBLIC
    vendored/GekkoNet/GekkoLib/include
    vendored/GekkoNet/GekkoLib/thirdparty
    vendored/GekkoNet/GekkoLib/thirdparty/zpp
    vendored/GekkoNet/GekkoLib/thirdparty/asio
)

# Windows-specific configuration for GekkoNet
if(WIN32)
    target_link_libraries(GekkoNet PUBLIC ws2_32)
    target_compile_definitions(GekkoNet PUBLIC
        _WIN32_WINNT=0x0601  # Target Windows 7 or later
        WIN32_LEAN_AND_MEAN  # Exclude rarely-used stuff from Windows headers
        GEKKONET_STATIC      # Static linking
    )
endif()

# MinHook library
set(MINHOOK_SOURCES
    vendored/minhook/src/buffer.c
    vendored/minhook/src/hook.c
    vendored/minhook/src/trampoline.c
    vendored/minhook/src/hde/hde32.c
    vendored/minhook/src/hde/hde64.c
)

add_library(MinHook STATIC ${MINHOOK_SOURCES})

# Ensure MinHook is built with MinGW
set_target_properties(MinHook PROPERTIES 
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(MinHook PRIVATE 
    -m32  # Force 32-bit compilation
)

target_include_directories(MinHook PUBLIC
    vendored/minhook/include
    vendored/minhook/src
)

# Windows-specific configuration for MinHook
target_compile_definitions(MinHook PRIVATE
    _WIN32_WINNT=0x0601  # Target Windows 7 or later
    WIN32_LEAN_AND_MEAN  # Exclude rarely-used stuff from Windows headers
)

# Add FM2KHook subdirectory
add_subdirectory(FM2KHook)

# Set FM2KHook output properties — both engines (FM2KHook + FM95Hook) ship
# alongside the launcher exe so it can pick the right DLL per game.engine.
set_target_properties(FM2KHook FM95Hook PROPERTIES
    PREFIX ""  # Remove "lib" prefix
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# Strip libwinpthread.a's `.rsrc` section so its VS_VERSION_INFO doesn't
# get merged into our final binaries alongside the one from launcher.rc /
# FM2KHook resource scripts. ld merges every input archive's RT_VERSION
# resource into the output PE; with two blocks present at resource ID 1
# Windows reads libwinpthread's first ("POSIX WinThreads for Windows")
# and that string leaks into Explorer details, taskbar tooltips, and
# Shell_NotifyIcon toast titles.
#
# Configure-time fix: `objcopy --remove-section=.rsrc` operates directly
# on a static archive, dropping the `.rsrc` section from every member
# object without touching any other section. We link --whole-archive
# against the cleaned copy so all libwinpthread symbols resolve from it,
# and the system -lwinpthread is then a no-op (no unresolved symbols
# left for it to satisfy → its `.rsrc` never gets pulled in).
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libwinpthread.a
    OUTPUT_VARIABLE SYS_LIBWINPTHREAD
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
get_filename_component(SYS_LIBWINPTHREAD "${SYS_LIBWINPTHREAD}" REALPATH)
set(CLEAN_LIBWINPTHREAD "${CMAKE_BINARY_DIR}/libwinpthread_no_rsrc.a")
add_custom_command(
    OUTPUT  "${CLEAN_LIBWINPTHREAD}"
    DEPENDS "${SYS_LIBWINPTHREAD}"
    COMMAND ${CMAKE_COMMAND} -E copy "${SYS_LIBWINPTHREAD}" "${CLEAN_LIBWINPTHREAD}"
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.rsrc "${CLEAN_LIBWINPTHREAD}"
    # `version.o` inside libwinpthread.a is *only* a .rsrc container —
    # stripping its sole section leaves an empty/invalid object that ld
    # rejects ("member ... is not an object"). It contributes only the
    # VERSIONINFO resource we're trying to suppress, so dropping the
    # member entirely from the archive is the right move. `ar d` is
    # silent on missing members so this is safe across libwinpthread
    # versions where the file might be named differently or absent.
    COMMAND ${CMAKE_AR} d "${CLEAN_LIBWINPTHREAD}" version.o || true
    COMMENT "Stripping .rsrc from libwinpthread.a -> ${CLEAN_LIBWINPTHREAD}"
    VERBATIM
)
add_custom_target(libwinpthread_no_rsrc DEPENDS "${CLEAN_LIBWINPTHREAD}")

# Add pthread static linking for FM2KHook + FM95Hook
target_link_options(FM2KHook PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)
add_dependencies(FM2KHook libwinpthread_no_rsrc)
target_link_options(FM95Hook PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)
add_dependencies(FM95Hook libwinpthread_no_rsrc)

# Main executable
# Pre-build version stamper. Writes version_local.h with the current
# semver code + git SHA + build timestamp. Re-runs every build so the
# binary always knows its own version (used by the auto-updater to
# decide if a published release is newer).
add_custom_target(fm2k_version
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_version.sh"
    BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/version_local.h"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Stamping version_local.h"
    VERBATIM
)

add_executable(FM2K_RollbackLauncher
    # launcher.rc must be FIRST so its compiled .res is the first
    # resource-bearing object on the link line. MinGW's libwinpthread.a
    # ships its own VS_VERSION_INFO ("POSIX WinThreads for Windows") and
    # ld merges every input's RT_VERSION resource into the final EXE.
    # Windows reads whichever block appears first in the resource
    # directory — putting ours first makes it win without surgery.
    launcher.rc
    # Main launcher files
    FM2K_RollbackClient.cpp
    FM2K_GameInstance.cpp
    OnlineSession.cpp
    LocalSession.cpp
    FM2K_LauncherUI.cpp
    FM2K_HubClient.cpp
    FM2K_DiscordAuth.cpp
    FM2K_Locale.cpp
    FM2K_Updater.cpp
    FM2K_GameIni.cpp
    FM2K_DDrawRedirect.cpp
    FM2K_CncDDraw.cpp
    FM2K_KgtParser.cpp
    # Stock miniz (public domain) for in-process zip extraction. SDL_image
    # ships its own statically-scoped copy with archive APIs disabled, so
    # this independent copy gives us mz_zip_reader_* without disturbing
    # the SDL_image build.
    vendored/miniz/miniz.c
    LocalNetworkAdapter.cpp
    FM2K_Integration.h
    # ImGui-based input binder (shared with the hook overlay) — depends only
    # on SDL3 + ImGui, no project-specific includes.
    FM2KHook/src/ui/input_binder.cpp
    # DLL injection approach - hooks are in FM2KHook DLL
)

# Static linking flags for MinGW-GCC
target_link_options(FM2K_RollbackLauncher PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    # Use the .rsrc-stripped libwinpthread (see config above) so its
    # VS_VERSION_INFO doesn't conflict with our launcher.rc one.
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)

# Add FM2KHook as dependency
add_dependencies(FM2K_RollbackLauncher FM2KHook fm2k_version libwinpthread_no_rsrc)

# FM2KUpdater.exe — companion process spawned by the launcher to
# perform the actual EXE/DLL replacement after launcher exit. Tiny
# binary, zero deps beyond Win32. See tools/FM2KUpdater.cpp.
add_executable(FM2KUpdater tools/FM2KUpdater.cpp)
target_link_options(FM2KUpdater PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
)
add_dependencies(FM2K_RollbackLauncher FM2KUpdater)

target_link_libraries(FM2K_RollbackLauncher PRIVATE
    GekkoNet
    SDL3::SDL3-static
    SDL3_image::SDL3_image-static
    SDL3_net::SDL3_net-static
    MinHook
    imgui_sdl
    psapi
    ws2_32
    winmm
    gdi32
    user32
    shell32
    ole32
    setupapi
    version
    imm32
    oleaut32
    wininet
    winhttp
    # XInput for input_binder's Sample_Win32 path. Compiled into the
    # launcher EXE too even though SDL3-Sample() is the default there;
    # unifying lets the launcher fall back if SDL3 events stop pumping.
    xinput9_1_0
)

# Preprocessor definitions
target_compile_definitions(FM2K_RollbackLauncher PRIVATE
    GEKKONET_STATIC
    SDL_STATIC_LIB
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Installation
install(TARGETS FM2K_RollbackLauncher
    RUNTIME DESTINATION bin
)

# Documentation
install(FILES
    README.md
    docs/FM2K_Rollback_Research.md
    docs/FM2K_Rollback_Plan.md
    docs/FM2K_Rollback_TestPlan.md
    docs/july7-2025.md
    DESTINATION docs
)

# Print configuration summary
message(STATUS "")
message(STATUS "FM2K Rollback Launcher Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Dependencies:")
message(STATUS "    - SDL3: ${SDL_VERSION}")
message(STATUS "    - SDL3_image: ${SDL3IMAGE_VERSION}")
message(STATUS "    - MinHook: Vendored")
message(STATUS "    - GekkoNet: Vendored")
message(STATUS "    - ImGui: Vendored")
message(STATUS "")