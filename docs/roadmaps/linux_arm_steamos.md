# Plan: FM2K Rollback Launcher — Linux / ARM / SteamOS Roadmap

## Context

User has two pending hardware migrations:

1. **ARM laptop (their primary)** — currently runs PKMN CC and other FM2K games "well enough" via... presumably native Wine ARM or a CrossOver/box64 stack. They ask whether our rollback launcher will work there too.
2. **Eventual Steam Machine** as primary gaming setup — wants PKMN CC + rollback netcode there.

They also note Fightcade's JSON-script architecture isn't ARM-compatible, so they're calibrated against the worst case.

The codebase today is built strictly for 32-bit Windows: MinGW i686 cross-compile, MinHook x86 inline hooks, `CreateRemoteThread` / `VirtualAllocEx` DLL injection into the FM2K process, DirectX9 vtable hook for the in-game ImGui overlay, ImGui+SDL3 launcher UI. Everything else (GekkoNet, SDL3_net, the rollback math, the game binaries themselves) is portable.

The exploration reports overstate the porting difficulty — Wine/Proton has shipped `CreateRemoteThread`, `VirtualAllocEx`, MinHook-style inline patching, and DirectX9 vtable hooks reliably for years (ReShade, Special K, Cheat Engine all rely on these under Proton without issue). The realistic answer is:

| Target | Status | Effort |
|---|---|---|
| Steam Deck / x86_64 Linux + Proton | Should work today; needs verification | **1-2 days** |
| Generic Linux + Wine | Same as Proton, slightly more setup friction | **incidental** |
| Apple Silicon Mac via CrossOver / Game Porting Toolkit | Should work; user already runs other FM2K games this way | **test only** |
| ARM Linux (Asahi, Snapdragon) via box86 + Wine ARM | Experimental but plausible | **community-quality** |
| Native Linux launcher (no Wine) | Not worth it; would reimplement Win32 surface | **out of scope** |

The user's actual question — "what do we need to have our 2dfm client work on Linux and ARM" — is best answered as a phased rollout, not a single rewrite. Most of the work is **verification + packaging + documentation**, not code rewrite.

## Phase 1 — Steam Deck / Proton (low effort, high impact)

Goal: turn the existing Windows build into a Steam Deck citizen.

Actions:

1. **Install + run-under-Proton smoke test.** On a Linux box (or Steam Deck), install Proton 9 / Proton Experimental. Add `FM2K_RollbackLauncher.exe` as a non-Steam game. Force compat tool. Run. Triage anything that breaks.
   - Likely good: launcher boots, ImGui renders via DXVK or SDL3-renderer, lobby UI works.
   - Likely problem area: 32-bit multilib detection (Proton ships its own; should be fine — but test).
   - Critical to verify: `CreateRemoteThread` injection chain into FM2K still completes. Wine's been compatible with this for years but our specific sequence (suspend → alloc → write → remote-thread → named-event-wait → resume) has many places to stumble.

2. **Wrap the few raw winsock2 calls that bypass SDL_net.** The exploration found `socket(AF_INET, SOCK_DGRAM, ...)` + `closesocket()` + `WSAStartup()` calls in `FM2K_LauncherUI.cpp` (NAT keepalive + port auto-pick). These are *fine* under Wine but moving them to SDL_net's `NET_CreateDatagramSocket` etc. means we have one fewer Win32 dependency in the launcher and we can build a non-Wine native Linux launcher in Phase 4 if we ever want to.

3. **Bundle a Proton launch script + README section.** A small `tools/run-proton.sh` that picks a Proton version, sets the compat data dir, runs the launcher, and forwards args. Plus a `docs/dev/steamdeck.md` documenting the Add-Non-Steam-Game flow + known quirks + recommended Proton version. Users on Steam Deck shouldn't have to figure out Proton invocation themselves.

4. **CI smoke test under Wine.** Add a GitHub Actions job (or Linux dev shell script) that runs the built `FM2K_RollbackLauncher.exe` under headless Wine + xvfb, confirms the SDL window opens and the launcher exits cleanly. Catches Wine-API-regression CI signal early. Optional but cheap.

After Phase 1, the answer to the user's question 2 becomes "yes, install Proton, add as non-Steam game, it works."

## Phase 2 — Apple Silicon Mac (verification, no code)

Goal: confirm CrossOver / Game Porting Toolkit / Whisky path works for the rollback launcher.

The user already runs other FM2K games on their ARM Mac, so the underlying Wine-on-ARM stack is proven for them. We need to confirm OUR launcher/hook DLL doesn't trip anything specific to that stack:

1. **Test on Whisky (open-source CrossOver-style Apple Silicon Wine wrapper).** Same flow as Steam Deck — install Whisky, add launcher exe, run. The failure modes here are typically Apple's Rosetta + box64 path failing on specific x86 instruction sequences (rare, but possible if MinHook generates exotic trampolines).

2. **Document findings.** If it works: short `docs/dev/macos.md` page. If specific games break, list which (so the user knows that 2/3 games work and the third doesn't).

No code changes anticipated. Low priority unless Phase 1 reveals an issue this would also surface.

## Phase 3 — Generic ARM Linux (experimental)

Goal: support Asahi Linux / Snapdragon laptops / Pinebook via the box86+Wine-ARM stack.

This is community-grade compat, not first-class. Same `CreateRemoteThread` chain runs through box86's x86-instruction emulator, MinHook patches land in the emulator's recompile cache (box86 specifically tries to support runtime code patching for game mods), DirectX9 vtable hook calls cross the box86 boundary.

Actions:

1. **Test under box86 + Wine 9 ARM** on Asahi or any aarch64 Linux. Note the success/failure of: launcher boot, lobby UI, game launch, hook DLL injection, in-game overlay, actual netplay match.
2. **Document supported configurations.** Probably "works on Asahi Fedora 41 + Wine 9.0 + box86 0.3.x" or similar narrow recipe. Don't promise universal ARM Linux support.
3. **Optional: ship a Flatpak or AppImage** that bundles box86 + Wine ARM + our launcher into one runnable. Not strictly needed but reduces user setup pain.

This phase is gated on Phase 1 working — same launcher binary, so anything we fix for Proton helps here too.

## Phase 4 — (Out of scope, deliberately) Native Linux launcher

Tempting but not worth the effort:
- The launcher's GUI is already SDL3 + ImGui (portable). A Linux native build is ~3 days.
- BUT the launcher's ONLY job is to spawn an FM2K game (Windows binary) and inject our DLL. The injection is `CreateRemoteThread` into a Windows process, which requires Wine.
- A native-Linux launcher that fork/execs a Wine instance to host the game is just a thin wrapper around `wine` invocation. Marginal benefit over the same launcher running under Wine itself.
- Skip this until/unless we have a non-Wine deployment target (which we don't).

Document this decision so future-us doesn't relitigate.

## What we will NOT do

- **Native FM2KHook.dll port to ARM/Linux.** The hook DLL has to be x86 because the FM2K game binary is x86. There's no way around that — the target opcodes are x86. On ARM hosts, both the game and our hook DLL run as x86 inside the emulator's recompile pipeline.
- **Replace MinHook with a portable hooker.** No benefit; MinHook works fine under Wine. The ARM concern is a misconception — we don't need an ARM-native hooker, we need Wine ARM to handle x86 hooks (which box86 + FEX-Emu do).
- **Move FM2K games away from x86.** They're 30-year-old binaries. Not happening.

## Files to modify (Phase 1 only — Phases 2/3 are no-code or trivial scripts)

- `FM2K_LauncherUI.cpp` — wrap the ~3 raw `socket()` / `closesocket()` / `WSAStartup()` calls behind SDL_net's `NET_CreateDatagramSocket` API, matching the rest of our network code.
- `tools/run-proton.sh` (NEW) — Proton launch wrapper script.
- `docs/dev/steamdeck.md` (NEW) — user-facing Steam Deck setup guide. Include: which Proton, how to add as non-Steam game, mapping controller bindings, known quirks.
- `docs/dev/wine_compat.md` (NEW) — developer-facing notes on what Wine APIs we depend on, version requirements, debugging tips when something Wine-specific breaks.
- `.github/workflows/wine-smoke.yml` (NEW, optional) — CI Wine smoke test.

No changes needed to: build system, hook DLL, MinHook integration, GekkoNet, SDL_net, GUI rendering, the catalog tools we just built. All of those are already portable-via-Wine.

## Existing utilities to reuse

- **SDL3_net wrapped sockets** — `vendored/SDL_net/include/SDL3_net/SDL_net.h:472-1012`. Already used elsewhere in the project; the few raw winsock calls in `FM2K_LauncherUI.cpp` are anomalies that should match the rest.
- **The injection chain** — `FM2K_DLLInjector.cpp:6-138` works under Wine as-is. No code changes; only verification.
- **Cross-compile build system** — `CMakeLists.txt`, `make_build.sh`, `go.sh`. Already produces Wine-runnable PE32 binaries. No conditional Linux build needed.
- **The rest of the launcher** — ImGui+SDL3 is portable; SDL3 backends handle Wine cleanly.

## Verification

End-to-end test sequence after Phase 1 lands:

1. **Steam Deck — fresh user flow.** Wipe Steam Deck of prior FM2K state. Run `tools/run-proton.sh` (or follow `docs/dev/steamdeck.md`). Launch a single-player FM2K game (pkmncc) → confirm launcher UI appears, game window opens, hook DLL initialized (check shared-mem state from launcher), input works.
2. **Steam Deck — netplay loop.** Two-machine test: Steam Deck as client, Windows PC as host. Connect via hub, play a 3-round match. Verify W/L/D records, character ID capture, no desyncs over 100 frames.
3. **Steam Deck — spectator.** Spectate an in-progress match from Steam Deck side. Verify TCP spectator transport works through Proton's network stack (it should — SDL_net under Wine routes via the host kernel).
4. **Apple Silicon — Whisky test.** Same as #1 but on a Whisky bottle on macOS. Document any quirks (rendering, controller binding).
5. **Wine version sweep.** Test under Wine 8.0, Wine 9.0, Proton Experimental, Proton 9.0. Lowest known-good version becomes the documented requirement.

If any of the above fail, the failure narrows the scope to a specific Wine API (probably one of CreateRemoteThread / DirectX9-vtable / named-event) — at which point we either work around in our code or pin a minimum Wine version that fixed the upstream bug.

After Phase 1: Steam Deck users have a documented working path. After Phase 2: Apple Silicon users do too. Phase 3 is documented community territory. We deliberately do NOT do Phase 4.
