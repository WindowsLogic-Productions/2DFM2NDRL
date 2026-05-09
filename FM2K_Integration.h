#pragma once

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "MinHook.h"
#include "ISession.h"
#include "FM2K_CncDDraw.h"   // IniConfig used as a member of LauncherUI
#include "FM2K_KgtParser.h"  // KgtSummary stored on FM2KGameInfo

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <unordered_map>

// Forward declarations
class FM2KGameInstance;
class LauncherUI;

// FM2K Memory addresses and structures (from research)
namespace FM2K {
    // Forward declarations for FM2K namespace classes
    class GekkoNetBridge;
    
    // Memory addresses for critical game state
    constexpr uintptr_t INPUT_BUFFER_INDEX_ADDR = 0x470000;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    
    // Player state addresses (working addresses for this game version)
    constexpr uintptr_t P1_INPUT_ADDR = 0x470100;          // Current input state (working)
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x470104;
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x470108;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;              // Current HP (verified in combat)
    constexpr uintptr_t P1_MAX_HP_ADDR = 0x470110;
    constexpr uintptr_t P1_INPUT_HISTORY_ADDR = 0x470200;  // Input history buffer
    
    constexpr uintptr_t P2_INPUT_ADDR = 0x470300;          // Current input state (working)
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;              // Current HP (verified in combat)
    constexpr uintptr_t P2_MAX_HP_ADDR = 0x470310;
    constexpr uintptr_t P2_INPUT_HISTORY_ADDR = 0x470400;
    
    // ArtMoney verified addresses (tested during live combat)
    // Player coordinates (stage positions)
    constexpr uintptr_t P1_COORD_X_ADDR = 0x4ADCC3;        // P1 X coordinate (u32)
    constexpr uintptr_t P1_COORD_Y_ADDR = 0x4ADCC7;        // P1 Y coordinate (u16)
    constexpr uintptr_t P2_COORD_X_ADDR = 0x4EDD02;        // P2 X coordinate (u32)
    constexpr uintptr_t P2_COORD_Y_ADDR = 0x4EDD06;        // P2 Y coordinate (u16)
    
    // Player max HP (character-specific values)
    constexpr uintptr_t P1_MAX_HP_ARTMONEY_ADDR = 0x4DFC85; // P1 max HP (u32) - verified: 800
    constexpr uintptr_t P2_MAX_HP_ARTMONEY_ADDR = 0x4EDC4;  // P2 max HP (u32) - needs validation
    
    // Super meter system
    constexpr uintptr_t P1_SUPER_ADDR = 0x4EDC3D;          // P1 super meter (u32)
    constexpr uintptr_t P2_SUPER_ADDR = 0x4EDC0C;          // P2 super meter (u32)
    constexpr uintptr_t P1_SPECIAL_STOCK_ADDR = 0x4EDDC95; // P1 special stock (u32) - needs validation
    constexpr uintptr_t P2_SPECIAL_STOCK_ADDR = 0x4EDC4;   // P2 special stock (u32) - same as max HP?
    
    // Map/stage coordinates
    constexpr uintptr_t MAP_X_COORD_ADDR = 0x44742C;       // Map X coordinate (u32)
    constexpr uintptr_t MAP_Y_COORD_ADDR = 0x447F30;       // Map Y coordinate (u32) - needs validation
    
    // Global state addresses
    constexpr uintptr_t ROUND_TIMER_ADDR = 0x470044;       // Primary timer (verified)
    constexpr uintptr_t ROUND_TIMER_ALT_ADDR = 0x47DB94;   // ArtMoney timer (verified: same value)
    constexpr uintptr_t GAME_TIMER_ADDR = 0x470060;
    constexpr uintptr_t ROUND_NUMBER_ADDR = 0x4070044;     // Round number (u32)
    
    // ======= CHARACTER SELECT ADDRESSES (CCCaster-style) =======
    // Core character selection state (similar to CC_P1/P2_CHARACTER_ADDR)
    constexpr uintptr_t P1_CHARACTER_ID_ADDR = 0x470180;    // P1 selected character ID (u32)
    constexpr uintptr_t P2_CHARACTER_ID_ADDR = 0x470184;    // P2 selected character ID (u32)
    constexpr uintptr_t SELECTED_STAGE_ADDR = 0x43010c;     // Selected stage ID (u32) — IDA-verified WW: vs_round_function reads this as wParam, settings_dialog_proc writes CB_GETCURSEL into it
    
    // Character grid cursor positions (similar to CC_P1/P2_CHARA_SELECTOR_ADDR)
    constexpr uintptr_t P1_CURSOR_X_ADDR = 0x47018C;        // P1 character grid cursor X (u32)
    constexpr uintptr_t P1_CURSOR_Y_ADDR = 0x470190;        // P1 character grid cursor Y (u32)
    constexpr uintptr_t P2_CURSOR_X_ADDR = 0x470194;        // P2 character grid cursor X (u32)
    constexpr uintptr_t P2_CURSOR_Y_ADDR = 0x470198;        // P2 character grid cursor Y (u32)
    
    // Character variations and colors (similar to CC_P1/P2_MOON_SELECTOR_ADDR, CC_P1/P2_COLOR_SELECTOR_ADDR)
    constexpr uintptr_t P1_VARIANT_ADDR = 0x47019C;         // P1 character variant/style (u32) - also confirms P1
    constexpr uintptr_t P2_VARIANT_ADDR = 0x4701A0;         // P2 character variant/style (u32) - also confirms P2
    constexpr uintptr_t P1_COLOR_ADDR = 0x4701A4;           // P1 color palette selection (u32)
    constexpr uintptr_t P2_COLOR_ADDR = 0x4701A8;           // P2 color palette selection (u32)
    
    // Selection mode tracking (similar to CC_P1/P2_SELECTOR_MODE_ADDR)
    constexpr uintptr_t P1_SELECTION_MODE_ADDR = 0x4701AC;  // P1 selection mode (0=selecting, 1=confirmed, 2=ready) (u32)
    constexpr uintptr_t P2_SELECTION_MODE_ADDR = 0x4701B0;  // P2 selection mode (0=selecting, 1=confirmed, 2=ready) (u32)
    
    // CSS timing and validation
    constexpr uintptr_t CSS_FRAME_COUNTER_ADDR = 0x4701B4;  // Frames since entering CSS (u32)
    constexpr uintptr_t CSS_STATE_FLAGS_ADDR = 0x4701B8;    // CSS state flags (lockout, timing validation) (u32)
    
    // Character select confirmation addresses (verified working addresses)
    constexpr uintptr_t P1_CONFIRMED_ADDR = 0x47019C;       // P1 confirmation status (1 = confirmed) (same as variant for FM2K)
    constexpr uintptr_t P2_CONFIRMED_ADDR = 0x4701A0;       // P2 confirmation status (1 = confirmed) (same as variant for FM2K)
    
    // ======= CHARACTER SELECT MEMORY ACCESS NAMESPACE =======
    namespace CharSelect {
        // Core selection addresses (CCCaster-style access)
        namespace Memory {
            // Character selection data
            constexpr uintptr_t P1_CHARACTER_ADDR = P1_CHARACTER_ID_ADDR;
            constexpr uintptr_t P2_CHARACTER_ADDR = P2_CHARACTER_ID_ADDR;
            constexpr uintptr_t STAGE_SELECTOR_ADDR = SELECTED_STAGE_ADDR;
            
            // Cursor positions (character grid navigation)
            constexpr uintptr_t P1_CURSOR_X_ADDR = FM2K::P1_CURSOR_X_ADDR;
            constexpr uintptr_t P1_CURSOR_Y_ADDR = FM2K::P1_CURSOR_Y_ADDR;
            constexpr uintptr_t P2_CURSOR_X_ADDR = FM2K::P2_CURSOR_X_ADDR;
            constexpr uintptr_t P2_CURSOR_Y_ADDR = FM2K::P2_CURSOR_Y_ADDR;
            
            // Character variations and colors
            constexpr uintptr_t P1_VARIANT_SELECTOR_ADDR = P1_VARIANT_ADDR;
            constexpr uintptr_t P2_VARIANT_SELECTOR_ADDR = P2_VARIANT_ADDR;
            constexpr uintptr_t P1_COLOR_SELECTOR_ADDR = P1_COLOR_ADDR;
            constexpr uintptr_t P2_COLOR_SELECTOR_ADDR = P2_COLOR_ADDR;
            
            // Selection mode tracking 
            constexpr uintptr_t P1_SELECTOR_MODE_ADDR = P1_SELECTION_MODE_ADDR;
            constexpr uintptr_t P2_SELECTOR_MODE_ADDR = P2_SELECTION_MODE_ADDR;
            
            // Confirmation status (CCCaster compatibility)
            constexpr uintptr_t P1_CONFIRMED_STATUS_ADDR = P1_CONFIRMED_ADDR;
            constexpr uintptr_t P2_CONFIRMED_STATUS_ADDR = P2_CONFIRMED_ADDR;
            
            // Timing and validation
            constexpr uintptr_t CSS_FRAME_COUNTER_ADDR = FM2K::CSS_FRAME_COUNTER_ADDR;
            constexpr uintptr_t CSS_STATE_FLAGS_ADDR = FM2K::CSS_STATE_FLAGS_ADDR;
        }
        
        // Character select constants (similar to CCCaster's CC_SELECT_CHARA)
        namespace Constants {
            constexpr uint32_t SELECT_CHARA = 0;        // Player is selecting character
            constexpr uint32_t CHARA_CONFIRMED = 1;     // Player has confirmed character
            constexpr uint32_t FULLY_READY = 2;         // Player is ready for battle
            
            // Input validation timing (CCCaster-style)
            constexpr uint32_t CSS_LOCKOUT_FRAMES = 150;        // Frames to wait before allowing confirm (prevents moon selector desync)
            constexpr uint32_t MODE_CHANGE_LOCKOUT = 2;         // Frames to wait after selector mode change
            constexpr uint32_t BUTTON_HISTORY_FRAMES = 3;       // Frames to check for button conflicts
        }
    }
    
    // Character variables (16 variables per player - verified addresses)
    // Player 1 character variables (1-byte each)
    constexpr uintptr_t P1_CHAR_VAR_A_ADDR = 0x4ADFD17;   // Char Var A (u8)
    constexpr uintptr_t P1_CHAR_VAR_B_ADDR = 0x4ADFD19;   // Char Var B (u8)
    constexpr uintptr_t P1_CHAR_VAR_C_ADDR = 0x4ADFD1B;   // Char Var C (u8)
    constexpr uintptr_t P1_CHAR_VAR_D_ADDR = 0x4ADFD1D;   // Char Var D (u8)
    constexpr uintptr_t P1_CHAR_VAR_E_ADDR = 0x4ADFD1F;   // Char Var E (u8)
    constexpr uintptr_t P1_CHAR_VAR_F_ADDR = 0x4ADFD21;   // Char Var F (u8)
    constexpr uintptr_t P1_CHAR_VAR_G_ADDR = 0x4ADFD23;   // Char Var G (u8)
    constexpr uintptr_t P1_CHAR_VAR_H_ADDR = 0x4ADFD25;   // Char Var H (u8)
    constexpr uintptr_t P1_CHAR_VAR_I_ADDR = 0x4ADFD27;   // Char Var I (u8)
    constexpr uintptr_t P1_CHAR_VAR_J_ADDR = 0x4ADFD29;   // Char Var J (u8)
    constexpr uintptr_t P1_CHAR_VAR_K_ADDR = 0x4ADFD2B;   // Char Var K (u8)
    constexpr uintptr_t P1_CHAR_VAR_L_ADDR = 0x4ADFD2D;   // Char Var L (u8)
    constexpr uintptr_t P1_CHAR_VAR_M_ADDR = 0x4ADFD2F;   // Char Var M (u8)
    constexpr uintptr_t P1_CHAR_VAR_N_ADDR = 0x4ADFD31;   // Char Var N (u8)
    constexpr uintptr_t P1_CHAR_VAR_O_ADDR = 0x4ADFD33;   // Char Var O (u8)
    constexpr uintptr_t P1_CHAR_VAR_P_ADDR = 0x4ADFD35;   // Char Var P (u8)
    
    // Player 2 character variables (2-byte each) 
    constexpr uintptr_t P2_CHAR_VAR_A_ADDR = 0x4ADFD5;    // Char Var A P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_B_ADDR = 0x4EDD58;    // Char Var B P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_C_ADDR = 0x4EDD5A;    // Char Var C P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_D_ADDR = 0x4EDDC;     // Char Var D P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_F_ADDR = 0x4EDD60;    // Char Var F P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_G_ADDR = 0x4EDD62;    // Char Var G P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_H_ADDR = 0x4EDD64;    // Char Var H P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_I_ADDR = 0x4ADFD6;    // Char Var I P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_J_ADDR = 0x4ADFD8;    // Char Var J P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_K_ADDR = 0x4EDD6A;    // Char Var K P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_L_ADDR = 0x4EDDC6;    // Char Var L P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_M_ADDR = 0x4EDD6E;    // Char Var M P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_N_ADDR = 0x4ADFD0;    // Char Var N P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_O_ADDR = 0x4ADFD7;    // Char Var O P2 (u16)
    constexpr uintptr_t P2_CHAR_VAR_P_ADDR = 0x4EDD74;    // Char Var P P2 (u16)
    
    // System variables (global game state variables)
    constexpr uintptr_t SYSTEM_VAR_A_ADDR = 0x4456B0;     // System Var A (u8)
    constexpr uintptr_t SYSTEM_VAR_B_ADDR = 0x4456B2;     // System Var B (u8)
    constexpr uintptr_t SYSTEM_VAR_D_ADDR = 0x4456B6;     // System Var D (u8)
    constexpr uintptr_t SYSTEM_VAR_E_ADDR = 0x4456B8;     // System Var E (u8)
    constexpr uintptr_t SYSTEM_VAR_F_ADDR = 0x4456BA;     // System Var F (u8)
    constexpr uintptr_t SYSTEM_VAR_G_ADDR = 0x4456BC;     // System Var G (u8)
    constexpr uintptr_t SYSTEM_VAR_H_ADDR = 0x4456BE;     // System Var H (u16)
    constexpr uintptr_t SYSTEM_VAR_I_ADDR = 0x4456C0;     // System Var I (u16)
    constexpr uintptr_t SYSTEM_VAR_J_ADDR = 0x456C2;      // System Var J (u16)
    constexpr uintptr_t SYSTEM_VAR_K_ADDR = 0x456C4;      // System Var K (u8)
    constexpr uintptr_t SYSTEM_VAR_L_ADDR = 0x456C6;      // System Var L (u8)
    constexpr uintptr_t SYSTEM_VAR_M_ADDR = 0x456C8;      // System Var M (u8)
    constexpr uintptr_t SYSTEM_VAR_N_ADDR = 0x456CA;      // System Var N (u8)
    constexpr uintptr_t SYSTEM_VAR_O_ADDR = 0x456CC;      // System Var O (u8)
    constexpr uintptr_t SYSTEM_VAR_P_ADDR = 0x456CE;      // System Var P (u16)

    // Sprite effect system addresses
    constexpr uintptr_t EFFECT_ACTIVE_FLAGS = 0x40CC30;  // Bitfield of active effects
    constexpr uintptr_t EFFECT_TIMERS_BASE = 0x40CC34;   // Array of 8 effect timers
    constexpr uintptr_t EFFECT_COLORS_BASE = 0x40CC54;   // Array of 8 RGB color sets
    constexpr uintptr_t EFFECT_TARGETS_BASE = 0x40CCD4;  // Array of 8 target IDs
    
    // Game State
    constexpr DWORD OBJECT_POOL_ADDR = 0x4701E0;
    
    // Hook Points
    constexpr DWORD FRAME_HOOK_ADDR = 0x4146D0;
    constexpr DWORD UPDATE_GAME_STATE_ADDR = 0x404CD0;
    
    // Engine variant — selects address tables / hook strategy / launch flow.
    // FM2K (Fighter Maker 2nd) is the default supported engine. FM95 is the
    // earlier prototype (e.g. CPW.exe — Comic Party Wars). They share the
    // 256-slot object pool and 11-bit input format but differ in:
    //   - g_game_mode encoding (FM2K uses 2000/3000 magic; FM95 stays 0/1)
    //   - frame loop shape (FM2K has RUN_GAME_LOOP; FM95 inlines into WinMain)
    //   - CSS / battle phase classification (FM95 has no global mode flag)
    // See FM95_Integration.h for FM95 address tables and CharSelect:: helpers.
    enum class Engine : uint32_t {
        FM2K = 0,  // default — Fighter Maker 2nd (most modern 2DFM titles)
        FM95 = 1,  // prototype — Fighter Maker 95 (CPW.exe, early 2002 builds)
    };

    inline const char* EngineName(Engine e) {
        switch (e) {
            case Engine::FM2K: return "FM2K";
            case Engine::FM95: return "FM95";
            default:           return "unknown";
        }
    }

    // Game instance info
    struct FM2KGameInfo {
        std::string exe_path;
        std::string dll_path;
        uint32_t process_id;
        bool is_host;
        Engine engine = Engine::FM2K;  // detected at discovery time

        // Identification state, set by discovery:
        //   - is_clean   : exe hash matched a known-clean entry in the registry.
        //   - packer_label: non-empty => detected a real packer (Enigma /
        //                   UPX / MoleBox / etc) by sniffing PE section names.
        //                   This is what we *actually* warn about in the UI.
        //   - clean_label: friendly title when is_clean (e.g. "WonderfulWorld v0.946").
        //   - xxh64      : computed once at discovery, reused for cache + UI.
        //
        // Three render states drive from these:
        //   is_clean=true                       -> "<exe>  [<engine> — <label>]"           normal
        //   packer_label non-empty              -> "* <exe>  [<engine> — packed: <name>]"  yellow
        //   else (untested, no packer detected) -> "<exe>  [<engine> — untested]"          normal
        bool        is_clean = false;
        std::string clean_label;
        std::string packer_label;
        uint64_t    xxh64    = 0;

        // Parsed .kgt summary — player/stage/demo name lists. Populated at
        // discovery time so the UI can populate dropdowns pre-launch without
        // having to boot the game once and ReadProcessMemory the in-memory
        // buffers. `kgt.valid == false` means parse failed or no .kgt was
        // present (FM95 .player-only directories).
        fm2k::KgtSummary kgt;

        // Helper to get just the filename from the full path
        std::string GetExeName() const {
            const char* path = exe_path.c_str();
            const char* last_slash = SDL_strrchr(path, '/');
            if (!last_slash) last_slash = SDL_strrchr(path, '\\');
            return last_slash ? std::string(last_slash + 1) : exe_path;
        }

        // Helper to get the directory containing the exe
        std::string GetExeDir() const {
            const char* path = exe_path.c_str();
            char* dir = SDL_strdup(path);
            if (!dir) return "";

            char* last_slash = SDL_strrchr(dir, '/');
            if (!last_slash) last_slash = SDL_strrchr(dir, '\\');
            if (last_slash) {
                *last_slash = '\0';
                std::string result(dir);
                SDL_free(dir);
                return result;
            }
            SDL_free(dir);
            return "";
        }
    };

    // Utility functions
    bool FileExists(const std::string& path);
    uint32_t Fletcher32(const uint16_t* data, size_t len);
    float GetFM2KFrameTime(float frames_ahead);
    std::chrono::milliseconds GetFrameDuration();

    struct GameState {
        // Frame and timing state
        uint32_t frame_number;
        uint32_t input_buffer_index;
        uint32_t last_frame_time;
        uint32_t frame_time_delta;
        uint8_t frame_skip_count;
        uint8_t frame_sync_flag;
        
        // Random number generator state
        uint32_t random_seed;
        
        // Player states
        struct PlayerState {
            uint32_t input_current;
            uint32_t input_history[1024];
            uint32_t stage_x, stage_y;
            uint32_t hp, max_hp;
            uint32_t meter, max_meter;
            uint32_t combo_counter;
            uint32_t hitstun_timer;
            uint32_t blockstun_timer;
            uint32_t anim_timer;
            uint32_t move_id;
            uint32_t state_flags;
        } players[2];

        // Visual effects state (simplified - no IPC)
        uint32_t visual_state_flags;

        // Hit detection tables
        struct HitBox {
            int32_t x, y, w, h;
            uint32_t type;
            uint32_t damage;
            uint32_t flags;
        };
        
        static constexpr size_t MAX_HITBOXES = 32;
        HitBox hit_boxes[MAX_HITBOXES];
        uint32_t hit_box_count;

        // Calculate state checksum for rollback verification
        uint32_t CalculateChecksum() const {
            return Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(GameState));
        }
    };

    // Minimal game state for GekkoNet rollback testing (48 bytes)
    // Contains only essential combat state to test desync detection
    struct MinimalGameState {
        // Core combat state (32 bytes)
        uint32_t p1_hp, p2_hp;              // Current HP (0x47010C, 0x47030C)
        uint32_t p1_max_hp, p2_max_hp;      // Max HP (0x4DFC85, 0x4EDC4)
        uint32_t p1_x, p1_y;                // Positions (0x4ADCC3, 0x4ADCC7)
        uint32_t p2_x, p2_y;                // Positions (0x4EDD02, 0x4EDD06)
        
        // Essential timers & RNG (16 bytes)
        uint32_t round_timer;                // 0x470044 or 0x47DB94
        uint32_t random_seed;                // 0x41FB1C
        uint32_t frame_number;               // Current frame
        uint32_t input_checksum;             // XOR of recent inputs
        
        // Calculate minimal state checksum
        uint32_t CalculateChecksum() const {
            return Fletcher32(reinterpret_cast<const uint16_t*>(this), sizeof(MinimalGameState));
        }
        
        // Load minimal state from memory addresses
        bool LoadFromMemory() {
            // Read HP values
            uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
            uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
            uint32_t* p1_max_hp_ptr = (uint32_t*)P1_MAX_HP_ARTMONEY_ADDR;
            uint32_t* p2_max_hp_ptr = (uint32_t*)P2_MAX_HP_ARTMONEY_ADDR;
            
            if (!p1_hp_ptr || !p2_hp_ptr || !p1_max_hp_ptr || !p2_max_hp_ptr) return false;
            
            p1_hp = *p1_hp_ptr;
            p2_hp = *p2_hp_ptr;
            p1_max_hp = *p1_max_hp_ptr;
            p2_max_hp = *p2_max_hp_ptr;
            
            // Read positions
            uint32_t* p1_x_ptr = (uint32_t*)P1_COORD_X_ADDR;
            uint16_t* p1_y_ptr = (uint16_t*)P1_COORD_Y_ADDR;
            uint32_t* p2_x_ptr = (uint32_t*)P2_COORD_X_ADDR;
            uint16_t* p2_y_ptr = (uint16_t*)P2_COORD_Y_ADDR;
            
            if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
            
            p1_x = *p1_x_ptr;
            p1_y = *p1_y_ptr;
            p2_x = *p2_x_ptr;
            p2_y = *p2_y_ptr;
            
            // Read timers and RNG
            uint32_t* timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
            uint32_t* rng_ptr = (uint32_t*)RANDOM_SEED_ADDR;
            
            if (!timer_ptr || !rng_ptr) return false;
            
            round_timer = *timer_ptr;
            random_seed = *rng_ptr;
            
            return true;
        }
        
        // Save minimal state to memory addresses
        bool SaveToMemory() const {
            // Write HP values
            uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
            uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
            
            if (!p1_hp_ptr || !p2_hp_ptr) return false;
            
            *p1_hp_ptr = p1_hp;
            *p2_hp_ptr = p2_hp;
            
            // Write positions
            uint32_t* p1_x_ptr = (uint32_t*)P1_COORD_X_ADDR;
            uint16_t* p1_y_ptr = (uint16_t*)P1_COORD_Y_ADDR;
            uint32_t* p2_x_ptr = (uint32_t*)P2_COORD_X_ADDR;
            uint16_t* p2_y_ptr = (uint16_t*)P2_COORD_Y_ADDR;
            
            if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
            
            *p1_x_ptr = p1_x;
            *p1_y_ptr = (uint16_t)p1_y;
            *p2_x_ptr = p2_x;
            *p2_y_ptr = (uint16_t)p2_y;
            
            // Write timers and RNG
            uint32_t* timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
            uint32_t* rng_ptr = (uint32_t*)RANDOM_SEED_ADDR;
            
            if (!timer_ptr || !rng_ptr) return false;
            
            *timer_ptr = round_timer;
            *rng_ptr = random_seed;
            
            return true;
        }
    };

    // Input structure (11-bit input mask)
    struct Input {
        union {
            struct {
                uint16_t left     : 1;
                uint16_t right    : 1;
                uint16_t up       : 1;
                uint16_t down     : 1;
                uint16_t button1  : 1;
                uint16_t button2  : 1;
                uint16_t button3  : 1;
                uint16_t button4  : 1;
                uint16_t button5  : 1;
                uint16_t button6  : 1;
                uint16_t button7  : 1;
                uint16_t reserved : 5;
            } bits;
            uint16_t value;
        };
    };

    // Memory access functions
    template<typename T>
    bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
        SIZE_T bytes_read;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), 
            &value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
    }

    template<typename T>
    bool WriteMemory(HANDLE process, uintptr_t address, const T& value) {
        SIZE_T bytes_written;
        return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address),
            &value, sizeof(T), &bytes_written) && bytes_written == sizeof(T);
    }
}

// Launcher states
enum class LauncherState {
    GameSelection,      // Selecting which FM2K game to launch
    Connecting,         // Establishing network connection
    InGame,            // Game running with rollback active
    Disconnected       // Connection lost, can reconnect
};

// Network configuration
struct NetworkConfig {
    SessionMode session_mode;
    int local_port;
    std::string remote_address;
    int input_delay;
    bool is_host; // True if hosting, false if joining

    NetworkConfig() 
        : session_mode(SessionMode::LOCAL)  // Default to LOCAL for testing
        , local_port(7000)
        , remote_address("127.0.0.1:7001")
        , input_delay(2)
        , is_host(false)
    {
        // Use SDL string functions for initialization
        char remote_addr[32];
        SDL_strlcpy(remote_addr, "127.0.0.1:7001", sizeof(remote_addr));
        remote_address = remote_addr;
    }
};

// Rollback performance statistics (standalone, used by both launcher and UI)
struct RollbackStats {
    uint32_t rollbacks_per_second; // Current rollback frequency
    uint32_t max_rollback_frames;  // Maximum rollback distance
    uint32_t avg_rollback_frames;  // Average rollback distance
    float frame_advantage;         // Current frame advantage
    uint32_t input_delay_frames;   // Current input delay
    uint32_t confirmed_frames;     // Number of confirmed frames
    uint32_t speculative_frames;   // Number of speculative frames
};

// Forward declaration
class LauncherUI;

// Main launcher class
class FM2KLauncher {
public:
    FM2KLauncher();
    ~FM2KLauncher();
    
    bool Initialize();
    void Shutdown();
    void Update(float delta_time);
    void Render();
    void HandleEvent(SDL_Event* event);
    
    bool LaunchGame(const FM2K::FM2KGameInfo& game);
    void TerminateGame();

    void StartOfflineSession();
    void StartOnlineSession(const NetworkConfig& config, bool is_host);
    void StartStressSession();  // GekkoStressSession determinism test (single instance)
    void StopSession();
    
    std::vector<FM2K::FM2KGameInfo> DiscoverGames();
    const std::vector<FM2K::FM2KGameInfo>& GetDiscoveredGames() const { return discovered_games_; }

    // Resolve a hub-style game_id (exe stem, e.g. "WonderfulWorld_ver_0946")
    // to its parsed .kgt summary. Returns nullptr if the game isn't
    // installed locally or its KGT failed to parse — UI callers should
    // pass the result straight into fm2k::FormatCharLabel /
    // FormatStageLabel which fall back to "Char #N" / "Stage #N".
    const fm2k::KgtSummary* FindKgtByGameId(const std::string& game_id) const;
    
    void SetState(LauncherState state);
    bool IsRunning() const { return running_; }
    void SetRunning(bool running) { running_ = running; }
    
    // Games directory management
    const std::vector<std::string>& GetGamesRootPaths() const { return games_root_paths_; }
    void SetGamesRootPaths(const std::vector<std::string>& paths);
    void SetSelectedGame(const FM2K::FM2KGameInfo& game);
    
    // ----- Asynchronous game discovery -----
    SDL_Thread* discovery_thread_ = nullptr; // Worker thread handle
    bool discovery_in_progress_ = false;     // Flag so we don't launch multiple scans

    // Starts a background SDL thread that will run DiscoverGames() and notify the main
    // thread when done. Implemented in FM2K_RollbackClient.cpp.
    //
    // `show_spinner` toggles the UI's "Scanning for games…" indicator. Pass
    // false when the cache already populated the games list — the user
    // shouldn't see a spinner if the displayed list is already correct;
    // the background walk is just an "anything new?" check at that point.
    void StartAsyncDiscovery(bool show_spinner = true);
    
    // Scan progress accessors for UI
    void SetScanning(bool scanning);

    SDL_Window* GetWindow() const { return window_; }

private:
    bool InitializeSDL();
    bool InitializeImGui();
    
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::vector<FM2K::FM2KGameInfo> discovered_games_;
    
    // Multi-client testing instances
    std::unique_ptr<FM2KGameInstance> client1_instance_;
    std::unique_ptr<FM2KGameInstance> client2_instance_;
    // Local spectator instance — subscribes to client1 (host) on its
    // multiplexed UDP port and replays the input stream. Used by the
    // launcher's "Launch Spectator" button so we can validate the
    // spectator pipeline against a live local dual-client session.
    std::unique_ptr<FM2KGameInstance> spectator_instance_;
    // Second local spectator that subscribes to spectator_instance_ rather
    // than the host — exercises the daisy-chain relay (host → spec1 → spec2).
    // Validates that a relay node correctly forwards confirmed-input frames
    // it received from upstream to its own subscribers.
    std::unique_ptr<FM2KGameInstance> spectator2_instance_;
    FM2K::FM2KGameInfo selected_game_;
    NetworkConfig network_config_;
    LauncherState current_state_;
    bool running_;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2K::FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
    
    // Multi-client testing helpers
    bool LaunchLocalClient(const std::string& game_path, bool is_host, int port);
    // Launch a local spectator pointing at the host (client1) on host_port.
    // Spectator-mode hook will SPEC_JOIN_REQ the host and start replaying
    // the streamed input history (CSS + battle).
    // mode: "full" (default; replay-from-session-start input log) or
    // "current" (CCCaster-style snapshot join — code path exists but has
    // structural issues; see task #18). Default flipped to "full" 2026-05-08
    // so the live-spec button works while CURRENT_MATCH bakes.
    bool LaunchLocalSpectator(const std::string& game_path,
                              int spectator_port,
                              int host_port,
                              const std::string& mode = "full");
    // Daisy-chain test: launches a second spectator that subscribes to the
    // first spectator instead of the host. Verifies relay-node forwarding.
    bool LaunchLocalSpectator2(const std::string& game_path,
                               int spectator_port,
                               int upstream_port);
public:
    // Launch a spectator pointing at an arbitrary remote host (typically
    // received via hub spectate_grant). Used by the lobby UI's "click an
    // active match to watch it" path AND the --spectate CLI flag for e2e
    // testing. spectator_port is local UDP bind; host_ip:host_port is where
    // the spectator's FM2K_REMOTE_ADDR points and SpectatorNode JOIN_REQ
    // is sent. mode: "full" (default) or "current". See LaunchLocalSpectator
    // above for the rationale on the "full" default.
    bool LaunchRemoteSpectator(const std::string& game_path,
                               int spectator_port,
                               const std::string& host_ip,
                               int host_port,
                               const std::string& mode = "full");

    // Offline replay player. Launches the game with FM2K_SPECTATOR_MODE=1
    // + FM2K_REPLAY_FILE=<replay_path>; the hook reads the env var in
    // Netplay_InitAsSpectator, calls SpectatorNode_LoadSessionFile to
    // populate pb_queue, and the trampoline's RunSpectatorTick drives
    // playback. No network, no peer, no STUN — just the file.
    bool LaunchReplayPlayer(const std::string& game_path,
                            const std::string& replay_path);
private:
    bool TerminateAllClients();
    
    
    // Multi-client testing
    uint32_t client1_process_id_;
    uint32_t client2_process_id_;
    
    
    // Games directories (one or more roots where FM2K games are located).
    // Persisted as one path per line in launcher.cfg; the historical
    // single-string format migrates transparently because that file is
    // already line-delimited.
    std::vector<std::string> games_root_paths_;
    
    // Pending configuration (set before instances are created)
    struct PendingConfig {
        bool has_minimal_gamestate_testing = false;
        bool minimal_gamestate_testing_value = false;
        bool has_production_mode = false;
        bool production_mode_value = false;
        bool has_input_recording = false;
        bool input_recording_value = false;
    } pending_config_;
    
    // Helper method to read rollback statistics from hook shared memory  
    bool ReadRollbackStatsFromSharedMemory(RollbackStats& stats);
    
    // Apply pending configuration to game instances
    void ApplyPendingConfigToInstance(FM2KGameInstance* instance);
};

// Game instance management - see FM2K_GameInstance.h for full definition

// Modern ImGui launcher interface
class LauncherUI {
public:
    LauncherUI();
    ~LauncherUI();
    
    bool Initialize(SDL_Window* window, SDL_Renderer* renderer);
    void Shutdown();
    
    void NewFrame();
    void Render();
    
    // UI state callbacks
    std::function<void(const FM2K::FM2KGameInfo&)> on_game_selected;
    std::function<void()> on_offline_session_start;
    std::function<void(const NetworkConfig&)> on_online_session_start;
    std::function<void()> on_stress_session_start;  // Single-instance GekkoStressSession determinism test
    // Click-to-spectate. host_ip:host_port comes from the hub's
    // spectate_grant. Launcher should boot a local FM2K spectator instance
    // pointing at that addr (LaunchRemoteSpectator).
    std::function<void(const std::string& host_ip, int host_port)> on_spectate_match;
    // Hub fired a spectator_incoming event — we're the host of an active
    // match and a spectator wants in. Their external UDP addr is passed
    // so we can fire an outbound NAT-punch packet to open the inbound
    // mapping before their first JOIN_REQ arrives at our NAT.
    std::function<void(const std::string& spec_udp_ip,
                       int                spec_udp_port,
                       int                spec_tcp_port)> on_spectator_punch_target;
    std::function<void()> on_session_stop;
    std::function<void()> on_exit;
    // C11 — replay browser dispatch. Called when the user clicks a row in
    // the Replays panel; should call FM2KLauncher::LaunchReplayPlayer with
    // the absolute path to a .fm2krep / .fm2kset file. Game .exe is
    // resolved from the file's grandparent directory (replays/<f>.fm2krep
    // is always under <game_dir>/replays/) — matches the same logic the
    // CLI --replay flag uses.
    std::function<void(const std::string& replay_path)> on_replay_play;
    // Fired when the user adds, removes, or otherwise reorders the
    // configured games-root list. The full new list is passed by value so
    // the launcher can persist it atomically; the UI keeps its own copy
    // in games_root_paths_ for rendering.
    std::function<void(const std::vector<std::string>&)> on_games_folders_set;
    
    // Debug state callbacks
    std::function<bool()> on_debug_save_state;
    std::function<bool()> on_debug_load_state;
    std::function<bool(uint32_t)> on_debug_force_rollback;
    
    // Frame stepping controls
    std::function<void(bool)> on_frame_step_pause;      // Pause/resume game execution
    std::function<void()> on_frame_step_single;         // Step one frame
    std::function<void(uint32_t)> on_frame_step_multi;   // Step multiple frames
    
    // Slot-based save/load callbacks
    std::function<bool(uint32_t)> on_debug_save_to_slot;
    std::function<bool(uint32_t)> on_debug_load_from_slot;
    std::function<bool(bool, uint32_t)> on_debug_auto_save_config;  // (enabled, interval_frames)
    
    // Slot status callback
    struct SlotStatusInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
        uint32_t active_object_count;
    };
    std::function<bool(uint32_t, SlotStatusInfo&)> on_get_slot_status;  // (slot, status_out)
    
    // Auto-save configuration callbacks
    struct AutoSaveConfig {
        bool enabled;
        uint32_t interval_frames;
    };
    std::function<bool(AutoSaveConfig&)> on_get_auto_save_config;  // Get current auto-save settings
    
    // Enhanced action inspection data structure (FM2K "objects" are actually "actions")
    struct EnhancedActionInfo {
        // Core action data from DetailedObject
        uint16_t slot_index;
        uint32_t type;
        uint32_t id;
        uint32_t position_x, position_y;
        uint32_t velocity_x, velocity_y;
        uint32_t animation_state;
        uint32_t health_damage;
        uint32_t state_flags;
        uint32_t timer_counter;
        
        // 2DFM script integration
        std::string type_name;              // Human readable action type name
        std::string action_name;            // Current action being performed
        uint32_t script_id;                 // Associated script ID
        uint32_t animation_frame;           // Current animation frame
        
        // Character-specific data (for CHARACTER actions)
        std::string character_name;         // Character performing the action
        std::string current_move;           // Current move/technique name
        uint32_t facing_direction;          // 0=left, 1=right
        uint32_t combo_count;               // Hit combo counter
        
        // Raw memory for deep inspection
        uint8_t raw_data[382];              // Complete action data
        
        // Analysis helpers
        bool IsCharacter() const { return type == 4; }
        bool IsProjectile() const { return type == 5; }
        bool IsEffect() const { return type == 6; }
        bool IsSystem() const { return type == 1; }
        bool HasMovement() const { return velocity_x != 0 || velocity_y != 0; }
    };
    
    // Action inspection callback - returns current active actions with enhanced data
    std::function<std::vector<EnhancedActionInfo>()> on_get_enhanced_actions;
    
    // Debug and testing configuration callbacks
    std::function<bool(bool)> on_set_production_mode;              // Set production mode (reduced logging)
    std::function<bool(bool)> on_set_input_recording;              // Set input recording
    std::function<bool(bool)> on_set_minimal_gamestate_testing;    // Set MinimalGameState testing
    
    // Multi-client testing data structures
    // NetworkStats struct removed - network stats handled by LocalNetworkAdapter
    
    // RollbackStats is now defined at global scope
    
    // Save state profile removed - now using optimized FastGameState system
    
    // ======= Multi-Client Testing Infrastructure =======
    
    // Multi-client process management  
    std::function<bool(const std::string&)> on_launch_local_client1;     // Launch first client as host
    std::function<bool(const std::string&)> on_launch_local_client2;     // Launch second client as guest
    std::function<bool(const std::string&)> on_launch_local_spectator;   // Launch spectator subscribing to client1
    std::function<bool(const std::string&)> on_launch_local_spectator2;  // Launch second spectator subscribing to first (daisy-chain test)
    std::function<bool()> on_terminate_all_clients;                      // Kill all launched clients
    std::function<bool(uint32_t&, uint32_t&)> on_get_client_status;      // Get client process IDs (client1_pid, client2_pid)

    // Resolve a stage_id into a UTF-8 name from the locally-discovered
    // game's parsed KGT. Returns empty string if game_id isn't installed
    // locally or the slot is empty / out-of-range. Used at match_result
    // bake time to ship a human-readable stage name to the hub so other
    // players viewing recent matches see "公園" not "Stage #2".
    std::function<std::string(const std::string&, uint32_t)> on_resolve_stage_name;
    // Same shape, parallel hook for char_id → name lookup. Used by the
    // live-matches lobby panel for rows where the wire payload didn't
    // carry char_name (older client / KGT not installed on the sender).
    // Returns empty string when game isn't installed locally.
    std::function<std::string(const std::string&, uint32_t)> on_resolve_char_name;
    
    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    
    // Rollback monitoring  
    std::function<bool(RollbackStats&)> on_get_rollback_stats;           // Get rollback performance data
    
    // Data binding
    void SetGames(const std::vector<FM2K::FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetLauncherState(LauncherState state);
    void SetFramesAhead(float frames_ahead);
    // Update scanning progress (0-1). Only meaningful while scanning flag is true.
    void SetScanning(bool scanning);
    void SetGamesRootPaths(const std::vector<std::string>& paths);

    // Forward an external TCP addr (learned by the spec hook via TCP-STUN
    // against the hub) to the hub via a `tcp_addr` WS message. Called
    // from FM2KLauncher::Update on tcp_stun_seq SharedMem bumps.
    void SendHubTcpAddr(uint32_t ip_be, uint16_t port);

private:
    // Logging
    void AddLog(const char* message);
    void ClearLog();
    static void SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message);

    // UI state
    std::vector<FM2K::FM2KGameInfo> games_;
    NetworkConfig network_config_;
    float frames_ahead_;
    LauncherState launcher_state_;
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    std::vector<std::string> games_root_paths_;  // Configured games root directories
    int selected_game_index_ = -1; // -1 means no selection
    bool scanning_games_ = false;  // True while background discovery is running

    // Challenge notification toggles. Defaults to all-on so players never
    // miss an incoming challenge while tabbed out. Persists in
    // %APPDATA%\FM2K_Rollback\settings.ini under keys notify_flash,
    // notify_sound, notify_toast. Loaded on first menu-bar render and
    // saved whenever the user toggles a checkbox in Settings →
    // Notifications.
    bool notify_flash_ = true;
    bool notify_sound_ = true;
    bool notify_toast_ = true;
    bool notify_state_loaded_ = false;
    
    // Console Log
    ImGuiTextBuffer log_buffer_;
    SDL_Mutex* log_buffer_mutex_;
    bool scroll_to_bottom_;
    SDL_LogOutputFunction original_log_function_;
    void* original_log_userdata_;

    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    void RenderSessionControls();
    void RenderDebugTools();
    void RenderMultiClientTools();
    void RenderNetworkTools();
    void RenderConsoleLog();
    void RenderObjectAnalysis();        // Stub
    void RenderSlotInspectionWindow();  // Stub
    void RenderHubPanel();              // Fightcade-style lobby
    void RenderHostConfigWindow();      // Match-settings UI (SOCD, stage, etc.)
    void RenderHubServerWindow();       // Legacy floating window — kept for hot-reload paths; new path is the Settings tab.
    void RenderDiscordAuthWindow();     // Stays separate — OAuth pairing flow has its own state machine.
    void RenderGamesFoldersWindow();    // Legacy.
    void RenderRecentMatchesWindow();   // Legacy.

    // Single consolidated Settings window with tabs. Replaces the
    // five floating Settings sub-windows. Floats but is non-movable
    // and non-dockable so it stays a popup-style modal — open it from
    // Settings → Settings…, do your thing, close it. Tabs: Input P1,
    // Input P2, Host Config, Hub Server, Games Folders, Recent Matches.
    void RenderSettingsWindow();
    // Per-tab body renderers (no Begin/End — caller owns the container).
    // Reused by both the legacy floating windows and the new Settings tabs.
    void RenderHubServerBody();
    void RenderHostConfigBody();
    void RenderGamesFoldersBody();
    void RenderRecentMatchesBody();
    // Hub-panel "Live Matches" — shows every InFlightMatch the hub knows
    // about. Char/stage names render via fm2k::FormatCharLabel /
    // FormatStageLabel so names appear when the viewer has the game
    // installed locally OR when a peer baked the names into the hub
    // payload. Refreshes from MatchInProgress* events; no per-render hub
    // round-trip.
    void RenderInProgressMatchesBody();
    // Settings → Display tab. Edits <install_dir>\ddraw.ini for the
    // bundled cnc-ddraw build. State cached in `ddraw_cfg_`; loaded
    // lazily on first tab render via `LoadDDrawCfgIfNeeded`. Per-widget
    // changes write back through fm2k::cnc_ddraw::Save* helpers, which
    // hit the ini through Win32 WritePrivateProfileString — preserves
    // unknown keys + per-game `[<exe>]` blocks the user might have.
    void RenderDisplayBody();
    void LoadDDrawCfgIfNeeded();
    // Per-player SOCD picker rendered above/below each player's
    // binding tab. SOCD is a local input filter applied before the
    // 11-bit mask hits the wire, so different modes on the two peers
    // do NOT cause desyncs — each peer's slot has its own setting.
    // Persisted to settings.ini (`socd_mode_p1`, `socd_mode_p2`).
    void RenderInputBindingsTab(int player_slot);
    // Per-launcher SOCD state. Loaded from settings.ini on first menu
    // render; written back when the user changes the picker. Pushed to
    // the spawned game's hook via FM2K_SOCD_MODE env at launch time.
    int  socd_mode_[2] = {1, 1};   // tournament-default (Hitbox SOCD)
    bool socd_state_loaded_ = false;
    void LoadSocdState();
    void SaveSocdState();
    // Random-stage host preference (#56). Persisted in settings.ini
    // alongside SOCD; consumed at challenge time to populate
    // MatchSettings::random_seed/min/max.
    bool random_stage_enable_   = false;
    int  random_stage_min_      = 0;
    int  random_stage_max_      = 7;
    bool random_state_loaded_   = false;
    void LoadRandomStageState();
    void SaveRandomStageState();

    // Refresh the SDL window title to "FM2K Rollback Launcher — <nick> (W-L-D)"
    // any time the user's record changes. No-op if we don't have a record
    // yet (record-fetch races with first lobby render). Called from the
    // K::RecordReceived handler.
    void UpdateWindowTitleWithRecord();

    // Push the current overall + vs-peer W/L/D and the peer/my nick into
    // the running game's FM2KSharedMemData. The hook reads these to
    // render the in-game titlebar (and eventually the in-game overlay)
    // so the player sees their record without alt-tabbing. Called any
    // time the cached record or current peer changes (K::RecordReceived,
    // K::MatchStart). Cheap no-op when there is no active game process.
    void PushStatsToHook();

    // Push a system-message to the in-game HUD (centered overlay
    // with TTL fade). Mirrors PushStatsToHook's PID-resolution path:
    // writes into both running clients' shared-mem mappings if any.
    // No-op when no game is running. Used for netplay events the
    // user benefits from seeing without alt-tabbing (peer dropped,
    // hub-side state change, match starting).
    void PushHudSystemMessage(const char* text_utf8, uint32_t ttl_ms);

    // Append one row to %APPDATA%\FM2K_Rollback\results.csv (#42). Writes
    // a UTF-8 BOM on first creation so Excel renders JP/accented names
    // correctly. Invoked from PollMatchOutcome alongside the hub send so
    // the local log captures the match even when the hub roundtrip fails.
    // outcome_str is the same string we send to the hub (self_won /
    // peer_won / draw / disconnect).
    void AppendResultsCsvRow(const char* outcome_str,
                             uint32_t p1_char_id, uint32_t p2_char_id,
                             const std::string& p1_char_name,
                             const std::string& p2_char_name);
    void LoadAudioMuteState();          // Read %APPDATA%\FM2K_Rollback\audio.ini
    void SaveAudioMuteState();          // Write same file (hook re-reads it)
    void LoadNotifyState();             // Read notify_* keys from settings.ini
    void SaveNotifyState();             // Write notify_* keys to settings.ini
    // Fire all enabled challenge notifications: taskbar flash + sound chirp
    // + Windows toast. Each piece is independently togglable in Settings.
    // Called from the K::ChallengeReceived event handler.
    void FireChallengeNotification(const std::string& from_nick);

    // Fire a generic notification (taskbar flash + sound + toast) with a
    // caller-provided title and body. Used for the peer-disconnect path
    // where the challenge-specific copy doesn't apply, but we still want
    // the user to notice the launcher is taking back focus + closing the
    // match. UTF-8 strings (converted to UTF-16 for Shell_NotifyIconW).
    void FireSystemNotification(const std::string& title_utf8,
                                const std::string& body_utf8);

    // Poll FM2KSharedMemData on every running game PID. When the hook
    // bumps `match_outcome_seq`, read the new outcome enum, map it to a
    // hub `match_result` string, and send. Idempotent across frames —
    // a per-PID last-seen-seq prevents re-sends of the same bump. No-op
    // when no hub match is active or the launcher isn't connected. Also
    // detects the FM2K_MATCH_OUTCOME_DISCONNECT case and asks the local
    // game to close so the surviving instance doesn't stay open after
    // the peer drops.
    void PollMatchOutcome();

    // Developer mode toggle. End-user UI hides the offline-bisect
    // checkboxes, dual-client launcher, stress test, and spectator
    // chain test. Enabled via FM2K_DEV_MODE=1 env var on launch or
    // via View → Developer Mode in the menu bar.
    bool developer_mode_ = false;

    // Settings windows toggled from the menu bar. input_binder_initialized_
    // gates Init() to a single call (gamepad subsystem startup) the first
    // time the user opens either binder window.
    bool show_settings_        = false;     // Single tabbed Settings window
    bool show_discord_auth_    = false;     // Sign in with Discord — OAuth flow is its own window
    // Legacy per-section flags kept for any path that still toggles them.
    // The unified Settings window is the user-facing surface now.
    bool show_input_binder_p1_ = false;
    bool show_input_binder_p2_ = false;
    bool show_host_config_     = false;
    bool show_hub_server_      = false;
    bool show_games_folders_   = false;
    bool show_recent_matches_  = false;
    bool show_replay_browser_  = false;
    bool input_binder_initialized_ = false;

    // Settings → Display state. `ddraw_cfg_` is loaded once on first
    // open of the Display tab; subsequent edits save back per-key
    // through fm2k::cnc_ddraw::Save* and refresh the cached value
    // immediately so the widget reflects the new state.
    fm2k::cnc_ddraw::IniConfig ddraw_cfg_{};
    bool ddraw_cfg_loaded_ = false;

    // Audio mute toggles (Settings menu). Persisted to
    // %APPDATA%\FM2K_Rollback\audio.ini; the hook DLL re-reads that file
    // ~once per second from inside the dispatcher so changes propagate
    // mid-game without IPC. Booted from the file on first menu render.
    bool mute_bgm_ = false;
    bool mute_se_  = false;
    bool mute_state_loaded_ = false;

    // Host-config staged values (committed on Apply → fm2k_host.ini + env var).
    int      host_config_socd_mode_ = 1;          // tournament default
    uint32_t host_config_stage_     = 0xFFFFFFFFu;// 0xFFFFFFFF = unset
    bool     host_config_dirty_     = false;

    // Hub server hostname / IP. Edited from Settings → Hub Server…
    // (used to live in the Hub panel; moved out so casual users don't
    // see it by default). Persisted via FM2K_HUB_HOST env var on
    // connect. Empty = use FM2K_HUB_HOST env var or 2dfm.sytes.net.
    char     hub_host_[128] = {};
    bool     hub_host_initialized_ = false;

    // Discord OAuth status, surfaced in the menu bar's sign-in pill.
    // Refreshed on startup, on sign-in completion, and on sign-out;
    // never read every frame so we don't hammer the .json file.
    bool     discord_signed_in_  = false;
    std::string discord_nick_;
    bool     discord_state_loaded_ = false;

    // Hub client + per-frame drained state. Owned by the launcher
    // (forward-declared in LauncherUI scope to avoid pulling
    // FM2K_HubClient.h into the header). Defined out-of-line in
    // FM2K_LauncherUI.cpp.
    struct HubState;
    std::unique_ptr<HubState> hub_state_;

public:
    // Tell the hub the current match (if any) has ended. Called by
    // FM2KLauncher::StopSession on both the user-initiated stop and
    // the game-process-died path. No-op when not in a hub-driven
    // session (the hub will silently treat the match_ended for an
    // already-idle user as a noop).
    void NotifyHubMatchEnded();

    // Helper methods
    void ShowGameValidationStatus(const FM2K::FM2KGameInfo& game);
    void ShowNetworkDiagnostics();      // Stub
    bool ValidateNetworkConfig();
    
    // Simplified theme - always Dark
    enum class UITheme { Dark };
    void SetTheme(UITheme theme);
    UITheme current_theme_;

    // Save state inspection
    int selected_inspection_slot_ = -1;
    bool show_slot_inspection_ = false;

    // C11 — Replay browser. Lazily-populated cache of .fm2krep / .fm2kset
    // files found across configured games-root paths. Each entry mirrors
    // the FM2KSessionFileHeader fields the tree UI displays. Sessions
    // group entries by session_id; matches inside each session order by
    // match_index_in_session.
    struct ReplayMeta {
        std::string path;             // absolute path
        bool        is_battle_slice;  // .fm2krep (true) vs .fm2kset (false)
        uint64_t    started_at_unix;
        uint64_t    finished_at_unix;
        uint32_t    event_count;
        uint32_t    input_count;
        char        game_id[32];      // null-padded
        char        p1_nick[32];
        char        p2_nick[32];
        uint8_t     p1_char_id;
        uint8_t     p2_char_id;
        uint8_t     rounds_won_p1;
        uint8_t     rounds_won_p2;
        uint8_t     match_count;
        uint8_t     match_index;
        uint64_t    session_id;
        uint8_t     round_count;
    };
    std::vector<ReplayMeta> replays_cache_;
    bool                    replays_cache_dirty_ = true;  // first-render rescan

    // Build replays_cache_ from games_root_paths_. Walks each root for
    // <game>/replays/*.fm2krep — sniffs the 256-byte FM2KSessionFileHeader
    // off the front of each file. Cheap (<200ms for ~1000 files); called
    // lazily on Replay panel open or after a refresh button.
    void ScanReplays();

    // ImGui body for the Replays window. Renders Session → Match tree.
    // Click a row → on_replay_play(path).
    void RenderReplayBrowser();
};