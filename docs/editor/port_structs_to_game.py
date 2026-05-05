# port_structs_to_game.py
#
# One-shot script to port the KGT2nd_EDITOR.exe struct library into KGT2nd_GAME.exe
# (or WonderfulWorld_ver_0946.exe, or any FM2K-engine runtime).
#
# Usage: in the target IDA instance, open File → Script File → select this file.
# Or paste the contents into the Output-window Python prompt.
#
# Produces: 51 named types (byte/intN/uintN + 44 Kgt* structs) that match the
# layouts reverse-engineered in KGT2nd_EDITOR.exe session 2026-04-23.
# Verifies each struct's size matches the expected value before returning.
#
# After running this, you can:
#   - Retype any function parameter to e.g. KgtProjectSlot* to get field-resolved decompile.
#   - Retype globals: the game's current-object pointer should be KgtRuntimeObject*;
#     the game's object pool is `KgtRuntimeObject[1024]` (find by searching for a
#     stride-382 access pattern).
#   - Use the hunt_* helpers at the bottom to find the game's equivalents of
#     ExecuteAnimationScript, ReadCommonResourcePart, etc.
#
# Source of truth: /mnt/c/dev/wanwan/docs/editor/ida_progress.md
# Subsystem docs:  /mnt/c/dev/wanwan/docs/editor/*.md

import ida_typeinf, idaapi

# ----------------------------------------------------------------------------
# All C declarations in dependency order. #pragma pack(1) everywhere — fields
# are byte-packed (verified against the binary on 2026-04-23).
# ----------------------------------------------------------------------------

DECLS = [

# --- Primitive typedefs ---
"typedef unsigned char byte;",
"typedef signed char int8;",
"typedef unsigned char uint8;",
"typedef short int16;",
"typedef unsigned short uint16;",
"typedef int int32;",
"typedef unsigned int uint32;",

# --- File-format structs (from 2dfm/*.hpp, validated in editor binary) ---

"""#pragma pack(push, 1)
struct KgtScript {            // 39 bytes
  char   scriptName[32];
  uint16 scriptIndex;         // index into scriptItems[] where this script's cells start
  byte   gap;
  int32  flags;               // ScriptSpecialFlag (NORMAL=0, BACKGROUND=1, SYSTEM=3, ...)
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtScriptItem {        // 16 bytes — discriminated union by [0] type byte
  byte type;
  byte bytes[15];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPictureHeader {     // 20 bytes — on-disk sprite header
  int32 unknownFlag1;
  int32 width;
  int32 height;
  int32 hasPrivatePalette;
  int32 size;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
union KgtColorBgra {          // 4 bytes
  struct { byte blue; byte green; byte red; byte alpha; } channel;
  uint32 value;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPalette {           // 1024 bytes (256 × 4)
  KgtColorBgra colors[256];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtSoundItemHeader {   // 42 bytes — on-disk
  int32 unknown;
  char  name[32];
  int32 size;
  byte  soundType;            // low nibble = type (1=WAV, 2=MIDI, 3=CDDA), bit 4 = loop
  byte  track;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtNameInfo {          // 256 bytes
  char name[256];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtReactionItem {      // 36 bytes
  char  reactionName[32];
  int32 isHurtAction;         // bit 0 = Doing (takes damage), see hit_junction_system.md
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtThrowReaction {     // 32 bytes — same layout as 'common image' name entries
  char name[32];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtFileHeader {        // 272 bytes (16-byte signature + 256-byte project name)
  byte        fileSignature[16];
  KgtNameInfo name;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtRecoverTimeConfig { // 4 bytes
  byte gap;
  byte attackRecoverTime;     // editor default 30; wanwan.kgt = 4
  byte defenceRecoverTime;    // editor default 30; wanwan.kgt = 4
  byte clashRecoverTime;      // editor default 50; wanwan.kgt = 20
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtGameDemoConfig {    // 8 bytes
  byte titleDemoId;
  byte storyModeCharSelectDemoId;
  byte oneVsOneModeCharSelectDemoId;
  byte teamModeCharSelectDemoId;
  byte continueDemoId;
  byte openingDemoId;
  byte unknownTag1;
  byte unknownTag2;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtProjectBaseConfig { // 4 bytes — bitfield
  // bit 0 encryptGame | 1 allowClash | 2 enableStoryMode | 3 enable1V1Mode |
  // bit 4 enableTeamMode | 5 showHpAfterHpBar | 6 pressToStart
  int32 rawValue;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtCharSelectConfig {  // 28 bytes (14 int16)
  int16 selectBoxStartX, selectBoxStartY;
  int16 iconWidth, iconHeight;
  int16 columnNum, rowNum;
  int16 player1PortraitX, player1PortraitY;
  int16 player1PortraitTeamOffsetX, player1PortraitTeamOffsetY;
  int16 player2PortraitX, player2PortraitY;
  int16 player2PortraitTeamOffsetX, player2PortraitTeamOffsetY;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtDemoConfig {        // 9 bytes — .demo file-specific block
  int16 bgmSoundId;
  byte  pressToSkip;
  int16 unknownGap;
  int32 totalTime;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtStageConfig {       // 4 bytes — .stage file-specific block
  int32 bgmSoundId;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtHurtBindInfo {      // 12 bytes — EDITOR's declared struct (but see note below)
  int32 hurtId;
  int32 scriptId;
  int32 effectObjectId;
};
#pragma pack(pop)""",

# NOTE from player_file_format.md: the on-disk "82-byte hurt binds" entries the
# 2dfm doc refers to are actually `KgtCommandInput` special-move entries. The
# true compact hurt-bind entry is 6 bytes. See player_file_format.md.

"""#pragma pack(push, 1)
struct KgtThrowActionInfo {   // 16 bytes
  int32 throwActionId;
  int32 picNo;
  int32 offsetX;
  int32 offsetY;
};
#pragma pack(pop)""",

# --- Script-item variants (16-byte overlays on KgtScriptItem, selected by type byte) ---

"""#pragma pack(push, 1)
struct KgtShowPic { byte type; uint16 keepTime; uint16 idxAndFlip; int16 offsetX; int16 offsetY; byte fixDir; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtMoveCmd { byte type; int16 accelX; int16 moveX; int16 moveY; int16 accelY; byte flags; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPlaySoundCmd { byte type; byte unknown; uint16 soundIdx; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtColorSetCmd { byte type; byte colorBlendType; int8 red; int8 green; int8 blue; int8 alpha; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtJumpCmd { byte type; uint16 jumpId; uint8 jumpPos; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtLoopCmd { byte type; uint8 loopCount; uint16 targetScriptId; uint8 targetPos; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtRandomCmd { byte type; uint16 randomMaxVal; uint16 moreThanVal; byte unknownGap; uint16 targetScriptId; uint8 targetPos; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtObjectCmd {
  byte type; byte flags;
  uint16 targetScriptId; uint8 targetPos;
  uint16 targetScriptIdIfExists; uint8 targetPosIfExists;
  int16 posX; int16 posY;
  uint8 manageNo; int8 layer;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtVariableCmd {
  byte type; uint16 targetScriptId; uint8 targetPos;
  byte targetVariable; byte opFlags; byte compareVariable;
  int16 operationValue; int16 compareValue;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtAfterimageCmd {
  byte type; uint16 unknownGap;
  uint8 afterimageMaxCount; uint8 afterimageGap;
  byte colorBlendType; byte afterimageColorType;
  int8 red; int8 green; int8 blue; int8 alpha;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtStageStart { byte type; byte flags; int16 horiScroll; int16 vertScroll; };
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPos { byte type; int16 x; int16 y; };
#pragma pack(pop)""",

# --- In-memory slot variants (first field of KgtPictureHeader/KgtSoundItemHeader
# is overwritten with a GlobalAlloc pointer after load) ---

"""#pragma pack(push, 1)
struct KgtPictureSlotEntry {  // 20 bytes
  void  *data;                // GlobalAlloc'd pixel buffer
  int32 width, height;
  int32 hasPrivatePalette;
  int32 size;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtSoundSlotEntry {    // 42 bytes
  void  *data;
  char   name[32];
  int32  size;
  byte   soundType;
  byte   track;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPaletteWithPad {    // 1056 bytes
  KgtPalette palette;
  byte       padding[32];
};
#pragma pack(pop)""",

# --- Top-level project/file-content structs ---

"""#pragma pack(push, 1)
struct KgtProjectSlot {       // 1,271,828 bytes (0x136814)
  KgtFileHeader       header;
  KgtScript           scripts[1024];
  KgtScriptItem       scriptItems[65536];
  KgtPictureSlotEntry pictureHeaders[8192];
  KgtPaletteWithPad   sharedPalettes[8];
  KgtSoundSlotEntry   soundHeaders[256];
  int32               trailer;
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtGameSystemData {    // 66,108 bytes (0x1023C) — follows KgtProjectSlot in KGT files
  KgtNameInfo           playerNames[50];
  KgtReactionItem       reactionItems[200];
  byte                  unknownByte0;          // default 2 — file-persisted, no editor writer
  byte                  unknownPad1_3[3];
  KgtRecoverTimeConfig  recoverTimeConfig;
  KgtNameInfo           stageNames[50];
  KgtNameInfo           demoNames[100];
  KgtGameDemoConfig     demoConfig;
  KgtProjectBaseConfig  projectBaseConfig;
  KgtThrowReaction      commonImageScripts[200];
  uint16                predefinedScriptIds[104];
  byte                  predefinedPad[56];
  KgtCharSelectConfig   charSelectConfig;
  byte                  playerSelectableInfos[50];
  byte                  trailingPadding[946];
};
#pragma pack(pop)""",

# --- Runtime structs (reverse-engineered in editor; SAME layout should hold in game binary) ---

"""#pragma pack(push, 1)
struct KgtRuntimeObject {     // 382 bytes — one entry per object in the pool (1024 entries)
  int32 state;                // +0   inner-loop filter wants ==2 (alive)
  int32 xflipIndicator;       // +4
  int32 xPos;                 // +8   q16 fixed-point pixels
  int32 yPos;                 // +12  q16 fixed-point pixels
  int32 flagMinus1;           // +16
  int32 direction;            // +20  & 1 = facing
  int32 accelX;               // +24
  int32 velX;                 // +28
  int32 accelY;               // +32
  int32 velY;                 // +36
  int32 flags40;              // +40  0xC0000000 for players, 0x40000000 for objects
  int32 currentItemIdx;       // +44  word — current scriptItem within the script
  int32 currentScriptId;      // +48  word
  int32 prevScriptId;         // +52
  int32 pendingAnim;          // +56
  int32 waitCountdown;        // +60  frames * 100
  int32 otherWait;            // +64
  int32 reactionParam0;       // +68
  int32 reactionParam1;       // +72
  int32 reactionParam2;       // +76
  int32 reactionParam3;       // +80
  int32 stanceId;             // +84
  int32 gravityBase;          // +88
  int32 facingBits;           // +92
  int32 roleClone;            // +96  == +342 (role)
  int32 opcode2_slot1;        // +100
  int32 opcode2_slot2;        // +104
  int32 opcode2_slot3;        // +108
  int32 opcode2_slot4;        // +112
  int32 opcode2_slot5;        // +116
  int32 opcode2_slot6;        // +120
  int32 callCount;            // +124
  int32 callReturnScriptIdPos;// +128
  int32 callTarget;           // +132
  byte  unknownArea137[80];   // +137..+216  object-manager slots A (opcode 0x18 writes here)
  void *boxArray[20];         // +217..+296  (+0xD9..+0x128) — hitbox pointer array (attacker reads 20 from here)
                              // Defender reads 20 pointers starting at +0xDD (= &boxArray[1]) — asymmetric skew, TODO in game binary
  byte  unknownArea297[8];    // +297..+304
  uint16 byteVariables[64];   // +305..+432? — at least +305 start per opcode 0x1F; length TBD
  // ... many more fields between +432 and +337 (varid at +337, +338 state)
  int32 afterimagePoolIdx;    // +337  1-based, 0 = none
  int32 stateInitialized;     // +338
  int32 playerIdx;            // +342  identifies which KgtPlayerRuntimeSlot
  int32 role;                 // +346  0=player, 1=object, 2=active-attack-object, etc.
  int32 flags350;             // +350
  byte  unknownTail[28];      // +354..+381  pad to 382
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPlayerRuntimeSlot { // 47,851 bytes (0xBAEB)
  byte  header[68];           // +0..+67
  void *compiledScripts;      // +68
  void *compiledScriptItems;  // +69 (word* into scriptItems)
  void *soundData;            // +72
  byte  _pad[5];              // +73..+77
  byte  stance;               // +78  1=standing, 2=ducking, ...
  byte  _pad2[47526];         // +79..+47604
  KgtRuntimeObject *currentObject;     // +47605
  KgtRuntimeObject *opponentObject;    // +47609
  byte  timerColorState[20];  // +47611..+47640  approx
  byte  unknownCount;         // +47641  default 20
  byte  _pad3[27];            // +47642..+47668
  byte  flag47669;            // +47669
  byte  _pad4[7];             // +47670..+47676
  byte  stanceFlag;           // +47677  0 or 5
  byte  _pad5[3];             // +47678..+47680
  int32 reset47681;           // +47681
  int32 reset47685;           // +47685
  int32 reset47689;           // +47689
  byte  flag47693;            // +47693
  byte  _pad6[11];            // +47694..+47704
  byte  reaction47705[6];     // +47705..+47710  set by opcode 0x1E [R]
  byte  byteVariables[128];   // +47711..+47838  local 2-byte vars (64 entries of 2 bytes)
  byte  _pad7[4];             // +47839..+47842
  void *spawnSlots[10];       // +47743..+47782  zeroed by memset 0x28 — spawn-slot pointers
  byte  _pad8[44];            // +47783..+47826
  byte  spawn47827[12];       // +47827..+47838
  byte  spawnPos47839[12];    // +47839..+47850
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtAfterimageEntry {   // 1616 bytes per entry in the afterimage pool (100 entries max)
  byte contents[1616];        // TODO: field layout TBD
};
#pragma pack(pop)""",

# --- Editor-side structs (may not exist in game binary, but harmless to declare) ---

"""#pragma pack(push, 1)
struct ParameterDescriptor {  // 32 bytes — drives numeric-input edit controls in the editor
  int32  min, max;            // +0, +4
  void  *target;              // +8   pointer to the KgtScriptItem field being edited
  int32  controlId;           // +12
  byte   unknown[16];         // +16..+31
  void  *hwnd;                // +28  HWND of the edit control
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtCommandInput {      // 82 bytes — the REAL "command list" entry (special moves)
  char   name[32];
  int32  commandTime;
  int32  scriptIds[4];
  byte   inputMasks[10];
  byte   thresholds[10];
  byte   unknown[4];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtCpuCondition {      // 7 bytes
  byte payload[7];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtCpuEntry {          // 110 bytes — one AI entry
  byte payload[110];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtStoryEntry {        // 206 bytes — story-mode event
  byte kind;                  // first byte tags the event type
  byte payload[205];
};
#pragma pack(pop)""",

"""#pragma pack(push, 1)
struct KgtPlayerFileBlocks {  // 48305 bytes total — all 7 player-specific blocks (see player_file_format.md)
  byte payload[48305];
};
#pragma pack(pop)""",

]


# Expected sizes (for verification post-declare)
EXPECTED = {
    'byte':1,'int8':1,'uint8':1,'int16':2,'uint16':2,'int32':4,'uint32':4,
    'KgtScript':39,'KgtScriptItem':16,'KgtPictureHeader':20,'KgtColorBgra':4,
    'KgtPalette':1024,'KgtSoundItemHeader':42,'KgtNameInfo':256,
    'KgtReactionItem':36,'KgtThrowReaction':32,'KgtFileHeader':272,
    'KgtRecoverTimeConfig':4,'KgtGameDemoConfig':8,'KgtProjectBaseConfig':4,
    'KgtCharSelectConfig':28,'KgtDemoConfig':9,'KgtStageConfig':4,
    'KgtHurtBindInfo':12,'KgtThrowActionInfo':16,
    'KgtShowPic':10,'KgtMoveCmd':10,'KgtPlaySoundCmd':4,'KgtColorSetCmd':6,
    'KgtJumpCmd':4,'KgtLoopCmd':5,'KgtRandomCmd':9,'KgtObjectCmd':14,
    'KgtVariableCmd':11,'KgtAfterimageCmd':11,'KgtStageStart':6,'KgtPos':5,
    'KgtPictureSlotEntry':20,'KgtSoundSlotEntry':42,'KgtPaletteWithPad':1056,
    'KgtProjectSlot':1271828,'KgtGameSystemData':66108,
    'KgtRuntimeObject':382,'KgtPlayerRuntimeSlot':47851,
    'KgtAfterimageEntry':1616,'ParameterDescriptor':32,
    'KgtCommandInput':82,'KgtCpuCondition':7,'KgtCpuEntry':110,
    'KgtStoryEntry':206,'KgtPlayerFileBlocks':48305,
}


def main():
    til = idaapi.get_idati()
    print("Declaring structs...")
    declared = 0
    failed = []
    for decl in DECLS:
        tif = ida_typeinf.tinfo_t()
        # Use parse_decl — PT_TYP = accept a type declaration
        r = ida_typeinf.parse_decl(tif, til, decl + ";", ida_typeinf.PT_TYP | ida_typeinf.PT_SIL)
        if r is None:
            # Try direct execution path via ida_typeinf.idc_parse_types or SetLocalType
            rc = idaapi.idc_parse_types(decl + ";", 0)
            if rc:
                declared += 1
            else:
                failed.append(decl.split('struct ')[1].split()[0] if 'struct ' in decl else decl[:40])
        else:
            declared += 1
    print(f"Declared: {declared}/{len(DECLS)}")
    if failed:
        print(f"Failed: {failed}")

    # Verify sizes
    print("\nSize verification:")
    problems = []
    for name, expected_size in EXPECTED.items():
        tif = ida_typeinf.tinfo_t()
        ok = tif.get_named_type(til, name)
        if not ok:
            problems.append((name, "NOT FOUND", expected_size))
            continue
        actual = tif.get_size()
        if actual != expected_size:
            problems.append((name, actual, expected_size))
    if problems:
        for name, actual, expected_size in problems:
            print(f"  {name}: got {actual}, expected {expected_size}")
    else:
        print(f"  All {len(EXPECTED)} types have correct sizes ✓")


# ----------------------------------------------------------------------------
# Hunt helpers — run these AFTER declaring to find the game binary's
# equivalents of key editor functions/globals.
# ----------------------------------------------------------------------------

def hunt_read_common_resource_part():
    """
    Find the equivalent of ReadCommonResourcePart@editor:0x428BE0.
    Signature: int __cdecl (KgtProjectSlot *slot, HANDLE hFile).
    Characteristic: 6+ ReadFile calls, immediate values 0x400, 0x10000, 0x2000, 0x100
    (the section-count caps for scripts/scriptItems/pictures/sounds).
    """
    import idautils
    print("Hunting ReadCommonResourcePart... (looking for functions with all of 0x400, 0x10000, 0x2000, 0x100 as immediates)")
    targets = {0x400: set(), 0x10000: set(), 0x2000: set(), 0x100: set()}
    for ea in idautils.Functions():
        import ida_funcs
        fn = ida_funcs.get_func(ea)
        if not fn: continue
        import ida_ua
        insn = ida_ua.insn_t()
        for a in range(fn.start_ea, fn.end_ea):
            ida_ua.decode_insn(insn, a)
            for op in insn.ops:
                if op.type == 5 and op.value in targets:  # o_imm
                    targets[op.value].add(ea)
    candidates = set.intersection(*targets.values())
    for c in candidates:
        print(f"  Candidate: {hex(c)} {idaapi.get_func_name(c)}")
    return candidates


def hunt_execute_animation_script():
    """
    Find the equivalent of ExecuteAnimationScript@editor:0x439CD0.
    Characteristic: large function (>4KB), references a global that's also referenced with
    stride-382 access patterns (the object pool), has a 42-entry jump table.
    Easier: look for a function with a switch on ~0x25+ opcodes, or that calls
    ReturnCharValue, DestroyGameObject, or similar.
    """
    print("Manual hunt: look for the largest function in .text. In the editor it was ~7.9KB.")
    print("Filter by: has big switch/jump-table, reads from one global with stride 382.")


def hunt_object_pool():
    """
    Find the equivalent of g_objectPool@editor:0x775900 (stride 382, 1024 entries).
    Characteristic: a data address that's accessed with stride 382 in loops.
    """
    print("Manual hunt: set KgtRuntimeObject* on candidate globals and look for")
    print("decompile cleanups that show 382*N indexing.")


if __name__ == "__main__":
    main()
    print("\n--- Now available: run hunt_read_common_resource_part() etc. to find game-binary equivalents.")
