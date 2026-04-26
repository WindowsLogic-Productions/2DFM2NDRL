// Replay — record match inputs + metadata for playback and spectator streaming.
//
// Philosophy: record the MINIMUM deterministic reconstruction data. With
// (initial RNG seed + char selects + per-frame P1/P2 inputs + start state hash)
// we can replay the whole match by driving the trampoline from the input log.
// No save states in the replay — determinism is enforced elsewhere (§2 of
// FM2K_Rollback_Production.md).
//
// File format: .fm2krep (Fighter Maker 2K REPlay). Little-endian on wire.
//
//   ReplayHeader  (fixed, 96 bytes)
//   ReplayFrame[] (variable, 4 bytes per frame: p1 + p2 inputs as uint16)
//
// Size budget: 3-minute match @ 100 FPS = 18000 frames * 4 bytes = 72 KB. Tiny.
#pragma once

#include <cstdint>
#include <cstddef>

namespace Replay {

constexpr uint32_t REPLAY_MAGIC   = 0x52504D46;  // 'FM2KREP' tag (FMPR little-endian)
constexpr uint16_t REPLAY_VERSION = 1;

#pragma pack(push, 1)
struct ReplayHeader {
    uint32_t magic;              // REPLAY_MAGIC
    uint16_t version;            // REPLAY_VERSION
    uint16_t flags;              // Reserved
    uint64_t unix_timestamp;     // When the match started
    uint32_t game_hash;          // FM2K game identifier (for cross-variant replays)
    uint32_t initial_rng_seed;   // RNG seed at battle start — required for determinism
    uint32_t initial_state_hash; // Fingerprint at battle start — sanity check on playback
    uint8_t  p1_char_slot;       // Character selections
    uint8_t  p1_color;
    uint8_t  p2_char_slot;
    uint8_t  p2_color;
    uint8_t  p1_name[24];        // Display names (null-padded)
    uint8_t  p2_name[24];
    uint8_t  stage_id;           // Reserved
    uint8_t  reserved[11];       // Pad so frame_count lands at offset 92 (total size = 96)
    uint32_t frame_count;        // Written on finalize — number of ReplayFrame entries that follow
};
static_assert(sizeof(ReplayHeader) == 96, "ReplayHeader must be 96 bytes");

struct ReplayFrame {
    uint16_t p1_input;           // 11-bit FM2K input mask
    uint16_t p2_input;
};
static_assert(sizeof(ReplayFrame) == 4, "ReplayFrame must be 4 bytes");
#pragma pack(pop)

// =============================================================================
// RECORDING
// =============================================================================

// Called at battle start. Opens a replay file in replays/YYYY-MM-DD_HHMMSS.fm2krep,
// writes a tentative header (frame_count=0 — finalized in Replay_EndRecording).
// game_hash identifies the FM2K variant; initial_rng_seed is captured from
// ADDR_RANDOM_SEED at the call site; char selections come from CSS state.
// Returns true on success.
bool Replay_BeginRecording(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color, const char* p1_name,
    uint8_t p2_char, uint8_t p2_color, const char* p2_name);

// Called every confirmed frame (GekkoNet AdvanceEvent). Appends a ReplayFrame.
// No-op if recording isn't active. Never called during rollback replay —
// we only record forward-sim confirmed frames.
void Replay_RecordFrame(uint16_t p1_input, uint16_t p2_input);

// Called at battle end. Rewrites the header with final frame_count and closes.
// Safe to call even if Replay_BeginRecording was never called.
void Replay_EndRecording();

// Is a recording currently active?
bool Replay_IsRecording();

// =============================================================================
// PLAYBACK
// =============================================================================

// Open a replay file for playback. Validates magic+version, reads header.
// Returns false on error (missing file, bad magic, version mismatch).
bool Replay_OpenForPlayback(const char* file_path);

// Get the loaded header. Valid after Replay_OpenForPlayback succeeds.
const ReplayHeader* Replay_GetHeader();

// Fetch the next frame's inputs. Returns false at end-of-replay.
bool Replay_NextFrame(uint16_t* p1_input, uint16_t* p2_input);

// Seek to a specific frame (for rewind / fast-forward). 0-indexed.
bool Replay_SeekFrame(uint32_t frame);

// Close the current playback file.
void Replay_ClosePlayback();

// Is playback currently active?
bool Replay_IsPlaying();

// Current playback frame cursor.
uint32_t Replay_GetPlaybackFrame();

// =============================================================================
// SPECTATOR STREAMING (§9 of master doc)
// =============================================================================
// Spectator = replay streamer. Host streams the just-played replay over the
// control channel; spectator receives it into a memory-backed replay, then
// plays it back locally via the playback API above.
//
// These helpers marshal a completed in-memory replay to/from bytes so the
// control channel can chunk and send it.

// Serialize the last-completed recording to a caller-provided buffer.
// Returns the number of bytes written, or 0 if no completed replay is held.
size_t Replay_SerializeLast(uint8_t* buf, size_t buf_size);

// Load a serialized replay from a byte buffer into the playback cursor.
// Same effect as Replay_OpenForPlayback on a file, but from memory.
bool Replay_LoadFromBuffer(const uint8_t* buf, size_t buf_size);

} // namespace Replay
