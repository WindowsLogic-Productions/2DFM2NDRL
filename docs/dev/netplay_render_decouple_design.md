# Netplay sim/render decouple -- architecture scoping

Status: **proposal / scoping** (no code). Written 2026-06-13 after the catch-up
approach was reverted (default-off).

## Goal

Heavy stages (object-heavy, ~13ms software sprite compositing) render below
100fps. Offline already holds 100fps sim via native-style frame-skip. We want
the same for **netplay**: sim at 100fps, render drops, with NO desync, NO
one-sided rollback, NO hitches.

## Why the catch-up failed (the lesson, do not repeat)

A rollback sim is **gekko-paced**, not free-running. GekkoNet's model (which our
`SleepToTarget` correctly implements, matching `Examples/OnlineSession`) is:
**one gekko cycle per loop tick, loop paced to 10ms, ×1.016 slowdown when
`frames_ahead > 0.5`** so peers converge to one timeline.

The catch-up ran *extra* gekko cycles per render off one peer's wall-clock. That
pushes that peer's frame-advantage to the prediction-window edge (FA ~+13), the
slowdown fights it to a bad equilibrium, and that peer rolls back pathologically
(soak: 1100 vs 12) while its own slowdown drags it to ~66 sim. It fights the
netcode's clock. **Adding gekko cycles to beat render cost is structurally
wrong.**

## The fundamental constraint

With a **single render thread that blocks the sim**, you cannot run sim at
100fps when a render tick is 13ms: a render tick is atomic, you cannot sleep
negative time. Two ways out:

1. Extra gekko cycles per render -- breaks gekko pacing. **Dead end (proven).**
2. **Decouple render onto its own cadence** so the sim thread never blocks on
   render. The only correct path.

## Proposed architecture (sim/render thread split)

```
SIM thread (main):  loop @ 100fps
    add_local_input -> update_session -> process Save/Load/Advance
    after each CONFIRMED advance: publish a render snapshot (lock-free swap)
    SleepToTarget(10ms, frames_ahead)   <- gekko pacing unchanged, FA stays ~0

RENDER thread:  loop @ best-effort (60-77fps on heavy stages)
    take latest published snapshot (triple-buffer, no lock on hot path)
    render it -> present
    never touches live sim state
```

Sim pacing is **untouched** (one cycle per tick, gekko-paced) -- so FA converges
exactly as today, no imbalance. Render simply consumes snapshots at its own rate
and drops frames when behind. This is the textbook decouple.

## The blockers (why this is a real project, not a patch)

1. **FM2K `render_game` reads game state from hardcoded global addresses**
   (object pool @ 0x4701E0 1024x382B, camera, palette, afterimage pool, effect
   structs, HUD). To render from a *snapshot* on another thread it must read the
   snapshot's copy, not the live addresses. Options, all costly:
   - copy snapshot INTO the live addresses before render -> sim thread can't use
     them concurrently -> back to a stall/lock (defeats the point);
   - rewrite `render_game` to read from a relocatable base -> patch hundreds of
     address references -> massive RE;
   - give the render thread a full duplicate of the game memory image -> heavy +
     render still writes DDraw globals.
   This is the crux blocker.

2. **DDraw is thread-affine.** `render_game` ends in a cnc-ddraw Blt/Flip; the
   device must be used on its owning thread. So render_game must live entirely
   on the render thread, which then needs the game state (blocker 1).

3. **Render-side mutations** (render RNG -- already isolated; effect/afterimage
   timers -- currently save/restored). On a snapshot copy these become harmless
   (they mutate the copy, not the sim) -- this part actually gets *cleaner* once
   blocker 1 is solved.

## Options + effort/risk

| Option | Effort | Risk | Heavy-netplay 100fps? |
|---|---|---|---|
| A. Accept render-bound (catch-up off) | done | none | no (both peers slow, synced, fair) |
| B. Parameterize render_game state reads + render thread | weeks | high (deep RE, every read) | yes |
| C. Full memory-image duplication for render thread | weeks | very high (DDraw write conflicts) | maybe |
| D. Reduce render cost (GPU-composite FM2K's software blit) | weeks | high (rewrite the renderer) | sidesteps it |

## Recommendation

Ship **A** (catch-up off) as the correct, shipping netplay architecture now.
Heavy netplay stages run render-bound -- both peers equally slow, synced, fair,
no desync, no one-sided rollback -- which is also what delay-based netcode always
did. The 100fps-sim-on-heavy-netplay goal lives entirely behind B/C/D, all
multi-week high-risk efforts.

**Gate before building B/C/D:** measure how often real netplay actually lands on
a >10ms-render stage. If heavy stages are rare in netplay (most matches are on
normal stages where sim already holds 100), the decouple project is not worth the
risk. Decide with data, not the one Yamada offline report (offline is already
fixed and is the actual report).

**If pursued:** start with a feasibility spike on blocker 1 only -- can
`render_game` be made to read from a relocated base cheaply (e.g., a single base
register the engine already uses) or is it truly hundreds of absolute addresses?
That spike decides whether B is days or weeks, before any thread work.

## Render anatomy (real binary: WonderfulWorld_ver_0946, render_game @ 0x404DD0)

(An earlier attempt read AALib.exe/Arcana Heart by mistake -- void. This is the
correct FM2K binary, IDA instance port 13337.)

`render_game` (0x404DD0): 2x ProcessShakeEffect, then **walk g_object_list_heads
and call `sprite_rendering_engine` (0x40CC30) per active object**, then HP bars
(cheap FillRectWithBlend), debug overlays (gated off: g_hit_judge_config /
g_replay_mode GDI text), then a fixed tail: `StridedMemoryCopy` (~0.6MB) + a
DirectDraw Flip. **The Flip/copy are fixed and small -> the heavy-stage 13ms is
NOT a vsync wait; it is the per-object sprite loop** (matches Yamada's
"tied to object count"). vsync=false / dxwrapper not helping is consistent.

`sprite_rendering_engine` (0x40CC30) cost centers, all scalar software work:
- **per sprite**: `RLEDecompress` (0x4140C0) into hMem + a **256-entry palette-LUT
  rebuild** (the `v133/v134=256` clamp loops, redone PER SPRITE) +
  `BlitSpriteWithBlendMode` (0x40C140) / `graphics_blitter` (0x40B4C0) per-pixel.
- **afterimages** (object+337): loops up to 100 trail frames, each = decompress +
  LUT rebuild + blit -> up to 100x for trail-heavy effects.
- **full-screen feedback/blur, case -10**: 638x478 per-pixel neighbor-average run
  `obj+342 / 20` PASSES -- one such object can dominate a whole frame.

## This reframes the options -- a 4th path (likely the best)

E. **Optimize the software renderer so render fits ~10ms** (SIMD the per-pixel
   blit/blur, cache the per-sprite LUT rebuild + RLE decompress which repeat).
   Pros: fixes OFFLINE and NETPLAY at once, NO threading, NO gekko-pacing
   landmines, no DDraw-affinity problem. It's a contained RE+optimization project
   on a handful of hot functions, far lower risk than the threaded rewrite (B/C).

## Revised recommendation

1. Ship A (catch-up OFF) now -- closes the regression. (done: committed 86c0319)
2. **Next: profile to RANK the cost centers** on real heavy stages -- which
   dominates: BlitSpriteWithBlendMode, the case -10 full-screen blur, RLEDecompress,
   or the per-sprite LUT rebuild? Instrument those four at runtime
   (FM2K_PERF_PROFILE-style timers) on RoHe/Aubeclisse + a heavy WW stage.
3. Optimize the dominant one(s) (Option E). If render drops under 10ms, the entire
   sim/render-decouple problem evaporates -- no frame-skip, no catch-up needed.
4. Threaded render (B/C) becomes a last resort only if E can't get under budget.

## The render profiler (BUILT + validated 2026-06-13)

Env-gated `FM2K_RENDER_PROFILE=1` hooks the per-sprite blit leaf
(`BlitSpriteWithBlendMode` @ 0x40C140, clean __cdecl) and, every 300 rendered
frames, logs one `[RENDER-PROF]` line to `<game>/logs/FM2K_P<N>_Debug.log`:

```
[RENDER-PROF] n=300 objs=<live obj count> render_game=<ms>/f
 | blit <calls>/f area=<kpx>/f time=<ms>/f
 | residual(blur+rle+lut+tail)=<ms>/f
 | blend copy=N half=N add=N sub=N alpha=N (calls/300)
```

- `render_game` = total `original_render_game()` time (existing #63 timer).
- `blit time` = measured inside the blit leaf (QPC). `residual = render_game -
  blit` = full-screen blur (case -10) + RLEDecompress + per-sprite 256-LUT
  rebuild + ddraw tail. The blur is the only ms-scale residual component, so a
  large residual ≈ a multi-pass blur object on the stage.
- `blit calls` vs `objs`: calls >> objs == afterimage-trail multiplication.
- blend mix: `add`/`sub`/`alpha` cost ~5x `copy` per pixel (object[+0x54]
  selects the mode; verified in disasm) -> they are the SIMD priority.

Hooked ONLY when the env is set, so normal play carries zero extra hooks. Wholly
display-side (no sim/rollback/determinism impact). Harness forwards the env:
`FM2K_RENDER_PROFILE=1 python3 tools/replay_netplay_selftest.py --frames 400`.

### Light-WW baseline (validated -- the "healthy" shape)

```
render_game ~1.9-2.1 ms/f | blit ~80% (1.5-1.7ms) | residual ~0.4-0.5ms (no blur)
blit calls 28-63  <  objs ~100        (no afterimage multiplication)
blend: copy ~85-90%, some add+alpha, half/sub negligible
```

Render well under 10ms -> WW holds 100fps. The blit IS the cost; there is no
blur and no trail multiplication on a light stage.

### HEAVY-stage measurement (DONE 2026-06-13 -- RoHe found at
`D:\Games\fm2k\RobotHeroes\Game\RoHe_0_7_1.exe`; offline per-stage sweep via
`tools/rohe_offline_profile.py` (now emits [RENDER-PROF]); KGT stage table:
0 Wintermane, 1 Baraga, 2 Aubeclisse, 3 Sekoia, 4 Akatia, 5 Shichari, 6 Gurish,
7 Grid, 8 MiriStage, 9 Visrum). Offline char-0 idle-in-battle, 300-frame avg:

```
idx stage       blit_ms  blit%  residual  render_max   area(nominal)
 2  Aubeclisse  10.75ms   90%    1.21ms    16.84ms      34.7 Mpx/f
 6  Gurish       7.86ms   86%    1.25ms    14.67ms      75.4 Mpx/f
 5  Shichari     7.69ms   78%    2.20ms    21.59ms      31.8 Mpx/f   <- blur, big spike
 3  Sekoia       7.49ms   87%    1.08ms    13.40ms      87.4 Mpx/f
 1  Baraga       6.43ms   75%    2.15ms    13.38ms      32.7 Mpx/f   <- blur, 99 objs
 0  Wintermane   5.51ms   84%    1.03ms    10.66ms      19.4 Mpx/f
 4  Akatia       4.64ms   83%    0.98ms                 12.7 Mpx/f
 8  MiriStage    2.92ms   72%    1.11ms                  9.0 Mpx/f
 7  Grid         1.99ms   64%    1.12ms                  4.4 Mpx/f
```

**VERDICT: every heavy stage is BLIT-BOUND.** 75-90% of render_game is
`BlitSpriteWithBlendMode` doing 20-35 Mpx/f of scalar 16-bit software
compositing, dead-linear in pixel count (~0.31ms/Mpx). Sim is ~0.2ms
everywhere. `area` is nominal (pre-clip a6*a7) so it overcounts offscreen
sprites -- blit_ms is the real cost. NOT afterimages (calls ~= objs). Baraga +
Shichari ALSO run an intermittent `case -10` full-screen blur (residual ~2.2ms
vs ~1.0 baseline = ~1 blur pass; drives the render_max spikes, Shichari 21.59).

### The fix (display-only, desync-safe -- render never feeds sim)

1. **Primary: SIMD `BlitSpriteWithBlendMode` (0x40C140).** Helps EVERY stage
   (it's 75-90% of all of them). Reimplement in-DLL, hook-replace, SSE2 the 5
   blend modes x 2 pixel formats. The per-pixel LUT lookup `LUT[idx]` is a
   gather (no SSE2 gather) -> keep the 8-bit->16-bit lookup scalar, vectorise
   the blend math (add/sub/alpha saturating per-channel = PADDUSW/PSUBUSW-style)
   which is where the costly modes live. Expect Aubeclisse 10.75 -> ~3-4ms.
2. **Secondary: SIMD the `case -10` blur** (478x638 neighbour-average, 8px/iter)
   -- kills the Baraga/Shichari spikes. Smaller, do after the blit lands.

De-risk: reimplement scalar-bit-exact FIRST (verify pixel-identical vs original,
gated FM2K_BLIT_SIMD), THEN vectorise. Render output desync-safe, so the only
risk is visual, caught by the pixel-diff check.

### Status 2026-06-13 (uncommitted on bleeding @ 86c0319)

Phase 1 DONE + VERIFIED. `FM2KHook/src/hooks/render_simd.cpp/.h`: bit-exact
SCALAR reimpl of the blit (5 modes x 565/555) + the case -10 blur, gated
`FM2K_BLIT_SIMD=scalar|simd[,verify]`, wired via the blit-leaf hook + a
`sprite_rendering_engine` (0x40CC30) hook intercepting render_mode==-10. Builds
clean. **Verify passed**: `scalar,verify` on Aubeclisse + Baraga
(`rohe_offline_profile.py`, which now forwards FM2K_BLIT_SIMD) logged
**0 `[BLIT-VERIFY]` + 0 `[BLUR-VERIFY]` mismatches** -> pixel-faithful.

Phase 2 (the speedup) NOT started: `use_simd` is a no-op, so `=simd` runs the
scalar path (~= original engine speed, no win yet). NEXT: write the SSE2 inner
loops (keep LUT[idx] lookup scalar; vectorise the blend math + blur 8px/iter),
re-verify `simd,verify`, then profile `FM2K_RENDER_PROFILE=1 FM2K_BLIT_SIMD=simd`
(NOT verify -- verify double-renders, ~3-4x slower, not a perf number). Target
Aubeclisse render 10.75 -> <10ms.

Disasm-verified field offsets: render_mode obj[+0x10], blend obj[+0x54], alpha
obj[+0x50], blur passes obj[+342]/20; sprite [+4]=stride [+8]=height;
ppvBits=0x4246CC, g_graphics_mode=0x424704 (!=0 => 565), g_object_data_ptr=0x4CFA00.
