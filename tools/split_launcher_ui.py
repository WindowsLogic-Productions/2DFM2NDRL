#!/usr/bin/env python3
"""Brace-aware splitter for FM2K_LauncherUI.cpp.

Finds each top-level LauncherUI:: member method (and the named free functions)
by its signature, brace-matches to the exact closing brace, attaches the
contiguous leading comment block, and routes each definition to a target TU.
Pure move: bytes are copied verbatim, only relocated. The core file keeps
everything not explicitly routed.
"""
import re, sys

SRC = "FM2K_LauncherUI.cpp"

# method/free-fn name -> target stem (None = keep in core FM2K_LauncherUI.cpp)
ROUTE = {
    # menubar
    "RenderMenuBar": "menubar",
    # settings windows
    "RenderSettingsWindow": "settings", "RenderGamesFoldersBody": "settings",
    "RenderGamesFoldersWindow": "settings", "RenderDiscordAuthWindow": "settings",
    "RenderHostConfigBody": "settings", "RenderHostConfigWindow": "settings",
    # game select + replays + direct-spec
    "RenderGameSelection": "gameselect", "RenderDirectSpecInline": "gameselect",
    "ScanReplays": "gameselect", "RenderReplayBrowser": "gameselect",
    # network config + session controls
    "RenderNetworkConfig": "netcfg", "RenderConnectionStatus": "netcfg",
    "RenderInGameUI": "netcfg", "ShowNetworkDiagnostics": "netcfg",
    "ValidateNetworkConfig": "netcfg", "RenderSessionControls": "netcfg",
    # input bindings + socd + random stage
    "LoadSocdState": "input", "SaveSocdState": "input",
    "LoadRandomStageState": "input", "SaveRandomStageState": "input",
    "EnsureRandomStageLoaded": "input", "RenderInputBindingsTab": "input",
    # notifications + audio
    "LoadAudioMuteState": "notify", "SaveAudioMuteState": "notify",
    "LoadNotifyState": "notify", "SaveNotifyState": "notify",
    "FireChallengeNotification": "notify", "FireSystemNotification": "notify",
    "NotifyHubMatchEnded": "notify",
    # hub lobby/match (incl. the file-scope helpers + RenderHubPanel)
    "HubPreflightPunch": "hub", "ExtractGameHashManifest": "hub",
    "FindInstalledGameForRoom": "hub", "LauncherStunClassify": "hub",
    "RenderHubServerBody": "hub", "RenderHubServerWindow": "hub",
    "RenderRecentMatchesBody": "hub", "RenderRecentMatchesWindow": "hub",
    "RenderInProgressMatchesBody": "hub", "AppendResultsCsvRow": "hub",
    "UpdateWindowTitleWithRecord": "hub", "PushStatsToHook": "hub",
    "PushHudSystemMessage": "hub", "PollUploadQueue": "hub",
    "PollMatchOutcome": "hub", "RenderHubPanel": "hub",
}

# Signature match: a top-level definition line that ends a name with `(` and is
# either `Ret LauncherUI::Name(` , `static Ret Name(` , or `fm2k::Ret Name(` .
SIG = re.compile(
    r'^(?:static\s+|inline\s+)*[A-Za-z_][\w:<>,\s\*&]*?'      # return type
    r'(?:LauncherUI::|fm2k::)?([A-Za-z_]\w*)\s*\('             # capture name
)

def strip_for_braces(line):
    """Remove // comments, /*..*/ (single-line only), and "..." / '...' so brace
    counting ignores braces inside them. Block comments spanning lines handled
    by caller via in_block flag."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c == '/' and i+1 < n and line[i+1] == '/':
            break
        if c == '"' or c == "'":
            q = c; i += 1
            while i < n:
                if line[i] == '\\': i += 2; continue
                if line[i] == q: i += 1; break
                i += 1
            continue
        out.append(c); i += 1
    return ''.join(out)

def main():
    lines = open(SRC, encoding='utf-8', errors='surrogateescape').read().split('\n')
    N = len(lines)
    # Identify the methods region: start after we see the first routed-or-core
    # top-level definition. We scan the whole file; the HubState struct and
    # ctor etc. that aren't routed just stay in core.
    i = 0
    # find first real definition line (skip includes + struct). We treat any
    # line at col0 matching SIG with a trailing '{' (possibly next lines) as a def.
    blocks = []  # (name|None, start_idx_incl_comment, end_idx_excl)
    in_block_comment = False
    idx = 0
    def find_def_start(j):
        """If line j begins a top-level definition, return its name, else None."""
        ln = lines[j]
        if not ln or ln[0] in ' \t/#}':  # defs start at col 0, not indented/comment/preproc/closebrace
            return None
        m = SIG.match(ln)
        if not m:
            return None
        return m.group(1)

    # Pre-scan: collect (name, sig_line) for lines that are real defs (brace opens
    # within the next few lines and we can brace-match to a col0 '}').
    j = 0
    consumed_until = 0
    while j < N:
        name = find_def_start(j)
        if name is None:
            j += 1; continue
        # Attach leading comment block: walk up over contiguous comment / blank-attached lines.
        cstart = j
        k = j - 1
        # include immediately-preceding contiguous comment lines (// or block) with no blank gap
        while k >= 0 and (lines[k].lstrip().startswith('//') or lines[k].lstrip().startswith('*')
                          or lines[k].lstrip().startswith('/*')):
            cstart = k; k -= 1
        # brace-match from j
        depth = 0; seen = False; end = None; bc = False
        p = j
        while p < N:
            s = lines[p]
            # handle block comments crudely
            t = ''
            q = 0
            while q < len(s):
                if bc:
                    e = s.find('*/', q)
                    if e == -1: q = len(s);
                    else: bc = False; q = e + 2
                    continue
                if s[q] == '/' and q+1 < len(s) and s[q+1] == '*':
                    bc = True; q += 2; continue
                if s[q] == '/' and q+1 < len(s) and s[q+1] == '/':
                    break
                t += s[q]; q += 1
            tt = strip_for_braces(t if not bc else t)
            for ch in tt:
                if ch == '{': depth += 1; seen = True
                elif ch == '}':
                    depth -= 1
            if seen and depth <= 0:
                end = p
                break
            p += 1
        if end is None:
            end = j  # fallback (shouldn't happen)
        blocks.append((name, cstart, end))
        j = end + 1

    # Build outputs
    routed = {}  # stem -> list of (cstart, end)
    keep_mask = [True] * N
    for (name, cstart, end) in blocks:
        stem = ROUTE.get(name)
        if stem is None:
            continue
        routed.setdefault(stem, []).append((cstart, end))
        for x in range(cstart, end + 1):
            keep_mask[x] = False

    # prelude = lines 1..44 (includes) + using namespace lui  (0-indexed 0..43)
    inc_end = None
    for x in range(N):
        if lines[x].strip() == '#include <unordered_set>':
            inc_end = x; break
    prelude = lines[0:inc_end+1] + ['', 'using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)', '']

    BANNER = {
        'menubar':   'launcher_ui_menubar.cpp -- LauncherUI top menu bar. Split from FM2K_LauncherUI.cpp (pure move).',
        'settings':  'launcher_ui_settings.cpp -- LauncherUI settings/host-config/discord/games-folders windows. Split from FM2K_LauncherUI.cpp (pure move).',
        'gameselect':'launcher_ui_gameselect.cpp -- LauncherUI game picker + replay browser + direct-spectate. Split from FM2K_LauncherUI.cpp (pure move).',
        'netcfg':    'launcher_ui_netcfg.cpp -- LauncherUI network config + connection status + session controls. Split from FM2K_LauncherUI.cpp (pure move).',
        'input':     'launcher_ui_input.cpp -- LauncherUI input-bindings tab + SOCD/random-stage state. Split from FM2K_LauncherUI.cpp (pure move).',
        'notify':    'launcher_ui_notify.cpp -- LauncherUI notifications + audio-mute state. Split from FM2K_LauncherUI.cpp (pure move).',
        'hub':       'launcher_ui_hub.cpp -- LauncherUI hub/lobby panel + match polling + hub helpers. Split from FM2K_LauncherUI.cpp. NOTE: RenderHubPanel is large; flagged for follow-up factoring.',
    }

    for stem, spans in routed.items():
        spans.sort()
        out = ['// ' + BANNER[stem]] + prelude[:]
        for (cstart, end) in spans:
            out += lines[cstart:end+1]
            out.append('')
        open(f'launcher_ui_{stem}.cpp', 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(out) + '\n')
        print(f'launcher_ui_{stem}.cpp: {sum(e-c+1 for c,e in spans)} moved lines across {len(spans)} blocks')

    # rewrite core
    core = [lines[x] for x in range(N) if keep_mask[x]]
    open(SRC, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(core))
    print(f'core {SRC}: {len(core)} lines (was {N})')

if __name__ == '__main__':
    main()
