#include "replay.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <windows.h>

namespace Replay {

// =============================================================================
// STATE
// =============================================================================

// Recording state. The frame log is kept entirely in memory; we flush on
// Replay_EndRecording. A 3-minute match at 100 FPS is 72 KB of ReplayFrame
// entries — cheaper to buffer than to fsync every frame.
struct RecordingState {
    bool                     active         = false;
    ReplayHeader             header         = {};
    std::vector<ReplayFrame> frames;
    char                     file_path[512] = {};
};

// Playback cursor. Can read from either a file-mapped buffer or an
// in-memory buffer (spectator stream). We normalise to a single byte buffer
// at open time so NextFrame is oblivious to the source.
struct PlaybackState {
    bool                 active       = false;
    ReplayHeader         header       = {};
    std::vector<uint8_t> buf;         // Full file contents
    uint32_t             cursor_frame = 0;
};

// Buffer holding the most recently completed recording so Replay_SerializeLast
// can hand it to the spectator stream without rereading from disk.
struct LastCompletedCache {
    std::vector<uint8_t> buf;
};

static RecordingState       g_rec;
static PlaybackState        g_play;
static LastCompletedCache   g_last;

// =============================================================================
// INTERNAL HELPERS
// =============================================================================

static void FormatTimestampFilename(char* out, size_t out_sz) {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &now);
    std::strftime(out, out_sz, "replays/%Y-%m-%d_%H%M%S.fm2krep", &tm_buf);
}

static void EnsureReplaysDir() {
    CreateDirectoryA("replays", nullptr);  // no-op if it exists
}

// Serialize the current recording into a byte buffer. Used both for file
// flush and for spectator stream handoff.
static void SerializeRecording(const RecordingState& rec, std::vector<uint8_t>& out) {
    ReplayHeader final_hdr = rec.header;
    final_hdr.frame_count = static_cast<uint32_t>(rec.frames.size());

    const size_t frames_bytes = rec.frames.size() * sizeof(ReplayFrame);
    out.resize(sizeof(ReplayHeader) + frames_bytes);
    std::memcpy(out.data(), &final_hdr, sizeof(ReplayHeader));
    if (frames_bytes > 0) {
        std::memcpy(out.data() + sizeof(ReplayHeader),
                    rec.frames.data(), frames_bytes);
    }
}

// =============================================================================
// RECORDING
// =============================================================================

bool Replay_BeginRecording(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color, const char* p1_name,
    uint8_t p2_char, uint8_t p2_color, const char* p2_name)
{
    // Defensive: close any in-flight recording from a prior match that
    // didn't hit its end handler (crash / disconnect).
    if (g_rec.active) {
        Replay_EndRecording();
    }

    g_rec = RecordingState{};
    g_rec.header.magic              = REPLAY_MAGIC;
    g_rec.header.version            = REPLAY_VERSION;
    g_rec.header.unix_timestamp     = static_cast<uint64_t>(std::time(nullptr));
    g_rec.header.game_hash          = game_hash;
    g_rec.header.initial_rng_seed   = initial_rng_seed;
    g_rec.header.initial_state_hash = initial_state_hash;
    g_rec.header.p1_char_slot       = p1_char;
    g_rec.header.p1_color           = p1_color;
    g_rec.header.p2_char_slot       = p2_char;
    g_rec.header.p2_color           = p2_color;

    if (p1_name) {
        std::strncpy(reinterpret_cast<char*>(g_rec.header.p1_name),
                     p1_name, sizeof(g_rec.header.p1_name) - 1);
    }
    if (p2_name) {
        std::strncpy(reinterpret_cast<char*>(g_rec.header.p2_name),
                     p2_name, sizeof(g_rec.header.p2_name) - 1);
    }

    EnsureReplaysDir();
    FormatTimestampFilename(g_rec.file_path, sizeof(g_rec.file_path));
    g_rec.active = true;

    // Pre-reserve for a 3-minute match so RecordFrame doesn't allocate
    // during gameplay.
    g_rec.frames.reserve(18000);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Replay: Begin recording -> %s (p1=%u/%u vs p2=%u/%u, seed=0x%08X)",
                g_rec.file_path, p1_char, p1_color, p2_char, p2_color,
                initial_rng_seed);
    return true;
}

void Replay_RecordFrame(uint16_t p1_input, uint16_t p2_input) {
    if (!g_rec.active) return;
    g_rec.frames.push_back({p1_input, p2_input});
}

void Replay_EndRecording() {
    if (!g_rec.active) return;

    std::vector<uint8_t> out;
    SerializeRecording(g_rec, out);

    FILE* fp = std::fopen(g_rec.file_path, "wb");
    if (fp) {
        std::fwrite(out.data(), 1, out.size(), fp);
        std::fclose(fp);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Replay: Wrote %zu bytes (%u frames) to %s",
                    out.size(),
                    static_cast<unsigned>(g_rec.frames.size()),
                    g_rec.file_path);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: Failed to open %s for write", g_rec.file_path);
    }

    // Keep a copy for spectator handoff before clearing recording state.
    g_last.buf = std::move(out);
    g_rec.active = false;
    g_rec.frames.clear();
    g_rec.frames.shrink_to_fit();
}

bool Replay_IsRecording() { return g_rec.active; }

// =============================================================================
// PLAYBACK
// =============================================================================

static bool ValidateHeader(const ReplayHeader& h) {
    if (h.magic != REPLAY_MAGIC) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: Bad magic 0x%08X (want 0x%08X)", h.magic, REPLAY_MAGIC);
        return false;
    }
    if (h.version != REPLAY_VERSION) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: Version mismatch: file=%u runtime=%u",
                     h.version, REPLAY_VERSION);
        return false;
    }
    return true;
}

bool Replay_OpenForPlayback(const char* file_path) {
    Replay_ClosePlayback();

    FILE* fp = std::fopen(file_path, "rb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: Cannot open %s", file_path);
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    if (sz < static_cast<long>(sizeof(ReplayHeader))) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: %s too small (%ld bytes)", file_path, sz);
        return false;
    }

    g_play.buf.resize(static_cast<size_t>(sz));
    std::fread(g_play.buf.data(), 1, static_cast<size_t>(sz), fp);
    std::fclose(fp);

    return Replay_LoadFromBuffer(g_play.buf.data(), g_play.buf.size());
}

bool Replay_LoadFromBuffer(const uint8_t* buf, size_t buf_size) {
    if (buf_size < sizeof(ReplayHeader)) return false;

    ReplayHeader hdr;
    std::memcpy(&hdr, buf, sizeof(ReplayHeader));
    if (!ValidateHeader(hdr)) return false;

    const size_t need = sizeof(ReplayHeader) + hdr.frame_count * sizeof(ReplayFrame);
    if (buf_size < need) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Replay: Truncated — have %zu bytes, need %zu",
                     buf_size, need);
        return false;
    }

    // If the buffer came from Replay_OpenForPlayback it's already in g_play.buf.
    // Otherwise (memory load from spectator stream), copy it in.
    if (buf != g_play.buf.data()) {
        g_play.buf.assign(buf, buf + buf_size);
    }
    g_play.header       = hdr;
    g_play.cursor_frame = 0;
    g_play.active       = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Replay: Loaded %u frames, p1=%u/%u vs p2=%u/%u, seed=0x%08X",
                hdr.frame_count, hdr.p1_char_slot, hdr.p1_color,
                hdr.p2_char_slot, hdr.p2_color, hdr.initial_rng_seed);
    return true;
}

const ReplayHeader* Replay_GetHeader() {
    return g_play.active ? &g_play.header : nullptr;
}

bool Replay_NextFrame(uint16_t* p1_input, uint16_t* p2_input) {
    if (!g_play.active) return false;
    if (g_play.cursor_frame >= g_play.header.frame_count) return false;

    const uint8_t* frames_base = g_play.buf.data() + sizeof(ReplayHeader);
    const ReplayFrame* f =
        reinterpret_cast<const ReplayFrame*>(frames_base) + g_play.cursor_frame;

    if (p1_input) *p1_input = f->p1_input;
    if (p2_input) *p2_input = f->p2_input;
    g_play.cursor_frame++;
    return true;
}

bool Replay_SeekFrame(uint32_t frame) {
    if (!g_play.active) return false;
    if (frame > g_play.header.frame_count) return false;
    g_play.cursor_frame = frame;
    return true;
}

void Replay_ClosePlayback() {
    g_play.active       = false;
    g_play.cursor_frame = 0;
    g_play.buf.clear();
    g_play.buf.shrink_to_fit();
}

bool Replay_IsPlaying()          { return g_play.active; }
uint32_t Replay_GetPlaybackFrame() { return g_play.cursor_frame; }

// =============================================================================
// SPECTATOR STREAMING
// =============================================================================

size_t Replay_SerializeLast(uint8_t* buf, size_t buf_size) {
    if (g_last.buf.empty()) return 0;
    if (buf_size < g_last.buf.size()) return 0;
    std::memcpy(buf, g_last.buf.data(), g_last.buf.size());
    return g_last.buf.size();
}

} // namespace Replay
