#pragma once
// Shared mode/atomic state for the split per_game_patches TUs. The mode flags
// are written by PerGamePatches_ApplyRuntime (battle TU) + the title/frame
// tick (input TU) and read by the input-override + AI-field paths, so they
// must be visible across the FM2K-side TUs. Definitions live in
// per_game_patches.cpp (the input/modes TU). FM2K-only -- the FM95 stub TU
// doesn't include this.
#include <atomic>
#include <cstdint>

extern std::atomic<bool>     g_vs_cpu_mode;
extern std::atomic<bool>     g_cpu_vs_cpu_mode;
extern std::atomic<bool>     g_training_mode;
extern std::atomic<bool>     g_option_mode_selector;
extern std::atomic<int>      g_training_p2_behavior;
extern std::atomic<bool>     g_option_was_pressed;
extern std::atomic<bool>     g_f2_was_pressed;
extern std::atomic<uint32_t> g_prev_game_mode;
extern std::atomic<int>      g_team_size_override;

// Address constants shared between the input/modes TU and the battle TU.
inline constexpr uintptr_t ADDR_GAME_MODE         = 0x00470054;  // current game_mode
inline constexpr uintptr_t STORY_INIT_HIJACK_SITE = 0x00411C8F;  // cmp eax, ebp
