#!/usr/bin/env python3
"""Pull the engine-sniffing cluster out of launcher/discovery/game_discovery.cpp
(920) into game_discovery_sniff.cpp: HashFileXXH64 + kKnownExes + FindKnownExe +
SniffEngineFromStrings + GuessEngineFromSize + DetectPackerFromPE. The KnownExe
struct + the 5 fn decls were promoted to game_discovery_internal.h. Functions are
un-static'd. Ranges 1-based inclusive, verified against the read.
"""
import pathlib

F = pathlib.Path("launcher/discovery/game_discovery.cpp")
L = F.read_text().splitlines(keepends=True)
if len(L) < 880 or not any("static uint64_t HashFileXXH64(" in l for l in L):
    raise SystemExit("game_discovery.cpp not original (already split?)")
def R(a, b):
    return "".join(L[a-1:b])

# sniff cluster body (un-static the 5 functions). Skip the KnownExe struct
# (307-311; now in internal.h) but keep the kKnownExes table.
body = R(285, 306) + R(312, 439)
for sig in (
    "static uint64_t HashFileXXH64(",
    "static const KnownExe* FindKnownExe(",
    "static std::optional<FM2K::Engine> SniffEngineFromStrings(",
    "static FM2K::Engine GuessEngineFromSize(",
    "static std::string DetectPackerFromPE(",
):
    body = body.replace(sig, sig[len("static "):], 1)

sniff = (
    "// game_discovery_sniff.cpp -- exe/engine identification (split from\n"
    "// game_discovery.cpp): xxhash + known-build registry + string/size sniff +\n"
    "// PE packer detect. The scan core calls these on a cache miss. KnownExe +\n"
    "// the decls live in game_discovery_internal.h.\n"
    '#include "game_discovery.h"\n'
    '#include "game_discovery_internal.h"\n'
    '#include "FM2K_Integration.h"\n'
    '#include "SDL3/SDL.h"\n'
    "#define XXH_INLINE_ALL\n"
    '#include "vendored/xxhash/xxhash.h"\n'
    "#include <windows.h>\n"
    "#include <cstdint>\n"
    "#include <cstring>\n"
    "#include <optional>\n"
    "#include <string>\n"
    "#include <vector>\n"
    "#include <fstream>\n"
    "#include <filesystem>\n\n"
    "namespace Utils {\n\n"
    + body +
    "\n}  // namespace Utils\n"
)
pathlib.Path("launcher/discovery/game_discovery_sniff.cpp").write_text(sniff)

# core: remove the sniff cluster (281-439, incl HashFileXXH64 lead comment).
F.write_text(R(1, 280) + R(440, len(L)))

print("split done:")
for f in ("game_discovery.cpp", "game_discovery_sniff.cpp"):
    p = pathlib.Path("launcher/discovery") / f
    print(f"  {f:28s} {sum(1 for _ in p.open())} lines")
