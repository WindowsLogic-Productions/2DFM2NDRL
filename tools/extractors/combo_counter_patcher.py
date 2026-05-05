#!/usr/bin/env python3
"""
2DFM/FM2K Combo Counter Patcher
===============================
This patches the combo counter to display at hit 1 instead of hit 2.

The fix changes a single byte:
- Original: jle (0x7E) - skip combo display if hits <= 1
- Patched:  jl  (0x7C) - skip combo display if hits < 1 (i.e., show at hit 1+)

Location: hit_detection_system function at 0x40f821 (virtual address)
"""

import sys
import shutil
from pathlib import Path

# Pattern to find the combo counter check
# mov eax, [esi+0xdf01]  ; load combo counter
# lea ebx, [esi+0xdf01]  ; load combo counter address
# cmp eax, 1             ; compare with 1
# jle +0x36              ; skip if <= 1 (THIS IS WHAT WE PATCH)
SEARCH_PATTERN = bytes([
    0x8B, 0x86, 0x01, 0xDF, 0x00, 0x00,  # mov eax, [esi+0xdf01]
    0x8D, 0x9E, 0x01, 0xDF, 0x00, 0x00,  # lea ebx, [esi+0xdf01]
    0x83, 0xF8, 0x01,                     # cmp eax, 1
    0x7E, 0x36,                           # jle +0x36 (byte 15 is 0x7E)
])

# Offset within pattern where the jle byte is
JLE_OFFSET = 15

# Original and patched bytes
ORIGINAL_BYTE = 0x7E  # jle
PATCHED_BYTE = 0x7C   # jl


def find_pattern(data: bytes, pattern: bytes) -> int:
    """Find pattern in data, return offset or -1 if not found."""
    return data.find(pattern)


def patch_exe(exe_path: str, create_backup: bool = True) -> bool:
    """Patch the executable to show combo counter at hit 1."""

    path = Path(exe_path)
    if not path.exists():
        print(f"[ERROR] File not found: {exe_path}")
        return False

    # Read the executable
    print(f"[*] Reading: {exe_path}")
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    print(f"[*] File size: {len(data)} bytes (0x{len(data):X})")

    # Find the pattern
    offset = find_pattern(bytes(data), SEARCH_PATTERN)

    if offset == -1:
        print("[ERROR] Pattern not found! This exe may already be patched or is a different version.")

        # Try to find if already patched
        patched_pattern = bytearray(SEARCH_PATTERN)
        patched_pattern[JLE_OFFSET] = PATCHED_BYTE

        patched_offset = find_pattern(bytes(data), bytes(patched_pattern))
        if patched_offset != -1:
            print(f"[!] Found PATCHED pattern at offset 0x{patched_offset:X}")
            print("[!] This executable has already been patched!")
        return False

    print(f"[+] Found pattern at file offset: 0x{offset:X}")

    # Calculate the exact byte location
    patch_offset = offset + JLE_OFFSET
    current_byte = data[patch_offset]

    print(f"[*] Patch location: 0x{patch_offset:X}")
    print(f"[*] Current byte: 0x{current_byte:02X} ({'jle' if current_byte == 0x7E else 'jl' if current_byte == 0x7C else 'unknown'})")

    if current_byte == PATCHED_BYTE:
        print("[!] Already patched!")
        return True

    if current_byte != ORIGINAL_BYTE:
        print(f"[ERROR] Unexpected byte 0x{current_byte:02X} at patch location!")
        return False

    # Create backup
    if create_backup:
        backup_path = path.with_suffix('.exe.backup')
        if not backup_path.exists():
            print(f"[*] Creating backup: {backup_path}")
            shutil.copy2(path, backup_path)
        else:
            print(f"[*] Backup already exists: {backup_path}")

    # Apply patch
    print(f"[*] Patching: 0x{ORIGINAL_BYTE:02X} (jle) -> 0x{PATCHED_BYTE:02X} (jl)")
    data[patch_offset] = PATCHED_BYTE

    # Write patched file
    with open(path, 'wb') as f:
        f.write(data)

    print("[+] Patch applied successfully!")
    print("[+] Combo counter will now show at hit 1 instead of hit 2!")

    return True


def unpatch_exe(exe_path: str) -> bool:
    """Revert the patch (change jl back to jle)."""

    path = Path(exe_path)
    if not path.exists():
        print(f"[ERROR] File not found: {exe_path}")
        return False

    print(f"[*] Reading: {exe_path}")
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    # Find the patched pattern
    patched_pattern = bytearray(SEARCH_PATTERN)
    patched_pattern[JLE_OFFSET] = PATCHED_BYTE

    offset = find_pattern(bytes(data), bytes(patched_pattern))

    if offset == -1:
        print("[ERROR] Patched pattern not found! File may not be patched.")
        return False

    patch_offset = offset + JLE_OFFSET

    print(f"[*] Found patched location at: 0x{patch_offset:X}")
    print(f"[*] Reverting: 0x{PATCHED_BYTE:02X} (jl) -> 0x{ORIGINAL_BYTE:02X} (jle)")

    data[patch_offset] = ORIGINAL_BYTE

    with open(path, 'wb') as f:
        f.write(data)

    print("[+] Patch reverted successfully!")
    return True


def main():
    if len(sys.argv) < 2:
        print("2DFM/FM2K Combo Counter Patcher")
        print("================================")
        print("Shows combo counter at hit 1 instead of hit 2")
        print()
        print("Usage:")
        print(f"  {sys.argv[0]} <path_to_exe>           - Apply patch")
        print(f"  {sys.argv[0]} <path_to_exe> --unpatch - Revert patch")
        print()
        print("Example:")
        print(f"  {sys.argv[0]} WonderfulWorld_ver_0946.exe")
        return 1

    exe_path = sys.argv[1]

    if len(sys.argv) > 2 and sys.argv[2] == '--unpatch':
        success = unpatch_exe(exe_path)
    else:
        success = patch_exe(exe_path)

    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
