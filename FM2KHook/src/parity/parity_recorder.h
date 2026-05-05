/* SPDX-License-Identifier: Apache-2.0 */
/* FM2KHook-side parity recorder API. See parity_recorder.cpp for context. */
#pragma once

namespace ParityRecorder {

/* Open an output .pty file. Returns true on success. */
bool Open(const char* path);

/* Capture one snapshot from FM2K's live globals. Call AFTER the frame's
 * update_game tick (so we record post-frame state, matching kgtengine's
 * post-advance recorder). Silently no-ops if no file is open. */
void Capture();

/* Flush + close. Patches the header's frame_count. */
void Close();

/* Convenience: if FM2K_PARITY_RECORD_PATH env var is set, Open() that path.
 * Call from FM2KHook's startup (DllMain attach or wherever the trampoline
 * is installed). Returns true if a file was opened. */
bool MaybeAutoOpen();

}  /* namespace ParityRecorder */
