#include "globals.h"

// Original function pointers (set by MinHook)
GetPlayerInputFunc original_get_player_input = nullptr;
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;
RenderGameFunc original_render_game = nullptr;
GameRandFunc original_game_rand = nullptr;
ProcessGameInputsFunc original_process_game_inputs = nullptr;

// Minimal global state
int g_player_index = 0;
uint32_t g_frame_counter = 0;

// Network config (parsed at startup, used when entering battle)
bool g_offline_mode = false;
uint16_t g_local_port = 7000;
char g_remote_addr[64] = "127.0.0.1:7001";
