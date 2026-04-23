#include "sound_rollback.h"
#include <cstring>
#include <unordered_map>
#include <SDL3/SDL_log.h>

// ============================================================================
// Constants from FM2K binary (verified via IDA, 2026-04-23).
//   g_sound_channel_table     @ 0x430640 — array of SoundBufferArray*
//   g_sound_channel_table_end @ 0x433240 — end marker (used by ReleaseAllSoundBuffers)
//   PlaySoundFromBufferArray  @ 0x415DF0 — core DSound play + round-robin
//   StopAllSoundsInBufferArray@ 0x415F00 — stop every buffer in one array
// ============================================================================
namespace {

constexpr uintptr_t ADDR_CHANNEL_TABLE     = 0x430640;
constexpr uintptr_t ADDR_CHANNEL_TABLE_END = 0x433240;
constexpr size_t    CHANNEL_TABLE_SLOTS    =
    (ADDR_CHANNEL_TABLE_END - ADDR_CHANNEL_TABLE) / sizeof(void*);

using PlaySoundFn = int(__cdecl*)(void*);
using StopSoundsFn = int(__cdecl*)(void*);

PlaySoundFn  g_fn_play = reinterpret_cast<PlaySoundFn>(0x415DF0);
StopSoundsFn g_fn_stop = reinterpret_cast<StopSoundsFn>(0x415F00);

SoundRollback::DesiredState g_desired[SoundRollback::MAX_CHANNELS];
SoundRollback::DesiredState g_actual [SoundRollback::MAX_CHANNELS];

std::unordered_map<void*, int> g_ptr_to_chan;

uint16_t g_seq_counter = 0;
uint32_t g_seq_anchor_frame = 0;

bool StatesEqual(const SoundRollback::DesiredState& a,
                 const SoundRollback::DesiredState& b) {
    return a.wave_ptr == b.wave_ptr
        && a.play_frame == b.play_frame
        && a.seq_in_frame == b.seq_in_frame
        && a.stopped == b.stopped;
}

inline bool FrameInWindow(uint32_t f, uint32_t lo, uint32_t hi) {
    return f >= lo && f <= hi;
}

int ChannelFor(void* arr) {
    auto it = g_ptr_to_chan.find(arr);
    if (it != g_ptr_to_chan.end()) return it->second;
    // Lazy fallback — a buffer array might have been populated after Init()
    // (e.g. mid-match asset load, unusual but handle cleanly).
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);
    for (int i = 0; i < static_cast<int>(CHANNEL_TABLE_SLOTS); i++) {
        if (table[i] == arr) {
            g_ptr_to_chan[arr] = i;
            return i;
        }
    }
    return -1;
}

} // anonymous

namespace SoundRollback {

void Init() {
    g_ptr_to_chan.clear();
    std::memset(g_desired, 0, sizeof(g_desired));
    std::memset(g_actual,  0, sizeof(g_actual));
    g_seq_counter = 0;
    g_seq_anchor_frame = 0;

    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);
    int populated = 0;
    for (int i = 0; i < static_cast<int>(CHANNEL_TABLE_SLOTS); i++) {
        if (table[i]) {
            g_ptr_to_chan[table[i]] = i;
            populated++;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SoundRollback: Init — %d non-null channels in %zu-slot table",
        populated, CHANNEL_TABLE_SLOTS);
}

void OnBattleEnd() {
    g_ptr_to_chan.clear();
    std::memset(g_desired, 0, sizeof(g_desired));
    std::memset(g_actual,  0, sizeof(g_actual));
}

void RecordDesired(void* arr, uint32_t current_frame) {
    int chan = ChannelFor(arr);
    if (chan < 0 || chan >= MAX_CHANNELS) return;

    if (current_frame != g_seq_anchor_frame) {
        g_seq_counter = 0;
        g_seq_anchor_frame = current_frame;
    }

    // SoundBufferArray layout: {wave_ptr, wave_len, buf_count, cur_index, buffers[]}.
    // Identity uses wave_ptr — same wave on same channel = "no change", lets the
    // dedupe path skip re-triggering.
    uint32_t wave_ptr = *reinterpret_cast<uint32_t*>(arr);

    g_desired[chan].wave_ptr     = wave_ptr;
    g_desired[chan].play_frame   = current_frame;
    g_desired[chan].seq_in_frame = ++g_seq_counter;
    g_desired[chan].stopped      = 0;
}

void SyncAfterAdvance(uint32_t earliest_frame, uint32_t current_frame) {
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);

    for (int chan = 0; chan < MAX_CHANNELS; chan++) {
        if (StatesEqual(g_desired[chan], g_actual[chan])) continue;

        const bool des_in = FrameInWindow(g_desired[chan].play_frame, earliest_frame, current_frame);
        const bool act_in = FrameInWindow(g_actual [chan].play_frame, earliest_frame, current_frame);

        if (!des_in) {
            // Desired was set outside the rollback window — rollback couldn't
            // have changed it. But if actual is inside the window, rollback
            // erased a play that's currently audible: stop it.
            if (act_in) {
                void* arr = table[chan];
                if (arr) g_fn_stop(arr);
                g_actual[chan].wave_ptr     = 0;
                g_actual[chan].play_frame   = current_frame;
                g_actual[chan].seq_in_frame = 0;
                g_actual[chan].stopped      = 1;
            }
            // Sync desired forward so savestates capture "what's really playing."
            g_desired[chan] = g_actual[chan];
        } else {
            // Desired was set during the window — trigger the actual play.
            void* arr = table[chan];
            if (arr) {
                g_fn_stop(arr);
                if (!g_desired[chan].stopped) g_fn_play(arr);
            }
            g_actual[chan] = g_desired[chan];
        }
    }
}

void CaptureDesired(DesiredState* out) {
    std::memcpy(out, g_desired, sizeof(g_desired));
}

void RestoreDesired(const DesiredState* in) {
    std::memcpy(g_desired, in, sizeof(g_desired));
}

} // namespace SoundRollback
