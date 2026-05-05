#include "game_hash.h"

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "../../../vendored/xxhash/xxhash.h"

namespace fm2k::game_hash {

namespace {

uint32_t s_cached_hash = 0;
bool     s_computed    = false;
char     s_describe[128] = {};

std::filesystem::path GameDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

// UTF-8 path conversion via Win32 wide APIs. std::filesystem::path's
// narrow accessors go through the system ANSI codepage on MinGW and
// would mangle JP filenames into '_' / '?' — different output on a
// Japanese-locale peer vs an English-locale peer for the same file
// on disk, which means different hashes for #57's handshake check.
// Using wide → UTF-8 makes the canonical bytes identical everywhere.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

bool MatchesExt(const std::filesystem::path& p, const char* dotext) {
    // Extensions are pure ASCII for our purposes; using wide here is
    // belt-and-suspenders. tolower on ASCII stays safe.
    std::string e = WideToUtf8(p.extension().wstring());
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == dotext;
}

}  // namespace

uint32_t Compute() {
    if (s_computed) return s_cached_hash;
    s_computed = true;

    auto dir = GameDir();
    if (dir.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameHash: GetModuleFileNameA failed; hash=0");
        s_cached_hash = 0;
        return 0;
    }

    // Per-extension policy:
    //   .player  → name + size only. Palette edits in a .player are
    //              gameplay-neutral (different colors, same frame data /
    //              hitboxes / scripts) and don't desync — content
    //              hashing them would reject those legitimate setups.
    //              Palette swaps overwrite bytes in-place so they keep
    //              size constant; size-changing edits (added frames,
    //              new scripts) get caught by the size delta.
    //   .kgt     → name + size + xxhash of contents. The compiled
    //              gamesystem owns stage data, system globals, and
    //              cross-character logic; bytes must match exactly or
    //              gameplay diverges.
    //   .exe     → name + size + xxhash of contents. Engine version
    //              must match; different patches change subtle
    //              simulation behavior.
    struct Entry {
        std::string  name_lower;
        uint64_t     size;
        XXH64_hash_t content_hash;   // 0 when not content-hashed
    };
    std::vector<Entry> entries;

    int n_player = 0, n_kgt = 0, n_exe = 0;

    auto hash_file_contents = [](const std::filesystem::path& p) -> XXH64_hash_t {
        // Stream-friendly: read in 1 MiB chunks so a multi-MB .kgt
        // doesn't allocate a giant buffer up front. xxhash supports
        // streaming via XXH3_64bits_update / digest, but XXH_NO_STREAM
        // strips that — so for these small files we read whole and
        // call the one-shot XXH3_64bits.
        std::error_code ec;
        std::uintmax_t sz = std::filesystem::file_size(p, ec);
        if (ec) return 0;
        // _wfopen (Win32 wide-char fopen) so JP filenames open
        // reliably regardless of the process's ANSI codepage.
        // Standard fopen with path::string().c_str() on MinGW would
        // try the mangled CP1252 form and fail on non-ASCII paths.
        FILE* f = _wfopen(p.wstring().c_str(), L"rb");
        if (!f) return 0;
        std::vector<uint8_t> buf((size_t)sz);
        size_t read = std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (read != buf.size()) return 0;
        return XXH3_64bits(buf.data(), buf.size());
    };

    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto& p = e.path();
        const bool is_player = MatchesExt(p, ".player");
        const bool is_kgt    = MatchesExt(p, ".kgt");
        const bool is_exe    = MatchesExt(p, ".exe");
        if (!is_player && !is_kgt && !is_exe) continue;

        // Wide → UTF-8 so the canonical hash input is identical bytes
        // on every peer regardless of stdlib narrow-conversion quirks.
        std::string name = WideToUtf8(p.filename().wstring());
        std::string lower = name;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

        uint64_t sz = 0;
        std::error_code ec2;
        sz = (uint64_t)std::filesystem::file_size(p, ec2);
        if (ec2) sz = 0;

        XXH64_hash_t ch = 0;
        if (is_kgt || is_exe) {
            ch = hash_file_contents(p);
        }

        entries.push_back({std::move(lower), sz, ch});
        if (is_player) ++n_player;
        else if (is_kgt) ++n_kgt;
        else if (is_exe) ++n_exe;
    }

    // Sort lexicographically so peer order doesn't matter.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.name_lower < b.name_lower;
              });

    // Canonical input: "<name>|<size>|<content_hash_hex_or_->\n" per entry.
    std::string canon;
    canon.reserve(entries.size() * 64);
    for (const auto& e : entries) {
        canon.append(e.name_lower);
        canon.push_back('|');
        char szbuf[32];
        std::snprintf(szbuf, sizeof(szbuf), "%llu",
                      (unsigned long long)e.size);
        canon.append(szbuf);
        canon.push_back('|');
        if (e.content_hash != 0) {
            char hbuf[24];
            std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                          (unsigned long long)e.content_hash);
            canon.append(hbuf);
        } else {
            canon.push_back('-');
        }
        canon.push_back('\n');
    }

    // xxhash3-64 → fold to 32 bits. Deterministic across architectures
    // for the same byte input. We just need enough bits to make
    // accidental collision negligible at lobby scale.
    XXH64_hash_t h = (entries.empty())
        ? 0
        : XXH3_64bits(canon.data(), canon.size());
    s_cached_hash = (uint32_t)(h ^ (h >> 32));
    // Reserve 0 as "no hash" sentinel for backwards compat. If the
    // legitimate hash collides with 0, bump it to 1.
    if (s_cached_hash == 0 && !entries.empty()) s_cached_hash = 1;

    std::snprintf(s_describe, sizeof(s_describe),
                  "%d .player + %d .kgt + %d .exe — 0x%08X",
                  n_player, n_kgt, n_exe, s_cached_hash);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GameHash: %s", s_describe);
    return s_cached_hash;
}

const char* DescribeLocal() { return s_describe; }

}  // namespace fm2k::game_hash
