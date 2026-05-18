#pragma once

// Minimal inline ZIP writer. STORED method only (no compression).
// Ported from /mnt/c/dev/bbbr/revolve_input_sdl3/src/rollback/zip_writer.h
// (same project owner; same rationale below).
//
// Why inline instead of shelling out to PowerShell / tar:
//   - PowerShell takes 1-3s to start a host instance on Windows. During
//     that wait the game's other threads keep running on torn state and
//     tend to crash on stale pointers.
//   - WaitForSingleObject blocks the caller's thread but doesn't stop
//     the other threads. DETACHED_PROCESS leaves us at the mercy of the
//     OS reaping the child if our parent dies.
//
// Inline zip writing runs in the caller's thread, finishes in tens of
// ms for a typical desync bundle (~3-4 MB), and is guaranteed to
// complete before we TerminateProcess.

#include <cstdint>
#include <string>
#include <vector>

namespace fm2k::util::zip {

// Write `files` (absolute or cwd-relative paths) into a STORED-method
// ZIP at `output_path`. Files that can't be opened are silently skipped
// (we don't want the bundle to fail because of one missing log).
//
// Returns the number of files actually written. 0 = nothing made it
// in (output file is deleted in that case). Output file is created /
// truncated; existing contents discarded.
int WriteZip(const std::string& output_path,
             const std::vector<std::string>& files);

}  // namespace fm2k::util::zip
