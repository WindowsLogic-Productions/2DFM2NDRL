# RNG Seed Cross-References in FM2K

## Overview
This document lists all cross-references to the RNG seed (`g_random_seed`) and the RNG function (`game_rand`) in FM2K. This is critical for rollback netcode implementation as RNG state must be deterministic and savable/restorable.

## RNG Function

### `game_rand` (0x417a22)
**Type**: Function  
**Size**: 0x1e (30 bytes)

**Implementation**:
```c
int game_rand()
{
  g_random_seed = 214013 * g_random_seed + 2531011;
  return (g_random_seed >> 16) & 0x7FFF;
}
```

**Algorithm**: Linear Congruential Generator (LCG)
- Multiplier: 214013 (0x344FD)
- Increment: 2531011 (0x269EC3)
- Returns: Upper 15 bits of seed (0x7FFF = 32767 max)

## RNG Seed Global Variable

### `g_random_seed` (0x41fb1c)
**Type**: DWORD (4 bytes)  
**Current Value**: 0x1 (initialized to 1)

## Cross-References

### 1. `game_rand` Function (0x417a22)
**References to `g_random_seed`**:
- **Line 2**: Write - `g_random_seed = 214013 * g_random_seed + 2531011;`
- **Line 2**: Read - Uses `g_random_seed` in calculation
- **Line 3**: Read - `return (g_random_seed >> 16) & 0x7FFF;`

### 2. `character_state_machine` Function (0x411bf0)
**Calls to `game_rand()`**:

#### Call Site 1: Line 273
**Address**: 0x411ed2  
**Context**: Character initialization - random HP variation
```c
if ( *(max_hp_value + 12) )
{
  *(v0 + 64) += 100 * (game_rand() % *(max_hp_value + 12));
  v0 = g_object_data_ptr;
}
```
**Purpose**: Adds random HP variation (0 to max_hp_value[12] * 100) to character's initial HP

#### Call Site 2: Line 1672
**Address**: 0x4139c0  
**Context**: Script command 0x20 - Random branch
```c
if ( game_rand() % (*(reference_count + 1) + 1) <= *(reference_count + 3) || !*(reference_count + 6) )
  goto LABEL_332;
```
**Purpose**: Random branch logic - if random value modulo (param1 + 1) <= param3, skip to param6 state

### 3. Other Functions (To Be Investigated)
The following functions may also use RNG but need further investigation:
- `ai_behavior_processor` (0x410060) - No direct calls found in decompiled code
- `game_state_manager` (0x406fc0) - No direct calls found in decompiled code
- Other game logic functions may use RNG for:
  - AI decision making
  - Random stage effects
  - Damage variation
  - Hit stun variation
  - Other gameplay randomization

## Rollback Netcode Implications

### Critical Requirements
1. **State Saving**: `g_random_seed` must be saved in every savestate
2. **State Loading**: `g_random_seed` must be restored exactly when loading a savestate
3. **Determinism**: All `game_rand()` calls must execute in the same order on both clients
4. **Synchronization**: RNG calls must be synchronized between network peers

### Memory Location
- **Address**: 0x41fb1c
- **Size**: 4 bytes (DWORD)
- **Type**: Unsigned 32-bit integer

### Hook Points
For rollback implementation, consider hooking:
1. `game_rand()` function entry - to track RNG usage
2. `g_random_seed` read/write - to ensure state consistency
3. All call sites to `game_rand()` - to verify deterministic execution

## Notes
- The LCG algorithm used is deterministic and suitable for rollback
- The seed is initialized to 1, but may be set elsewhere during game initialization
- Need to verify if seed is ever initialized from external sources (time, etc.)
- Need to check if there are any other RNG functions or seed variables
