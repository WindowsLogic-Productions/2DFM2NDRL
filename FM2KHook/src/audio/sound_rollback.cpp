#include "sound_rollback.h"
#include <cstring>
#include <unordered_map>
#include <SDL3/SDL_log.h>
#include <windows.h>  // GetTickCount

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

// Real DSound plays happen by invoking the ORIGINAL DispatchScriptSoundCommand
// trampoline on the saved script_item. That covers the full StopAllSounds +
// PlaySoundFromBufferArray + IDirectSoundBuffer::Play + volume sequence — none
// of which PlaySoundFromBufferArray alone performs (it only preps the buffer).
SoundRollback::OriginalDispatcherFn g_original_dispatcher = nullptr;

SoundRollback::DesiredState g_desired[SoundRollback::MAX_CHANNELS];
SoundRollback::DesiredState g_actual [SoundRollback::MAX_CHANNELS];

std::unordered_map<void*, int> g_ptr_to_chan;

uint16_t g_seq_counter = 0;
uint32_t g_seq_anchor_frame = 0;

// Diagnostics: tally how many plays land on known vs unknown channels. Logged
// periodically so we can see whether the Mike Z layer is actually covering the
// SFX we care about, or whether characters allocate buffer_arrays outside
// g_sound_channel_table and we're all passthrough.
uint32_t g_stat_record_known = 0;
uint32_t g_stat_record_unknown = 0;
uint32_t g_stat_last_log_tick = 0;

// Music-path dedup state (MIDI/CD). Cleared by OnBattleEnd so each match
// starts with a blank slate.
uint32_t g_last_music_cmd = 0xFFFFFFFFu;
uint32_t g_last_music_payload = 0;
bool     g_last_music_valid = false;

bool StatesEqual(const SoundRollback::DesiredState& a,
                 const SoundRollback::DesiredState& b) {
    // Channel identity is (wave_ptr, play_frame, stopped). seq_in_frame is
    // NOT part of identity — it's a within-frame tiebreaker that can drift
    // across stress-mode replay batches because g_seq_counter is global and
    // the number of channels firing in a given frame anchor varies across
    // batches. Including seq here caused constant stop+restart cycles on
    // stable sounds (audible as buzzing).
    return a.wave_ptr   == b.wave_ptr
        && a.play_frame == b.play_frame
        && a.stopped    == b.stopped;
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

void SetOriginalDispatcher(OriginalDispatcherFn fn) {
    g_original_dispatcher = fn;
}

bool IsRedundantMusicDispatch(uint8_t cmd, uint32_t payload) {
    // Skip only when BOTH cmd and payload match the previous non-replay
    // dispatch. Any change — a new track, a stop-then-same-track, a switch
    // to CD audio, a round-end fanfare — updates the stored key and fires.
    // So mid-match music transitions work normally; only the exact same
    // (cmd, payload) repeating (which is what GekkoNet's save-ring scroll
    // causes) gets filtered out.
    if (g_last_music_valid &&
        g_last_music_cmd == static_cast<uint32_t>(cmd) &&
        g_last_music_payload == payload) {
        return true;
    }
    g_last_music_valid   = true;
    g_last_music_cmd     = static_cast<uint32_t>(cmd);
    g_last_music_payload = payload;
    return false;
}

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
    if (g_stat_record_known + g_stat_record_unknown > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: battle end — total plays known=%u unknown=%u",
            g_stat_record_known, g_stat_record_unknown);
    }
    g_stat_record_known = 0;
    g_stat_record_unknown = 0;
    g_stat_last_log_tick = 0;
    g_last_music_valid = false;       // next match's first music dispatch always fires
    g_last_music_cmd = 0xFFFFFFFFu;
    g_last_music_payload = 0;
    g_ptr_to_chan.clear();
    std::memset(g_desired, 0, sizeof(g_desired));
    std::memset(g_actual,  0, sizeof(g_actual));
}

bool RecordDesired(void* arr, int script_item, uint32_t current_frame) {
    int chan = ChannelFor(arr);
    if (chan < 0 || chan >= MAX_CHANNELS) {
        // Unknown channel — not in g_sound_channel_table. Caller must fall
        // through to the original dispatcher so the sound still plays; it just
        // won't be rollback-tracked.
        g_stat_record_unknown++;
        return false;
    }
    g_stat_record_known++;

    if (current_frame != g_seq_anchor_frame) {
        g_seq_counter = 0;
        g_seq_anchor_frame = current_frame;
    }

    // SoundBufferArray layout: {wave_ptr, wave_len, buf_count, cur_index, buffers[]}.
    // Identity uses wave_ptr — same wave on same channel = "no change", lets the
    // dedupe path skip re-triggering.
    uint32_t wave_ptr = *reinterpret_cast<uint32_t*>(arr);

    g_desired[chan].script_item_ptr = static_cast<uint32_t>(script_item);
    g_desired[chan].wave_ptr        = wave_ptr;
    g_desired[chan].play_frame      = current_frame;
    g_desired[chan].seq_in_frame    = ++g_seq_counter;
    g_desired[chan].stopped         = 0;
    return true;
}

void SyncAfterAdvance(uint32_t earliest_frame, uint32_t current_frame) {
    void** table = reinterpret_cast<void**>(ADDR_CHANNEL_TABLE);

    // Once-per-second coverage log. If unknown >> known, the Mike Z layer is
    // mostly inert and the pre-scanned g_sound_channel_table isn't where
    // character SFX actually live — we need to widen the channel identity.
    uint32_t now_tick = GetTickCount();
    if (now_tick - g_stat_last_log_tick >= 1000 &&
        (g_stat_record_known + g_stat_record_unknown) > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: coverage — known=%u unknown=%u (passthrough ratio %.1f%%)",
            g_stat_record_known, g_stat_record_unknown,
            100.0f * g_stat_record_unknown /
                (float)(g_stat_record_known + g_stat_record_unknown));
        g_stat_last_log_tick = now_tick;
    }

    // Per-sync breakdown of which branch each divergent channel takes, so we
    // can see why "known=331 unknown=0" still produces silence. Logged at the
    // same 1-Hz cadence as the coverage line, bounded to 4 entries/sync.
    static uint32_t s_branch_log_tick = 0;
    bool verbose = (now_tick - s_branch_log_tick >= 1000);
    int verbose_remaining = 4;
    uint32_t branch_plays = 0;
    uint32_t branch_stops = 0;
    uint32_t branch_skips = 0;

    for (int chan = 0; chan < MAX_CHANNELS; chan++) {
        if (StatesEqual(g_desired[chan], g_actual[chan])) {
            branch_skips++;
            continue;
        }

        const bool des_in = FrameInWindow(g_desired[chan].play_frame, earliest_frame, current_frame);
        const bool act_in = FrameInWindow(g_actual [chan].play_frame, earliest_frame, current_frame);

        if (verbose && verbose_remaining > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "  chan=%d des{wav=0x%08X f=%u s=%u stop=%u} act{wav=0x%08X f=%u s=%u stop=%u} win=[%u,%u] des_in=%d act_in=%d",
                chan,
                g_desired[chan].wave_ptr, g_desired[chan].play_frame,
                g_desired[chan].seq_in_frame, g_desired[chan].stopped,
                g_actual[chan].wave_ptr,  g_actual[chan].play_frame,
                g_actual[chan].seq_in_frame, g_actual[chan].stopped,
                earliest_frame, current_frame, (int)des_in, (int)act_in);
            verbose_remaining--;
        }

        if (!des_in) {
            // Desired was set outside the rollback window — rollback couldn't
            // have changed it. But if actual is inside the window, rollback
            // erased a play that's currently audible: stop it by stopping
            // every buffer on the channel.
            if (act_in) {
                void* arr = table[chan];
                if (arr) {
                    // Re-invoke the dispatcher with a synthesised "stop" script
                    // item would require allocation; instead just clobber the
                    // channel's buffers directly. For FM2K we can do this via
                    // the existing StopAllSoundsInBufferArray @ 0x415F00.
                    using StopFn = int(__cdecl*)(void*);
                    ((StopFn)0x415F00)(arr);
                }
                g_actual[chan].script_item_ptr = 0;
                g_actual[chan].wave_ptr        = 0;
                g_actual[chan].play_frame      = current_frame;
                g_actual[chan].seq_in_frame    = 0;
                g_actual[chan].stopped         = 1;
                branch_stops++;
            }
            // Sync desired forward so savestates capture "what's really playing."
            g_desired[chan] = g_actual[chan];
        } else {
            // Desired was set during the window — trigger the actual play via
            // the original dispatcher, which runs the full
            // Stop+Prepare+Play+Volume sequence. PlaySoundFromBufferArray
            // alone only preps the buffer; this is why the previous version
            // recorded 100% known channels but produced zero audible output.
            if (!g_desired[chan].stopped &&
                g_original_dispatcher &&
                g_desired[chan].script_item_ptr) {
                g_original_dispatcher(
                    static_cast<int>(g_desired[chan].script_item_ptr));
            } else {
                // Explicit "play nothing" or missing dispatcher — just stop.
                void* arr = table[chan];
                if (arr) {
                    using StopFn = int(__cdecl*)(void*);
                    ((StopFn)0x415F00)(arr);
                }
            }
            g_actual[chan] = g_desired[chan];
            branch_plays++;
        }
    }

    if (verbose && (branch_plays + branch_stops) > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SoundRollback: sync window=[%u,%u] plays=%u stops=%u skips=%u",
            earliest_frame, current_frame, branch_plays, branch_stops, branch_skips);
        s_branch_log_tick = now_tick;
    }
}

void CaptureDesired(DesiredState* out) {
    std::memcpy(out, g_desired, sizeof(g_desired));
}

void RestoreDesired(const DesiredState* in) {
    std::memcpy(g_desired, in, sizeof(g_desired));
}

} // namespace SoundRollback
