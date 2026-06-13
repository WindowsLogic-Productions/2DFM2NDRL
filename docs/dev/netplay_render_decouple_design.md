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

## Feasibility spike result (2026-06-13)

Ran the spike (IDA on the render path, 0x40A910 + callees). Verdict: **blocker 1
is confirmed worst-case.** The render code reads game state through **absolute
hardcoded global addresses** -- e.g. `word_65BF84E`, `g_CharStats_Singleton`
(0xAA9A494), `dword_AA98440`, `g_FontMgr_LanguageBucket`, `word_6DC0D0` -- plus
`this`-relative object fields, with **no relocatable base register**. This is the
2000-era MSVC pattern and it is pervasive (every render helper -- BattleBuf_*,
Sprite_ResolveAnimSlot, Font_*, the sprite blitters -- does the same). There is
no single base to repoint.

Implication: Option B (relocate render's state reads to a snapshot) means
rewriting hundreds of absolute references across render_game and all its callees
-- multi-week, high-risk deep RE. Option C (full memory-image duplication) still
collides on the DDraw write side. **This validates the recommendation: ship A
(render-bound heavy netplay), do not build the decouple unless real-netplay data
proves heavy stages are common enough to justify weeks of RE.**
