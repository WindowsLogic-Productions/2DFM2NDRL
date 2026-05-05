#!/usr/bin/env python3
# Diff two FM2K rng-trace files and print the first divergent call.
#
# Usage:
#   python3 tools/rng_trace_diff.py FM2K_rng_trace_pid<HOST>.bin FM2K_rng_trace_pid<SPEC>.bin
#
# Each record is 16 bytes little-endian:
#   uint32 call_index
#   uint32 caller_pc       (return address, lives in FM2K.exe text section)
#   uint32 rng_pre         (seed value before the call)
#   uint32 rng_post        (seed value after original_game_rand() returned)
#
# A bit-exact build of the host and spectator MUST produce identical streams.
# Find the first index where any of (caller_pc, rng_pre) differs — that's the
# call site where the two processes' executions first diverge. Looking up
# caller_pc in IDA tells you which game function called rand at that moment.

import struct
import sys

REC = struct.Struct('<IIII')
RECSZ = REC.size

def load(path):
    with open(path, 'rb') as f:
        return f.read()

def main(a_path, b_path):
    a = load(a_path)
    b = load(b_path)
    n = min(len(a), len(b)) // RECSZ
    print(f"{a_path}: {len(a)//RECSZ} records")
    print(f"{b_path}: {len(b)//RECSZ} records")
    print(f"comparing first {n} records ...\n")

    for i in range(n):
        ar = REC.unpack_from(a, i * RECSZ)
        br = REC.unpack_from(b, i * RECSZ)
        if ar != br:
            ctx_lo = max(0, i - 3)
            ctx_hi = min(n, i + 4)
            print(f"FIRST DIVERGENCE AT call_index={i}\n")
            print(f"  {'idx':>8}  {'caller_pc':>10}  {'rng_pre':>10}  {'rng_post':>10}")
            for j in range(ctx_lo, ctx_hi):
                aj = REC.unpack_from(a, j * RECSZ)
                bj = REC.unpack_from(b, j * RECSZ)
                marker_a = '  '
                marker_b = '  '
                if aj != bj:
                    marker_a = '> '
                    marker_b = '> '
                print(f"  A {marker_a}{aj[0]:>6}  0x{aj[1]:08X}  0x{aj[2]:08X}  0x{aj[3]:08X}")
                print(f"  B {marker_b}{bj[0]:>6}  0x{bj[1]:08X}  0x{bj[2]:08X}  0x{bj[3]:08X}")
                print()
            print(f"\nHost  caller_pc=0x{ar[1]:08X}  pre=0x{ar[2]:08X}  post=0x{ar[3]:08X}")
            print(f"Spec  caller_pc=0x{br[1]:08X}  pre=0x{br[2]:08X}  post=0x{br[3]:08X}")
            if ar[1] != br[1]:
                print("\n=> Different CALL SITES. One side ran an extra rng call OR a")
                print("   different function called rand at this position. Look up both")
                print("   PCs in IDA — the unique one is the leak.")
            elif ar[2] != br[2]:
                print("\n=> Same call site but different rng_pre. Means earlier divergence")
                print("   was MISSED — re-run with FM2K_RNG_TRACE_MAX larger or check that")
                print("   battle started at the same point on both sides.")
            return 1

    print("OK — first {} records identical".format(n))
    if len(a) != len(b):
        print(f"NOTE: trace lengths differ "
              f"({len(a)//RECSZ} vs {len(b)//RECSZ} records); "
              f"the longer side has extra trailing calls.")
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
