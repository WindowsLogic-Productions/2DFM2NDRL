#include "globals.h"

// Original function pointers (set by MinHook)
GetPlayerInputFunc original_get_player_input = nullptr;
GetPlayerInputFM95Func original_get_player_input_p1 = nullptr;  // FM95 only
GetPlayerInputFM95Func original_get_player_input_p2 = nullptr;  // FM95 only
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;
RenderGameFunc original_render_game = nullptr;
GameRandFunc original_game_rand = nullptr;
ProcessGameInputsFunc original_process_game_inputs = nullptr;

// Minimal global state
int g_player_index = 0;
uint32_t g_frame_counter = 0;
bool g_is_rolling_back = false;

// Network config (parsed at startup, used when entering battle)
bool g_offline_mode = false;
uint16_t g_local_port = 7000;
char g_remote_addr[64] = "127.0.0.1:7001";

// Stress-test mode (FM2K_STRESS_MODE=1): single-instance determinism test
bool g_stress_mode = false;

// Spectator mode (FM2K_SPECTATOR_MODE=1): passive viewer.
bool g_spectator_mode = false;

// FM95 host-driven trampoline: when Hook_UpdateGameState runs the
// trampoline tick on the FM95 build, it sets this to tell the host's
// natural render_game call (caught by Hook_RenderGame) to skip its body
// — the trampoline already drove RenderFrameWithSnapshot inside the
// tick, calling original_render_game once. Without this, FM95 renders
// twice per frame: once via RenderFrameWithSnapshot, once via the
// host's natural render_game pump. Cleared at the top of Hook_RenderGame
// after the skip fires.
bool g_fm95_skip_next_render = false;
