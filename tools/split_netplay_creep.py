#!/usr/bin/env python3
"""Creep splits for the two near-limit hook netplay files (pure single-fn moves).
  netplay.cpp 992 -> -HandleDesyncDetected (148-290) -> netplay_desync.cpp
  netplay_battle.cpp 928 -> -Netplay_EndBattle (701-end) -> netplay_battle_end.cpp
Both functions are already declared in netplay_internal.h / netplay.h. Shared
include block reused. Ranges 1-based inclusive, verified against the read.
"""
import pathlib

NP = pathlib.Path("FM2KHook/src/netplay/netplay.cpp")
NB = pathlib.Path("FM2KHook/src/netplay/netplay_battle.cpp")
np = NP.read_text().splitlines(keepends=True)
nb = NB.read_text().splitlines(keepends=True)
if len(np) < 950 or not any("void HandleDesyncDetected(" in l for l in np):
    raise SystemExit("netplay.cpp not original (already split?)")
if len(nb) < 900 or not any("void Netplay_EndBattle()" in l for l in nb):
    raise SystemExit("netplay_battle.cpp not original (already split?)")
def R(L, a, b):
    return "".join(L[a-1:b])

INC = R(np, 5, 32)  # shared netplay include block

# ---- netplay_desync.cpp (HandleDesyncDetected + its doc comment) ----
desync = (
    "// netplay_desync.cpp -- HandleDesyncDetected: the common real+synthetic\n"
    "// desync handler (diagnostic dump -> RNG flush -> ZIP bundle -> upload\n"
    "// manifest -> TerminateProcess). Split from netplay.cpp; declared in\n"
    "// netplay_internal.h. Shares file-scope state via that header.\n"
    + INC + "\n" + R(np, 148, 290)
)
pathlib.Path("FM2KHook/src/netplay/netplay_desync.cpp").write_text(desync)
NP.write_text(R(np, 1, 147) + R(np, 291, len(np)))

# ---- netplay_battle_end.cpp (Netplay_EndBattle) ----
INCB = R(nb, 8, 35)
end = (
    "// netplay_battle_end.cpp -- Netplay_EndBattle: battle-session teardown +\n"
    "// match-outcome capture. Split from netplay_battle.cpp; declared in netplay.h.\n"
    + INCB + "\n" + R(nb, 701, len(nb))
)
pathlib.Path("FM2KHook/src/netplay/netplay_battle_end.cpp").write_text(end)
NB.write_text(R(nb, 1, 700))

print("split done:")
for f in ("netplay.cpp", "netplay_desync.cpp", "netplay_battle.cpp", "netplay_battle_end.cpp"):
    p = pathlib.Path("FM2KHook/src/netplay") / f
    print(f"  {f:26s} {sum(1 for _ in p.open())} lines")
