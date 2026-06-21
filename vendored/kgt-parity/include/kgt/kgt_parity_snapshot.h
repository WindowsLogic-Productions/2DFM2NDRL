/* SPDX-License-Identifier: Apache-2.0 */
/* Canonical per-frame parity snapshot.
 *
 * This is the on-disk format used by:
 *   - FM2KHook recorder (writes one snapshot per frame from
 *     WonderfulWorld_ver_0946.exe live globals)
 *   - kgtengine recorder (writes one snapshot per frame from KgtEngineState)
 *   - tools/kgt_diff_pty (reads two .pty files and reports first divergence)
 *
 * Design constraints:
 *   - Pure POD, no pointers, no platform-dependent types.
 *   - Little-endian everywhere (x86 / x86-64 only).
 *   - Process-independent: no heap addresses, no `time(NULL)`, no thread IDs.
 *   - Forward-compatible: file header carries `version`; the diff tool reads
 *     v1 + v2 + v3 files transparently — newer fields are appended at struct
 *     tails so older readers can stop at `header.snapshot_size`.
 *
 * File layout:
 *   [KgtParitySnapshotHeader]   (fixed 32 bytes, magic "KGTP" + meta)
 *   [KgtParitySnapshot] × N     (one per frame, contiguous;
 *                                size = header.snapshot_size, NOT
 *                                sizeof(KgtParitySnapshot) — recorder
 *                                writes the version it knows, reader
 *                                handles all)
 *
 * Versioning history:
 *   v1 (136 B/frame): frame, rng, input_p1, input_p2, match_phase,
 *                     round_timer, camera_x/y, players[2] (12×i32 each),
 *                     pad[2].
 *   v2 (196 B/frame): v1 + system_vars[16] (i16) + rng_after_frame (u32) +
 *                     per-player {hit_box_active_count,
 *                     hurt_box_active_count, cancel_script_item}.
 *   v3 (260 B/frame): v2 + per-player task_vars_i16[16] (32 B per player,
 *                     +64 B total). The 2DFM script interpreter exposes
 *                     a task-variable bank per object (FM2K [V] bank 0x00,
 *                     16 × i16); these drive cancel logic, super-meter
 *                     gating in custom scripts, and combo state. Without
 *                     them in the snapshot, divergences in the script
 *                     VM's inner state are invisible to the diff tool. */

#ifndef KGT_PARITY_SNAPSHOT_H
#define KGT_PARITY_SNAPSHOT_H

#include <stdint.h>

#define KGT_PARITY_MAGIC "KGTP"
#define KGT_PARITY_SNAPSHOT_VERSION 3u

#define KGT_PARITY_PLAYERS 2
#define KGT_PARITY_TASK_VARS 16

/* Canonical per-player projection. All fields are pulled from the same
 * source-of-truth in both engines:
 *
 *   FM2KHook side                  kgtengine side
 *   -----------------------------  -----------------------------------
 *   g_character_data_base + slot   engine->state.pool.slots[main_idx]
 *   character_state_machine PC     obj->vm.script_idx / item_idx
 *   Q14.18 pos/vel @ slot+0x18..   obj->pos_x / pos_y / vel_x / vel_y
 *   HP @ slot+0xBA01               obj->hp
 *   Super gauge @ slot+0xDF05      obj->super_meter
 *   facing @ slot+0x4              obj->facing
 *   task vars @ slot+0x131         obj->task_vars_i16[16]
 *
 * Q14.18 fixed-point fields are stored RAW (not converted to int pixels)
 * so any sub-pixel divergence is visible to the diff tool.
 *
 * Extension fields appear in version order at the tail of each struct so
 * older readers can stop at `header.snapshot_size`. */
typedef struct KgtParityPlayer {
    /* ----- v1 fields (frozen layout, 48 B) ------------------------- */
    int32_t  script_idx;     /* current VM script id */
    int32_t  item_idx;       /* current VM item index (program counter) */
    int32_t  pos_x;          /* Q14.18 fixed */
    int32_t  pos_y;          /* Q14.18 fixed */
    int32_t  vel_x;          /* Q14.18 fixed */
    int32_t  vel_y;          /* Q14.18 fixed */
    int32_t  hp;             /* current HP */
    int32_t  super_meter;    /* current super gauge */
    int32_t  facing;         /* +1 right, -1 left */
    int32_t  state_flags;    /* guard / hit-confirmed / etc. bitfield */
    int32_t  hitstun;        /* frames left in hitstun */
    int32_t  hitstop;        /* frames left in hitstop */
    /* ----- v2 fields (12 B, total 60) ------------------------------ */
    int32_t  hit_box_active_count;   /* popcount of hit_box_slot[] != -1 */
    int32_t  hurt_box_active_count;  /* popcount of hurt_box_slot[] != -1 */
    int32_t  cancel_script_item;     /* most-recent [C] item index, -1 = none */
    /* ----- v3 fields (32 B, total 92) ------------------------------ */
    int16_t  task_vars[KGT_PARITY_TASK_VARS];   /* per-object [V] bank 0x00 */
} KgtParityPlayer;

/* Per-frame snapshot. */
typedef struct KgtParitySnapshot {
    /* ----- v1 fields (frozen layout) ------------------------------- */
    uint32_t        frame;            /* monotonic from match start */
    uint32_t        rng;              /* MSVC LCG state at frame start */
    uint32_t        input_p1;         /* 11-bit raw input bitmask */
    uint32_t        input_p2;
    int32_t         match_phase;      /* kgt_match_phase_t */
    int32_t         round_timer;
    int32_t         camera_x;
    int32_t         camera_y;
    /* ----- v1 player block ----------------------------------------- */
    /* In v1 each KgtParityPlayer was 48 B; v2 grew it to 60 B; v3 to
     * 92 B. The snapshot's `players[]` shifts the post-player tail by
     * 88 bytes total over v1.  Older-version readers must use
     * `header.snapshot_size` (NOT sizeof(KgtParitySnapshot)) for stride
     * and field offsets. */
    KgtParityPlayer players[KGT_PARITY_PLAYERS];
    /* ----- v2 fields ----------------------------------------------- */
    uint32_t        rng_after_frame;       /* RNG state at frame end (post-advance) */
    int16_t         system_vars[16];       /* match.system_vars[16] (FM2K [V] bank 0x80) */
    /* Pad to 8-byte alignment for clean fwrite chunks. */
    uint32_t        pad[2];
} KgtParitySnapshot;

/* File header. 32 bytes fixed, written once at file open. */
typedef struct KgtParitySnapshotHeader {
    char     magic[4];               /* "KGTP" */
    uint32_t version;                /* KGT_PARITY_SNAPSHOT_VERSION */
    uint32_t snapshot_size;          /* sizeof(KgtParitySnapshot) for the writer's
                                        version. Reader uses THIS, not
                                        sizeof(KgtParitySnapshot), to stride. */
    uint32_t flags;                  /* bit 0 = recorded by FM2KHook,
                                        bit 1 = recorded by kgtengine */
    uint32_t frame_count;            /* total frames present (set by recorder on
                                        close; readers fall back to
                                        (file_size - sizeof(header)) /
                                        snapshot_size if zero). */
    /* RNG seed at recorder-open time (FM2K's g_rand_seed @ 0x41FB1C, or
     * kgt's KgtMatchConfig.seed). Lets the diff harness boot kgt with the
     * same initial RNG so frame-0 rng matches by construction; per-frame
     * divergence after that reflects actual consumption-pattern drift, not
     * mismatched seeds. Zero in old captures (header was reserved). */
    uint32_t initial_seed;
    uint32_t reserved[2];
} KgtParitySnapshotHeader;

#define KGT_PARITY_FLAG_FROM_FM2K  0x1u
#define KGT_PARITY_FLAG_FROM_KGT   0x2u

/* Sizes of the v1 prefix — used by readers comparing against v1 files. */
#define KGT_PARITY_V1_PLAYER_BYTES   48u
#define KGT_PARITY_V1_SNAPSHOT_BYTES 136u
/* v2 sizes — exposed for back-compat readers. */
#define KGT_PARITY_V2_PLAYER_BYTES   60u
#define KGT_PARITY_V2_SNAPSHOT_BYTES 196u

/* Layout lock-in for v3. The sizes below are part of the on-disk format
 * spec; any drift bumps the version and breaks older readers. */
#if defined(__cplusplus)
static_assert(sizeof(KgtParityPlayer)         == 92,  "KgtParityPlayer v3 layout drifted");
static_assert(sizeof(KgtParitySnapshot)       == 260, "KgtParitySnapshot v3 layout drifted");
static_assert(sizeof(KgtParitySnapshotHeader) == 32,  "KgtParitySnapshotHeader layout drifted");
#else
_Static_assert(sizeof(KgtParityPlayer)         == 92,  "KgtParityPlayer v3 layout drifted");
_Static_assert(sizeof(KgtParitySnapshot)       == 260, "KgtParitySnapshot v3 layout drifted");
_Static_assert(sizeof(KgtParitySnapshotHeader) == 32,  "KgtParitySnapshotHeader layout drifted");
#endif

#endif /* KGT_PARITY_SNAPSHOT_H */
