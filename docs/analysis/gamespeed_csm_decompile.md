# GameSpeed Bug Investigation — `character_state_machine` (WonderfulWorld 0946)

Reference for why `Editor.TestPlay.GameSpeed` values other than `10` break scripts (Vanpri stuck at 5, BG fx broken at 9, VS-intro/assists broken at 8).

**Status:** Fixed in FM2KHook via `FixGameSpeedDesync()` in `dllmain.cpp` — pins `g_gamespeed_pic_step` to 100, leaves MOVE/ACCEL scaling intact. Runtime toggle: launcher dev mode → "FM2K diagnostics" → "GameSpeed PIC desync fix (recommended)".

## TL;DR

`init_round_state_and_apply_gamespeed @ 0x406450` derives a single integer `gs_scalar` from `g_ini_GameSpeed` and writes three globals from it:

```c
int init_round_state_and_apply_gamespeed()
{
    ObjectSlot *current_obj_data = g_object_data_ptr;
    ObjectSlot *obj_iter         = g_object_pool;
    int         obj_count_remaining = 1024;
    do {
        if (obj_iter->type > 1 && obj_iter != current_obj_data)
            obj_iter->type = 1;
        ++obj_iter;
        --obj_count_remaining;
    } while (obj_count_remaining);

    int gs_scalar;
    if (g_ini_GameSpeed <= 10) gs_scalar = 5  * g_ini_GameSpeed + 50;  // gs=10 → 100, gs=5 → 75
    else                       gs_scalar = 10 * g_ini_GameSpeed;        // gs=20 → 200

    g_gamespeed_pic_step     = gs_scalar;                  // 0x445704 — used by OP_PIC
    g_gamespeed_move_scalar  = 0x10000 / gs_scalar;        // 0x541F78 — used by OP_MOVE pos
    g_gamespeed_accel_scalar = 3932160 / (gs_scalar * gs_scalar); // 0x445700 — OP_MOVE accel
    return g_gamespeed_accel_scalar;
}
```

| GameSpeed | gs_scalar | move_scalar | accel_scalar |
|-----------|-----------|-------------|--------------|
| 10 (def)  | 100       | 655         | 393          |
| 9         | 95        | 689         | 435          |
| 8         | 90        | 728         | 485          |
| 7         | 85        | 771         | 544          |
| 6         | 80        | 819         | 614          |
| 5         | 75        | 873         | 698          |

## Where the script interpreter consumes the three scalars

Three call sites in `character_state_machine @ 0x411bf0`:

### 1. `OP_PIC` (case 0x0C) — wait timer (root cause of every breakage)

```c
case OP_PIC:
    if (obj->entity_kind == CSMK_PLAYER)
        char_data->unknown_dfbb[84] = 1;
    pic_keep_time     = *(script_item_ptr + 1);    // u16 keepTime arg from script
    pic_wait_timer_new = -1;
    obj->ground_y      = -1;
    if (pic_keep_time)
        pic_wait_timer_new = g_gamespeed_pic_step * pic_keep_time
                           + obj->wait_timer;       // ADD: scaled by gs_scalar
    obj->wait_timer = pic_wait_timer_new;
    ++obj->script_item_idx;
    goto csm_dispatch_iter_continue;
```

The wait timer is **incremented by `gs_scalar * keepTime`** (75 per script-unit at gs=5; 100 at gs=10).

### 2. Dispatch-loop entry — the hardcoded counterpart

```c
pic_wait_timer_now = obj->wait_timer;
if (pic_wait_timer_now >= 0) {
    pic_wait_timer_after_tick = pic_wait_timer_now - 100;   // ← HARDCODED 100, not scaled
    obj->wait_timer = pic_wait_timer_after_tick;
    if (pic_wait_timer_after_tick < 0) {
        // ...advance to next opcode (likely another OP_PIC)...
    }
}
```

**This is the bug.** ADD scales with gs, SUBTRACT does not. They cancel exactly only at gs=10 (`+100` add per wait-unit vs `-100` per frame). At any other gs the wait window is wrong:

| keepTime | gs=10 frames | gs=5 frames | speedup |
|----------|--------------|-------------|---------|
| 1        | 2            | 1           | **2.0×** (collapses!) |
| 2        | 3            | 2           | 1.5× |
| 3        | 4            | 3           | 1.33× |
| 10       | 11           | 8           | 1.375× |

Small waits collapse disproportionately.

### 3. `OP_MOVE` (case 0x01) — position & acceleration scalars

```c
case OP_MOVE:
    move_additive_flag = 0;
    move_facing_sign   = (obj->flags_misc) ? -1 : 1;
    move_flags_byte    = *(script_item_ptr + 9);
    if (move_flags_byte & 1) move_additive_flag = 1;

    move_x_result = move_facing_sign * g_gamespeed_move_scalar  * *(script_item_ptr + 3);
    if ((move_flags_byte & 2) == 0)
        obj->move_x = move_additive_flag ? obj->move_x + move_x_result : move_x_result;

    move_y_result = g_gamespeed_move_scalar * *(script_item_ptr + 5);
    if ((move_flags_byte & 4) == 0)
        obj->move_y = move_additive_flag ? obj->move_y + move_y_result : move_y_result;

    accel_x_result = move_facing_sign * g_gamespeed_accel_scalar * *(script_item_ptr + 1);
    if ((move_flags_byte & 8) == 0)
        obj->accel_x = move_additive_flag ? obj->accel_x + accel_x_result : accel_x_result;

    accel_y_result = g_gamespeed_accel_scalar * *(script_item_ptr + 7);
    if (move_flags_byte & 0x10) goto csm_op_default_noop;
    obj->accel_y = move_additive_flag ? obj->accel_y + accel_y_result : accel_y_result;
    ++obj->script_item_idx;
    goto csm_dispatch_iter_continue;
```

MOVE values scale by `move_scalar` (1.33× larger per script-unit at gs=5 vs gs=10).
ACCEL values scale by `accel_scalar` (1.78× larger).

These scalings are **internally consistent** with the per-frame integration (`pos += vel; vel += accel` runs once per frame regardless of gs, so larger per-frame deltas = faster movement). They don't desync anything on their own.

## What does NOT scale (the desync source)

Everything that ticks at a constant 1/frame regardless of `g_ini_GameSpeed`:

- `obj->hitstop -= 1` per frame
- The hardcoded `-100` per frame at the dispatch-loop top (above)
- `g_game_timer`
- Hurtbox / hitbox lifetimes (frame-counter driven)
- BG_EFFECT and shake-effect timers (`g_effect_timer_*`, `g_shake_effect_*`)
- `OP_PLAYER_STOP` / hitstop-extension durations (raw script values, no scaling)
- `OP_RANDOM` thresholds
- Stage script (`ProcessStageScript` ticks once per frame)

When `OP_PIC` waits compress but these don't, scripts run their state transitions ahead of the systems they're synchronizing with.

## Why each gs value breaks specific things

- **gs=9** ("BG/effects fucked") — 5% drift. Background animations using parallel scripts go out of phase against unscaled BG_EFFECT/shake timers.
- **gs=8** ("VS intros stick, assists broken") — VS intros are long `OP_PIC` chains; assists (CSMK_PLAYER_PARTNER) lifetimes are hitstop-based. Both desync.
- **gs=5** ("characters stuck") — biggest delta. `OP_PIC 1` waits collapse to 1 frame → script races past states needing animation/parent-pointer to catch up. The dispatch loop's 300-iteration safety break trips and leaves the object in an intermediate state. (Bottom of `character_state_machine` prints `ScriptMainLoopError %d %d - nd:%d step:%d`.)

## The fix

Pin `g_gamespeed_pic_step = 100` always, regardless of `g_ini_GameSpeed`. Don't touch `g_gamespeed_move_scalar` / `g_gamespeed_accel_scalar`.

Implementation in `FM2KHook/src/core/dllmain.cpp::FixGameSpeedDesync()`:
1. NOP the 6-byte `mov ds:[g_gamespeed_pic_step], ecx` at `0x40649A` (the only writer).
2. Initialize `g_gamespeed_pic_step = 100` once at hook init (replaces the now-NOPed runtime write).

Effect:
- `OP_PIC N` always waits exactly N+1 frames, regardless of gs.
- MOVE/ACCEL still scale → game still feels faster at gs<10 (chars move further per frame).
- Nothing desyncs against hitstop / hurtbox / BG_FX / `g_game_timer`.

Opt-out env var: `FM2K_KEEP_GAMESPEED_PIC=1` re-enables vanilla (broken) behavior. UI toggle persists in `dev_flags.ini` key `gs_pic_fix` (default true).

## How to reproduce the original bug

1. Launcher → dev mode → "FM2K diagnostics" → uncheck "GameSpeed PIC desync fix".
2. Restart the game (env var is read at hook init).
3. Set `Editor.TestPlay.GameSpeed=5` in the relevant `.ini`.
4. Pick Vanpri. Character will get stuck mid-script as before.

## IDA renames applied (this work)

| Address    | Old                         | New                                    |
|------------|-----------------------------|----------------------------------------|
| 0x430104   | `uValue`                    | `g_ini_GameSpeed`                      |
| 0x430100   | `uCheck`                    | `g_ini_HitJudge`                       |
| 0x541F78   | `g_char_data_array_end`     | `g_gamespeed_move_scalar`              |
| 0x445700   | `g_char_velocity_multiplier`| `g_gamespeed_accel_scalar`             |
| 0x445704   | `g_delay_frame_multiplier`  | `g_gamespeed_pic_step`                 |
| 0x406450   | `ResetObjectsAndCalculateSpeed` | `init_round_state_and_apply_gamespeed` |

Locals in `init_round_state_and_apply_gamespeed`:
`v0→current_obj_data, v1→obj_iter, v2→obj_count_remaining, v3→gs_scalar`.

Locals in `character_state_machine` (gamespeed-relevant only):
`delay_frames→move_additive_flag, delay_result→move_facing_sign, velocity_absolute_flag→move_flags_byte, velocity_direction→move_x_result, command_flags→move_y_result, velocity_x_calc→accel_x_result, velocity_y_calc→accel_y_result, previous_attack_state→pic_keep_time, sprite_data_base_ptr→pic_wait_timer_new, timer_value→pic_wait_timer_now, timer_remaining→pic_wait_timer_after_tick, delay_timer→csm_kind_dispatch`.

> Note: `obj->unknown_34[8]` (i.e. offset 0x3C in the object slot) is the `wait_timer` field — used as `obj->wait_timer` in the snippets above for readability, but the underlying struct field is still anonymous in the IDB. Future cleanup: declare a struct field at offset 0x3C in the object-slot type.

## Re-pulling fresh decomp

Names above are live in the IDB as of the rename batch on 2026-05-10. Re-pull verbatim with:

```
mcp__ida-pro-mcp__decompile addr=init_round_state_and_apply_gamespeed
mcp__ida-pro-mcp__decompile addr=character_state_machine            # huge, ~99k chars
```
