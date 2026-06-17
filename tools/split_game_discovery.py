#!/usr/bin/env python3
"""One-shot splitter for game_discovery.cpp (1191 lines, launcher).

Pure behaviour-preserving move: extracts the cache serialization block
(Utf8/Wide widen, CanonicalizePath, binary Write*/ReadStr, SaveGameCache,
LoadGameCacheMap, LoadGameCache) into game_discovery_cache.cpp, leaving the
filesystem scan + exe/engine sniffing + async worker in game_discovery.cpp.
GameCacheEntry + the 4 cross-TU helpers are shared via game_discovery_internal.h
(already written). Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("game_discovery.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 1100 or not any("LoadGameCacheMap()" in ln for ln in L):
    raise SystemExit("game_discovery.cpp is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

INCLUDES = R(5, 47)   # game_discovery.h + all the launcher/SDL/win includes
INT_INC  = '#include "game_discovery_internal.h"\n'

# ---- cache-IO TU ----
cache = []
cache.append('// game_discovery_cache.cpp -- games.cache binary serialization +\n')
cache.append('// path canonicalization. Split from game_discovery.cpp (pure move).\n')
cache.append('// Shares GameCacheEntry + GetCacheFilePath/StatFile via the internal header.\n')
cache.append(INCLUDES)
cache.append(INT_INC)
cache.append("\n")
cache.append("namespace Utils {\n\n")
cache_body = R(466, 717)
cache_body = cache_body.replace("static std::string CanonicalizePath(",
                                "std::string CanonicalizePath(")
cache.append(cache_body)
cache.append("\n}  // namespace Utils\n")
pathlib.Path("game_discovery_cache.cpp").write_text("".join(cache))

# ---- core (overwrites SRC) ----
# 1-261 (includes + Utils open + general/config helpers up to the comment
# before GameCacheEntry) ; skip 262-281 (GameCacheEntry -> internal.h) ;
# 282-465 (GetCacheFilePath..DetectPackerFromPE) ; skip 466-717 (cache block) ;
# 718-end (NormalizePath + recursive find + Utils close + scan/discovery).
core = R(1, 261) + R(282, 465) + R(718, len(L))
# Insert the internal-header include right after game_discovery.h.
core = core.replace('#include "game_discovery.h"\n',
                    '#include "game_discovery.h"\n' + INT_INC, 1)
# De-static the two helpers the cache TU now calls.
core = core.replace("static std::string GetCacheFilePath()",
                    "std::string GetCacheFilePath()", 1)
core = core.replace("static bool StatFile(const std::string& path, uint64_t& size, int64_t& mtime)",
                    "bool StatFile(const std::string& path, uint64_t& size, int64_t& mtime)", 1)
SRC.write_text(core)

print("split done:")
for f in ("game_discovery.cpp", "game_discovery_cache.cpp"):
    print(f"  {f:28s} {sum(1 for _ in open(f))} lines")
