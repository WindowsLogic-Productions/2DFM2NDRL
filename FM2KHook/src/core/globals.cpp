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

// Render RNG stream (see globals.h). Re-seeded from the gameplay seed each
// render; advanced only by render-side game_rand draws via Hook_GameRand.
uint32_t g_render_rng_seed = 0;
bool     g_in_render_rng   = false;
// Diagnostic counter (#63): render-side game_rand calls, to test whether the
// Robot Heroes heavy-stage render cost is our Hook_GameRand overhead scaling
// with per-frame rng draws. Reset per offline frame by the trampoline.
uint32_t g_render_rand_calls = 0;
volatile uint32_t g_sim_step_count = 0;

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

// Spectator fast catch-up flag (C5.5). Set by RunSpectatorTick's inner
// loop while burning through queued events; cleared once pb_queue depth
// drops below LIVE_LAG_FRAMES.
bool g_spectator_catchup = false;

// User-toggled FF (F12 in spectator window). Persistent across
// catchup-loop iterations; flipped by WndProc subclass on key press.
bool g_spectator_ff_user = false;

// FM95 host-driven trampoline: when Hook_UpdateGameState runs the
// trampoline tick on the FM95 build, it sets this to tell the host's
// natural render_game call (caught by Hook_RenderGame) to skip its body
// — the trampoline already drove RenderFrameWithSnapshot inside the
// tick, calling original_render_game once. Without this, FM95 renders
// twice per frame: once via RenderFrameWithSnapshot, once via the
// host's natural render_game pump. Cleared at the top of Hook_RenderGame
// after the skip fires.
bool g_fm95_skip_next_render = false;
