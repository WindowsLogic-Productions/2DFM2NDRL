# Random-Stage Feature — Design Doc

Status: design only, no code yet. Target tasks: composes with #53 (parse `[GamePlay]`), #54 (sync to client), #55 (per-game overrides), unblocks #56.

---

## 1. How LilithPort does it

### 1.1 INI fields

LilithPort stores stage config per profile (one profile per FM2K game) in `LilithPort.ini`. From `LilithPort/stdafx.cpp:205-206` (read) and `:399-401` (write):

```
[<ProfileName>]
MaxStage=<N>          ; default 1
StageSelect=<S>       ; default 0
```

### 1.2 Range semantics (the clever overload)

Both fields are 1-based stage numbers. The **relationship** between them encodes three different modes — there is no separate "random enable" checkbox:

| Condition                       | Mode             | Behavior                                                                 |
|---------------------------------|------------------|--------------------------------------------------------------------------|
| `StageSelect == 0`              | **Random**       | Pick a stage uniformly from `1..MaxStage` each match.                    |
| `StageSelect <= MaxStage` (≠0)  | **Fixed**        | Always play stage `StageSelect`. `MaxStage` is ignored at match time.    |
| `StageSelect > MaxStage`        | **Cycle / loop** | Rotate stages each match: `MaxStage, MaxStage+1, ..., StageSelect`, repeat. (Used for "play through all stages" sets.) |

Visible to the user via dynamic relabeling in `OptionForm.h:2156-2169`: the labels morph between *"Max stage / Match stage"*, *"Max stage / Random stage"*, and *"Start stage / End stage"* depending on the relationship — clever, but completely undiscoverable.

The README confirms (`README.md:79-80`): *"Random stage: if 0, the stage will be selected at random; if the value is greater than the maximum number of stages, it will cycle through stages."*

### 1.3 What stage 0 means in FM2K vs in `StageSelect`

- In FM2K's own `[GamePlay]` `Editor.TestPlay.StageNb`, stage `0` is a real, valid first stage.
- In LilithPort's `StageSelect`, **`0` is the "random" sentinel**, NOT stage zero. Stage 1 in LilithPort UI maps to FM2K stage index 0. See `MainForm.cpp:2464-2469`:
  ```cpp
  if(MTINFO.STAGE_SELECT == 0){
      i = RandomStage() % MTINFO.MAX_STAGE;     // 0..MaxStage-1
  } else {
      i = MTINFO.STAGE_SELECT - 1;              // 1-based -> 0-based
  }
  ```
  `i` is then written to the game's stage register via `c.Eax = i`. So Lilith's `MaxStage=5, StageSelect=0` → uniform pick over FM2K stage indices `0..4`.

### 1.4 Where the randomization happens

**Per-match, on the host's machine**, intercepted at the moment FM2K reads its stage variable. The host is acting as a debugger (`CreateProcess` with `DEBUG_PROCESS`); it sets a soft int3 at `STAGE_SELECT = 0x00408756` (or `STAGE_SELECT_95 = 0x0041162C` for FM95), and on the breakpoint hit it patches the EAX/ECX register to the chosen stage index (`MainForm.cpp:2438-2480`). That single function services all three modes (random / fixed / cycle).

For the **cycle** mode (`StageSelect > MaxStage`), there is a `stage_loop` counter local to `RunGame`'s frame loop that advances each time STAGE_SELECT is hit, so subsequent matches in the same session walk through the range. This is incidental in-process state — not networked.

### 1.5 How the result reaches the peer

LilithPort does **not** transmit the chosen stage. Instead it synchronizes the **PRNG seed** at match start, and both peers run the same xorshift locally:

- Host generates `MTINFO.SEED = XorShift()` once per match (`MainForm.cpp:1297`).
- Host packs `[seed:4][max_stage:1][stage_select:1][round:1][timer:1][team_round_hp:1]` into a `PH_RES_VS_SETTING` packet (`MainForm.cpp:1326-1331`, header definition `stdafx.h:447`).
- Client receives it and copies the four match-config bytes wholesale (`MainForm.cpp:1373-1377`) — no negotiation; **host's settings always win**.
- Both sides call `RandomStage(MTINFO.SEED)` in `RunGame` at frame 0 (`MainForm.cpp:2222`), which seeds an identical xorshift state on both machines (`stdafx.cpp:670-687`).
- Each subsequent `RandomStage()` call (no arg) draws from the synced sequence, so both peers compute the same stage index without ever transmitting it.

This is a nice trick but brittle: a single dropped/duplicated `STAGE_SELECT` breakpoint on one side desyncs the stage stream forever, and there's no resync path.

### 1.6 Per-game stage list

LilithPort does **not** know how many stages a game has. It trusts the user's `MaxStage` value. The README warns: *"Set this to 1 if unsure. If this number is higher than the real number of stages in the game, the game will crash."* No validation, no autodetect.

### 1.7 Spectator path

`PH_RES_WATCH` (`stdafx.h:453`) carries the same seed + max_stage + stage_select bytes, so spectators reproduce the host's stage stream identically. We should mirror this.

---

## 2. Recommendation for our launcher

### 2.1 Where the config lives — **per-host**, with a per-game-aware UI

The user's open question was per-player vs per-host vs per-game. Recommendation:

- **The match-time random pool is per-host** (the challenger / room host). Host's `[Host]` config wins, just like SOCD and `SelectedStage` already do today. This is what LilithPort effectively does (host's `MaxStage`/`StageSelect` overwrites the client's via `PH_RES_VS_SETTING`) and it matches our `host_config` flow.
- **The default value is sourced from the per-game `[GamePlay]` `Editor.TestPlay.StageNb`** parsed by #53 — when the host opens Host Config for the first time after picking a game, prefill `MaxStage = max(1, StageNb_observed_or_known)` and leave `RandomEnable=false`.
- **We do NOT make this a per-player launcher setting.** Two players each having their own "max stage" leads to a peer rolling a stage the other doesn't have. Keep authority on one side.

So: storage in `fm2k_host.ini` (extended, see §2.5), defaults seeded from the per-game ini, broadcast over our existing host-config channel.

### 2.2 Where randomization happens — **host rolls once at match start, broadcasts the result**

Do **not** copy LilithPort's seed-sync approach. We have a control channel that already broadcasts `host_config` reliably (with retransmit). Just put the rolled stage index in that packet:

1. Host UI captures `RandomEnable` + `RandomMaxStage` (and optionally `RandomMinStage`).
2. On challenge accept (the moment we already build `host_config` today, see `FM2KHook/src/netplay/netplay.cpp:169-185`), if `RandomEnable` is set, the host rolls `selected_stage = uniform_int(min..max)` using `std::mt19937` seeded from `std::random_device` — **once per match**.
3. The rolled value goes into the existing `host_config.selected_stage` field. Client + spectators receive it via the existing `CtrlPacket` dispatch (`netplay.cpp:331-344`) and write to `0x470188` exactly as they do today.

Advantages over LilithPort:
- **No PRNG state divergence risk.** The actual stage number is what crosses the wire.
- **Replays/spectators get the right stage automatically** because the same packet feeds them.
- **Zero new protocol surface** — we reuse `host_config.selected_stage` unchanged. The randomness is invisible to the client.
- **Per-round / per-set policies become trivial** (just re-roll on match end before the next `host_config` broadcast).

### 2.3 Range mapping to FM2K

Use plain 1-based UI, plain 0-based wire — **don't** copy LilithPort's `StageSelect=0 means random` overload:

- UI displays "Stages 1..N" (matches what users see in the editor and in Lilith).
- Internally `random_min`/`random_max` are stored 1-based.
- After rolling: `selected_stage = roll - 1` (0-based) before placing in the packet, since `0x470188` and `Editor.TestPlay.StageNb` are both 0-indexed (StageNb defaults to 0 = first stage).
- Sentinel: keep the existing `host_config.selected_stage == 0xFFFFFFFF` "unset" path. When `RandomEnable=false` and the user hasn't picked a stage either, leave it `0xFFFFFFFF` (game uses its own default).

### 2.4 UI shape (Settings → Host Config)

Layout extension to the existing Host Config panel (`FM2K_LauncherUI.cpp:1671-1709`):

```
Selected stage: [  3 ]   ( disabled when [x] Random pool enabled )
[x] Random pool enabled
    Random pool: stages [ 1 ] .. [ N ]
    (defaults sourced from <gamename>'s 2dfm.ini StageNb)
```

- Default `RandomEnable=false` so existing behavior is unchanged.
- When enabled, the fixed `Selected stage` input is greyed out (it's the *outcome*, not the *setting*).
- `N` upper bound has no hard cap in the UI; the warning text reads *"If you pick a number higher than the game's stage count, the match will crash."* (Same warning as Lilith — we cannot know the real count without parsing the game's `KGT2nd_*.exe` data, which is out of scope.)
- When `RandomEnable=true`, immediately after Apply we can optionally surface the *next* roll preview in the lobby ("Next stage: random 1..7"), but the actual roll happens at challenge time, not Apply time.

### 2.5 `fm2k_host.ini` extension

```ini
[Host]
SelectedStage=3                ; existing — used when RandomEnable=0
SOCDMode=1                     ; existing
RandomEnable=0                 ; new, default 0
RandomStageMin=1               ; new, 1-based, default 1
RandomStageMax=1               ; new, 1-based, default 1 (must seed from per-game)
```

Backwards compatible: missing keys → `RandomEnable=0`, behavior identical to today.

### 2.6 Edge cases

| Case                                              | Behavior                                                                                                                                                                                                |
|---------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Peers disagree on N                               | Doesn't matter — host rolls and sends the resolved index. Client just writes whatever `selected_stage` it received.                                                                                     |
| Host rolls a stage the client's game files lack   | Same failure mode as today's manual "Selected stage = 99" — game crashes on that side. Mitigation: same as Lilith (warn in UI, no autodetect). Future: hash game data dirs to verify stage presence.    |
| `RandomEnable=true`, `RandomStageMax < Min`       | Clamp `Max = max(Min, Max)` on save; UI prevents this state.                                                                                                                                            |
| `RandomEnable=true` but no game selected yet      | Apply is disabled until a game is chosen (we already gate on game selection elsewhere).                                                                                                                 |
| Spectator joins mid-match                         | Already covered: spectators replay the last-broadcast `host_config`, which contains the resolved stage. No change.                                                                                      |
| Rematch / `match_start` round 2                   | Phase 1: keep the same stage for the whole set (one roll per challenge). Phase 2 (optional): re-roll on rematch — host emits a fresh `host_config` before each match. Out of scope for the initial PR. |
| Per-game `Editor.TestPlay.StageNb` is 0           | That's just "default = first stage". Use it as the prefill for `Min=1, Max=1` (no randomness until the user widens the range).                                                                          |

### 2.7 Composition with #53/#54/#55

- **#53 (parse `[GamePlay]`)** — already done in `FM2K_GameIni.cpp:28`. We only *consume* `stage_nb` to seed UI defaults; we don't need it at runtime.
- **#54 (sync `[GamePlay]` to client)** — orthogonal. Random pool config does not need to ride this channel; it lives in `fm2k_host.ini` and `host_config` (host-authoritative), so `[GamePlay]` sync remains free to be a pure local-config thing.
- **#55 (per-game overrides)** — relevant only for *prefilling* `RandomStageMax` when the user switches games. Implementation: when the active game changes, if `fm2k_host.ini` has no `RandomStageMax` for the new game (or we keep this global and read fresh defaults), repopulate from the per-game config. Simplest first cut: keep `[Host]` flat (one set of host config), and re-prefill `RandomStageMax` from the new game's `StageNb` when the user switches games AND `RandomEnable=false` AND the stored max equals the previous game's prefill. (Don't clobber a user-customized value.)
- **#56 (this feature)** — unblocked once the three above land in any form.

### 2.8 Files to touch (when we implement)

- `FM2K_Integration.h` — add `host_config_random_enable_`, `host_config_random_min_`, `host_config_random_max_` to the staged-state block alongside `host_config_stage_`.
- `FM2K_LauncherUI.cpp:1671-1709` (`RenderHostConfigWindow` / equivalent name) — add checkbox + min/max inputs; extend the `fopen("fm2k_host.ini","w")` writer; extend the loader.
- `FM2K_GameIni.cpp` (or wherever load/save of `fm2k_host.ini` lives if separate) — add the three new keys.
- `FM2KHook/src/netplay/netplay.cpp:169-185` — when constructing the outbound `host_config`, if `random_enable` is true do the roll here and pass the result as `stage`. **No protocol change needed** — `selected_stage` carries either the user's pick or the rolled value transparently.
- `FM2K_LauncherUI.cpp` host panel — show the random range as part of the post-connect summary (the same area that already shows "Stage: N").
- Spectator/replay paths: no change (they already trust `host_config.selected_stage`).

No `CtrlPacket` schema bump, no version negotiation, no migration. The only on-wire delta is "the stage number is now sometimes a fresh random draw instead of a stored constant," which is invisible to the receiver.

---

## 3. What to copy from LilithPort, and what to skip

### Copy
- **Host-authoritative config.** Host wins, client mirrors. Exactly our model already.
- **Spectators receive the same resolved values via the same channel.** Already our model.
- **Single integer "max stage" with no autodetect, plus a clear UI warning.** Trying to enumerate real stage counts means parsing the game's binary tables — not worth it for v1.
- **One source of truth at match start.** Roll once, distribute, done.

### Do NOT copy
- **The `StageSelect=0` sentinel.** Overloading "0" to mean "random" is unfindable in the UI without the dynamic relabeling trick. Use an explicit `RandomEnable` boolean.
- **The `StageSelect > MaxStage` cycle mode squeezed into the same two fields.** If we ever want a cycle/rotation feature, it gets its own checkbox + clear copy. Don't make field semantics depend on which value is bigger.
- **Seed-synchronized PRNG that everyone runs locally.** It's elegant on paper but a single missed breakpoint or rollback divergence desyncs the stage stream silently. Sending the resolved index over our reliable control channel is simpler and self-correcting.
- **Trusting the user's `MaxStage` to match the game without any sanity check.** Lilith's README literally says *"the game will crash"* if it's wrong. We can do better in v2 by recording the highest stage index ever observed for a given game hash and warning when the user sets `Max` above it — but that's a follow-up, not a blocker.
- **Per-profile duplication of every setting.** Lilith puts `MaxStage` in every game profile. We have a single `[Host]` section; keep it that way and prefill from the per-game `[GamePlay]` data instead of fanning out into N copies of the same field.
