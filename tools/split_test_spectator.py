#!/usr/bin/env python3
"""One-shot splitter for tests/test_spectator_protocol.cpp (1256 lines).

doctest TEST_CASEs auto-aggregate across TUs (test_main.cpp owns the runner),
so this safely splits the file at the §2/§3 boundary:
  tests/test_spectator_protocol.cpp            §1 header/backfill + §2 events
  tests/test_spectator_protocol_sessionfile.cpp §3 file + §4 seek + §5 fp + §6 snapshot
The only cross-section dependency is mirror_seek (§4) -> mirror_event (§2), so
mirror_event is lifted into tests/spectator_protocol_events.h (free fns made
inline for multi-TU inclusion). Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("tests/test_spectator_protocol.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 1200 or not any("namespace mirror_event" in ln for ln in L):
    raise SystemExit("test_spectator_protocol.cpp is not the original "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

# ---- shared header: mirror_event (free fns -> inline) ----
ev = R(219, 403)
out = []
for ln in ev.splitlines(keepends=True):
    if ln.startswith("size_t ") or ln.startswith("bool "):
        out.append("inline " + ln)
    else:
        out.append(ln)
ev_inline = "".join(out)
header = (
    "#pragma once\n"
    "// Shared mirror of the SessionEvent wire protocol (split from\n"
    "// test_spectator_protocol.cpp). Used by both the event encode/decode tests\n"
    "// and the session-file/seek tests. Free functions are inline for multi-TU use.\n"
    "#include <cstdint>\n#include <cstring>\n#include <vector>\n\n"
    + ev_inline
)
pathlib.Path("tests/spectator_protocol_events.h").write_text(header)

EV_INC = '#include "spectator_protocol_events.h"\n'

# ---- File A: §1 (header/backfill) + §2 (events tests), mirror_event removed ----
a = R(1, 218) + R(405, 712)
a = a.replace('#include "../FM2KHook/src/netplay/replay.h"\n',
              '#include "../FM2KHook/src/netplay/replay.h"\n' + EV_INC, 1)
SRC.write_text(a)

# ---- File B: §3-6 (session file / seek / fletcher / snapshot) ----
b = []
b.append('// test_spectator_protocol_sessionfile.cpp -- session-file layout, seek,\n')
b.append('// fingerprint + snapshot wire tests. Split from test_spectator_protocol.cpp;\n')
b.append('// shares the SessionEvent mirror via spectator_protocol_events.h.\n')
b.append(R(1, 19))           # same include preamble (doctest + std + replay.h)
b.append(EV_INC)
b.append("\n")
b.append(R(714, len(L)))     # §3-6 verbatim
pathlib.Path("tests/test_spectator_protocol_sessionfile.cpp").write_text("".join(b))

print("split done:")
for f in ("test_spectator_protocol.cpp", "test_spectator_protocol_sessionfile.cpp",
          "spectator_protocol_events.h"):
    p = pathlib.Path("tests") / f
    print(f"  {f:42s} {sum(1 for _ in p.open())} lines")
