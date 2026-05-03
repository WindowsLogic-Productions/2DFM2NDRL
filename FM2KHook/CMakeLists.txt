cmake_minimum_required(VERSION 3.15)

# FM2K Hook DLL - must be 32-bit
project(FM2KHook)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Enable debug symbols
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# ORGANIZED SOURCE FILES
add_library(FM2KHook SHARED
    # Core
    src/core/dllmain.cpp
    src/core/globals.cpp
    src/core/main_loop_trampoline.cpp
    # Hooks
    src/hooks/hooks.cpp
    src/hooks/input.cpp
    src/hooks/game_patches.cpp
    # Netplay
    src/netplay/netplay.cpp
    src/netplay/control_channel.cpp
    src/netplay/savestate.cpp
    src/netplay/replay.cpp
    src/netplay/spectator_node.cpp
    src/netplay/nat_traversal.cpp
    # Audio — Mike Z rollback-safe sound layer
    src/audio/sound_rollback.cpp
    # UI
    src/ui/imgui_overlay.cpp
    src/ui/shared_mem.cpp
    src/ui/input_binder.cpp
    # Parity — kgtengine cross-engine ground-truth recorder
    src/parity/parity_recorder.cpp
)

# Export symbols for DLL
target_compile_definitions(FM2KHook PRIVATE
    FM2KHOOK_EXPORTS
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    GEKKONET_STATIC
    _WIN32_WINNT=0x0601
)

# Link against dependencies
target_link_libraries(FM2KHook PRIVATE
    MinHook
    GekkoNet
    SDL3::SDL3-static
    SDL3_net::SDL3_net-static
    imgui_dx
    ws2_32
    winmm
    d3d9
    # XInput for the input binder's Win32 polling path. Use xinput9_1_0
    # (ships with Win7+ in-box) to avoid the redist requirement of the
    # current xinput1_4.
    xinput9_1_0
)

# Include directories - add all src subdirectories so includes work
target_include_directories(FM2KHook PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks
    ${CMAKE_CURRENT_SOURCE_DIR}/src/netplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/vendored/minhook/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL_net/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/GekkoLib/include
    ${CMAKE_SOURCE_DIR}/vendored/imgui
    # kgtengine parity-snapshot header (sibling repo at /mnt/c/dev/kgtengine).
    # FM2KHook/CMakeLists.txt is at /mnt/c/dev/wanwan/FM2KHook/, so go up two
    # levels (../..) to reach /mnt/c/dev/, then into kgtengine/include.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kgtengine/include
)

# Set output directories
set_target_properties(FM2KHook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Install rules
install(TARGETS FM2KHook
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
