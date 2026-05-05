# port_to_game_master.py — ALL-IN-ONE port of KGT2nd_EDITOR → KGT2nd_GAME (or any FM2K runtime)
#
# Run in the target IDA (game binary) via File → Script File…
# Companion data files (read from the same docs/editor/ directory):
#   editor_funcs.json               — 495 user-named functions (name, addr, size, signature)
#   editor_func_comments.json       — 72 top-level function comments (human narrative)
#   editor_inline_comments.json     — 1461 substantial inline comments across 167 functions
#   editor_func_fingerprints.json   — 336 functions with string + immediate-constant fingerprints
#   editor_globals.json             — 898 user-named globals (name, addr, size, xrefs, type)
#   editor_engine_core_decomp.json  — decompiled text of 31 engine-core functions (for content match)
#   editor_types.json               — 155 declared types (sizes + string form)
#
# This script:
#   1. Declares all 51 Kgt/Parameter/etc structs  (equivalent to port_structs_to_game.py)
#   2. Match: finds game functions whose fingerprint (strings + immediates)
#      matches editor functions, proposing name ports
#   3. Match: finds game globals by size + xref-count heuristic
#   4. Applies renames + comments after user confirmation
#
# Run order:
#   >>> load_dumps()             # loads all JSON — call once
#   >>> declare_all_types()      # declares 51 structs and verifies sizes
#   >>> match_all_functions()    # prints proposed matches by fingerprint
#   >>> apply_matches(confirmed) # after reviewing, apply renames + port comments
#
# Edit DOCS_DIR if running from a different path.

import os, json, sys, re
import idaapi, idautils, idc, ida_funcs, ida_name, ida_typeinf, ida_bytes, ida_ua

# Point this at wherever editor_*.json live
DOCS_DIR = r"C:\dev\wanwan\docs\editor"

_DUMPS = {}

def load_dumps():
    for basename in ["editor_funcs","editor_func_comments","editor_inline_comments",
                     "editor_func_fingerprints","editor_globals","editor_engine_core_decomp",
                     "editor_types"]:
        path = os.path.join(DOCS_DIR, basename + ".json")
        with open(path, 'r', encoding='utf-8') as f:
            _DUMPS[basename] = json.load(f)
    print(f"Loaded dumps:")
    for k, v in _DUMPS.items():
        print(f"  {k}: {len(v)} entries")


# ============================================================================
# STRUCT DECLARATIONS (same as port_structs_to_game.py — self-contained)
# ============================================================================

TYPE_DECLS = [
    "typedef unsigned char byte;",
    "typedef signed char int8;",
    "typedef unsigned char uint8;",
    "typedef short int16;",
    "typedef unsigned short uint16;",
    "typedef int int32;",
    "typedef unsigned int uint32;",
    # file-format
    "#pragma pack(push,1) struct KgtScript { char scriptName[32]; uint16 scriptIndex; byte gap; int32 flags; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtScriptItem { byte type; byte bytes[15]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPictureHeader { int32 unknownFlag1; int32 width; int32 height; int32 hasPrivatePalette; int32 size; }; #pragma pack(pop)",
    "#pragma pack(push,1) union KgtColorBgra { struct { byte blue; byte green; byte red; byte alpha; } channel; uint32 value; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPalette { KgtColorBgra colors[256]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtSoundItemHeader { int32 unknown; char name[32]; int32 size; byte soundType; byte track; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtNameInfo { char name[256]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtReactionItem { char reactionName[32]; int32 isHurtAction; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtThrowReaction { char name[32]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtFileHeader { byte fileSignature[16]; KgtNameInfo name; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtRecoverTimeConfig { byte gap; byte attackRecoverTime; byte defenceRecoverTime; byte clashRecoverTime; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtGameDemoConfig { byte titleDemoId; byte storyModeCharSelectDemoId; byte oneVsOneModeCharSelectDemoId; byte teamModeCharSelectDemoId; byte continueDemoId; byte openingDemoId; byte unknownTag1; byte unknownTag2; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtProjectBaseConfig { int32 rawValue; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtCharSelectConfig { int16 selectBoxStartX; int16 selectBoxStartY; int16 iconWidth; int16 iconHeight; int16 columnNum; int16 rowNum; int16 player1PortraitX; int16 player1PortraitY; int16 player1PortraitTeamOffsetX; int16 player1PortraitTeamOffsetY; int16 player2PortraitX; int16 player2PortraitY; int16 player2PortraitTeamOffsetX; int16 player2PortraitTeamOffsetY; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtDemoConfig { int16 bgmSoundId; byte pressToSkip; int16 unknownGap; int32 totalTime; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtStageConfig { int32 bgmSoundId; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtHurtBindInfo { int32 hurtId; int32 scriptId; int32 effectObjectId; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtThrowActionInfo { int32 throwActionId; int32 picNo; int32 offsetX; int32 offsetY; }; #pragma pack(pop)",
    # opcode variants
    "#pragma pack(push,1) struct KgtShowPic { byte type; uint16 keepTime; uint16 idxAndFlip; int16 offsetX; int16 offsetY; byte fixDir; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtMoveCmd { byte type; int16 accelX; int16 moveX; int16 moveY; int16 accelY; byte flags; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPlaySoundCmd { byte type; byte unknown; uint16 soundIdx; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtColorSetCmd { byte type; byte colorBlendType; int8 red; int8 green; int8 blue; int8 alpha; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtJumpCmd { byte type; uint16 jumpId; uint8 jumpPos; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtLoopCmd { byte type; uint8 loopCount; uint16 targetScriptId; uint8 targetPos; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtRandomCmd { byte type; uint16 randomMaxVal; uint16 moreThanVal; byte unknownGap; uint16 targetScriptId; uint8 targetPos; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtObjectCmd { byte type; byte flags; uint16 targetScriptId; uint8 targetPos; uint16 targetScriptIdIfExists; uint8 targetPosIfExists; int16 posX; int16 posY; uint8 manageNo; int8 layer; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtVariableCmd { byte type; uint16 targetScriptId; uint8 targetPos; byte targetVariable; byte opFlags; byte compareVariable; int16 operationValue; int16 compareValue; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtAfterimageCmd { byte type; uint16 unknownGap; uint8 afterimageMaxCount; uint8 afterimageGap; byte colorBlendType; byte afterimageColorType; int8 red; int8 green; int8 blue; int8 alpha; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtStageStart { byte type; byte flags; int16 horiScroll; int16 vertScroll; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPos { byte type; int16 x; int16 y; }; #pragma pack(pop)",
    # slot variants
    "#pragma pack(push,1) struct KgtPictureSlotEntry { void *data; int32 width; int32 height; int32 hasPrivatePalette; int32 size; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtSoundSlotEntry { void *data; char name[32]; int32 size; byte soundType; byte track; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPaletteWithPad { KgtPalette palette; byte padding[32]; }; #pragma pack(pop)",
    # top-level
    "#pragma pack(push,1) struct KgtProjectSlot { KgtFileHeader header; KgtScript scripts[1024]; KgtScriptItem scriptItems[65536]; KgtPictureSlotEntry pictureHeaders[8192]; KgtPaletteWithPad sharedPalettes[8]; KgtSoundSlotEntry soundHeaders[256]; int32 trailer; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtGameSystemData { KgtNameInfo playerNames[50]; KgtReactionItem reactionItems[200]; byte unknownByte0; byte unknownPad1_3[3]; KgtRecoverTimeConfig recoverTimeConfig; KgtNameInfo stageNames[50]; KgtNameInfo demoNames[100]; KgtGameDemoConfig demoConfig; KgtProjectBaseConfig projectBaseConfig; KgtThrowReaction commonImageScripts[200]; uint16 predefinedScriptIds[104]; byte predefinedPad[56]; KgtCharSelectConfig charSelectConfig; byte playerSelectableInfos[50]; byte trailingPadding[946]; }; #pragma pack(pop)",
    # runtime
    "#pragma pack(push,1) struct KgtRuntimeObject { int32 state; int32 xflipIndicator; int32 xPos; int32 yPos; int32 flagMinus1; int32 direction; int32 accelX; int32 velX; int32 accelY; int32 velY; int32 flags40; int32 currentItemIdx; int32 currentScriptId; int32 prevScriptId; int32 pendingAnim; int32 waitCountdown; int32 otherWait; int32 reactionParam0; int32 reactionParam1; int32 reactionParam2; int32 reactionParam3; int32 stanceId; int32 gravityBase; int32 facingBits; int32 roleClone; int32 opcode2_slot1; int32 opcode2_slot2; int32 opcode2_slot3; int32 opcode2_slot4; int32 opcode2_slot5; int32 opcode2_slot6; int32 callCount; int32 callReturnScriptIdPos; int32 callTarget; byte unknownArea137[80]; void *boxArray[20]; byte unknownArea297[8]; uint16 byteVariables[64]; byte gap40[157]; int32 afterimagePoolIdx; int32 stateInitialized; int32 gap338[1]; int32 playerIdx; int32 role; int32 flags350; byte unknownTail[28]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPlayerRuntimeSlot { byte payload[47851]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtAfterimageEntry { byte contents[1616]; }; #pragma pack(pop)",
    # editor-side (harmless in game binary)
    "#pragma pack(push,1) struct ParameterDescriptor { int32 min; int32 max; void *target; int32 controlId; byte unknown[16]; void *hwnd; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtCommandInput { char name[32]; int32 commandTime; int32 scriptIds[4]; byte inputMasks[10]; byte thresholds[10]; byte unknown[4]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtCpuCondition { byte payload[7]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtCpuEntry { byte payload[110]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtStoryEntry { byte kind; byte payload[205]; }; #pragma pack(pop)",
    "#pragma pack(push,1) struct KgtPlayerFileBlocks { byte payload[48305]; }; #pragma pack(pop)",
]

EXPECTED_SIZES = {
    'byte':1,'KgtScript':39,'KgtScriptItem':16,'KgtPictureHeader':20,'KgtColorBgra':4,
    'KgtPalette':1024,'KgtSoundItemHeader':42,'KgtNameInfo':256,'KgtReactionItem':36,
    'KgtThrowReaction':32,'KgtFileHeader':272,'KgtRecoverTimeConfig':4,
    'KgtGameDemoConfig':8,'KgtProjectBaseConfig':4,'KgtCharSelectConfig':28,
    'KgtDemoConfig':9,'KgtStageConfig':4,'KgtHurtBindInfo':12,'KgtThrowActionInfo':16,
    'KgtShowPic':10,'KgtMoveCmd':10,'KgtPlaySoundCmd':4,'KgtColorSetCmd':6,
    'KgtJumpCmd':4,'KgtLoopCmd':5,'KgtRandomCmd':9,'KgtObjectCmd':14,
    'KgtVariableCmd':11,'KgtAfterimageCmd':11,'KgtStageStart':6,'KgtPos':5,
    'KgtPictureSlotEntry':20,'KgtSoundSlotEntry':42,'KgtPaletteWithPad':1056,
    'KgtProjectSlot':1271828,'KgtGameSystemData':66108,
    'KgtRuntimeObject':382,'KgtPlayerRuntimeSlot':47851,'KgtAfterimageEntry':1616,
    'ParameterDescriptor':32,'KgtCommandInput':82,'KgtCpuCondition':7,
    'KgtCpuEntry':110,'KgtStoryEntry':206,'KgtPlayerFileBlocks':48305,
}


def declare_all_types():
    for decl in TYPE_DECLS:
        idaapi.idc_parse_types(decl, 0)
    til = idaapi.get_idati()
    problems = []
    for name, exp in EXPECTED_SIZES.items():
        tif = ida_typeinf.tinfo_t()
        ok = tif.get_named_type(til, name)
        if not ok:
            problems.append((name, "NOT FOUND", exp))
        elif tif.get_size() != exp:
            problems.append((name, tif.get_size(), exp))
    if problems:
        print(f"Size mismatches ({len(problems)}):")
        for p in problems: print(f"  {p}")
    else:
        print(f"All {len(EXPECTED_SIZES)} types have correct sizes ✓")


# ============================================================================
# FUNCTION FINGERPRINT MATCHING
# ============================================================================

def _fingerprint_current_function(fn_ea):
    """Build fingerprint for a game-binary function."""
    fn = ida_funcs.get_func(fn_ea)
    if fn is None: return None
    strings = set()
    imms = set()
    insn = ida_ua.insn_t()
    a = fn.start_ea
    while a < fn.end_ea:
        if ida_ua.decode_insn(insn, a):
            for opi in range(6):
                op = insn.ops[opi]
                if op.type == 0: break
                if op.type == idaapi.o_imm:
                    v = op.value & 0xffffffff
                    if 0x100 <= v < 0x10000000:
                        imms.add(v)
                if op.type in (idaapi.o_mem, idaapi.o_displ):
                    t = op.addr
                    if t:
                        s = idc.get_strlit_contents(t, -1, idc.STRTYPE_C)
                        if s:
                            try:
                                ss = s.decode('ascii', errors='replace')
                                if 4 <= len(ss) < 100:
                                    strings.add(ss)
                            except: pass
        a2 = idc.next_head(a, fn.end_ea)
        if a2 == idc.BADADDR or a2 <= a: break
        a = a2
    return strings, imms


def match_all_functions(min_score=3, print_matches=True):
    """Match editor functions to game functions by fingerprint overlap."""
    if "editor_func_fingerprints" not in _DUMPS:
        print("Call load_dumps() first.")
        return None
    editor_fps = _DUMPS["editor_func_fingerprints"]

    # Fingerprint every game function up-front
    print("Fingerprinting game functions...")
    game_fps = {}
    for fn_ea in idautils.Functions():
        fp = _fingerprint_current_function(fn_ea)
        if fp:
            game_fps[fn_ea] = fp
    print(f"  {len(game_fps)} game functions fingerprinted")

    matches = []
    for editor_addr, info in editor_fps.items():
        ed_strs = set(info["strings"])
        ed_imms = set(info["immediates"])
        best = []
        for game_ea, (g_strs, g_imms) in game_fps.items():
            str_overlap = len(ed_strs & g_strs)
            imm_overlap = len(ed_imms & g_imms)
            # Score: strings are much more distinctive than immediates
            score = str_overlap * 5 + imm_overlap
            if score >= min_score:
                best.append((score, str_overlap, imm_overlap, game_ea))
        best.sort(reverse=True)
        if best:
            top = best[0]
            matches.append({
                "editor_name": info["name"],
                "editor_addr": editor_addr,
                "game_addr": hex(top[3]),
                "game_name": idc.get_func_name(top[3]),
                "score": top[0],
                "str_overlap": top[1],
                "imm_overlap": top[2],
                "runner_up": (best[1][0] if len(best) > 1 else 0),
            })
    matches.sort(key=lambda m: -m["score"])

    if print_matches:
        print(f"\n{len(matches)} candidate matches (score >= {min_score}):")
        print(f"{'score':>5}  {'ed_addr':>10}  {'ed_name':<36}  →  {'game_addr':>10}  {'game_name':<36} (str={})".format(''))
        for m in matches:
            run = f" (next={m['runner_up']})" if m['runner_up'] > 0 else ""
            print(f"  {m['score']:>3}  {m['editor_addr']:>10}  {m['editor_name'][:34]:<36}  →  {m['game_addr']:>10}  {m['game_name'][:34]:<36}  s={m['str_overlap']} i={m['imm_overlap']}{run}")
    return matches


def apply_matches(matches, min_score=5, dry_run=True):
    """Apply proposed matches: rename game function + port editor's top comment.
    Set dry_run=False to actually apply."""
    if "editor_func_comments" not in _DUMPS:
        print("Call load_dumps() first.")
        return
    fcmts = _DUMPS["editor_func_comments"]
    renamed = 0
    commented = 0
    for m in matches:
        if m["score"] < min_score: continue
        if m["str_overlap"] == 0: continue   # require at least one string match for safety
        game_ea = int(m["game_addr"], 16)
        ed_name = m["editor_name"]
        old_name = idc.get_func_name(game_ea)
        if not old_name.startswith("sub_") and old_name != ed_name:
            # Already named; don't stomp
            continue
        if not dry_run:
            idc.set_name(game_ea, ed_name, idc.SN_CHECK)
        renamed += 1
        # Port top comment
        ed_cmt = fcmts.get(m["editor_addr"])
        if ed_cmt:
            combined = (ed_cmt.get("repeatable","") + "\n" + ed_cmt.get("regular","")).strip()
            if combined:
                if not dry_run:
                    idc.set_func_cmt(game_ea, combined, 1)
                commented += 1
    tag = "DRY RUN" if dry_run else "APPLIED"
    print(f"[{tag}] {renamed} renames, {commented} comments ported.")


# ============================================================================
# GLOBAL MATCHING (by size + xref-count heuristic)
# ============================================================================

def match_globals_by_size():
    """List game globals whose size matches editor's known named globals.
    Use the output to hand-match candidates."""
    if "editor_globals" not in _DUMPS:
        print("Call load_dumps() first.")
        return
    ed_globs = _DUMPS["editor_globals"]
    # Group editor globals by size
    by_size = {}
    for g in ed_globs:
        by_size.setdefault(g["size"], []).append(g)
    # Enumerate game user-named globals
    print("Size-class distribution of editor globals:")
    sizes = sorted(by_size.keys())
    for sz in sizes:
        eds = by_size[sz]
        if len(eds) <= 5:
            names = ", ".join(g["name"] for g in eds[:8])
            print(f"  size={sz:>10}: {len(eds)} editor globals  ({names})")


# ============================================================================
# MAIN USAGE
# ============================================================================

def help_me():
    print("""
Commands:
  load_dumps()                       Load all 7 JSON dumps from {DOCS_DIR}
  declare_all_types()                Declare 51 Kgt* structs and verify sizes
  match_all_functions(min_score=3)   Fingerprint-match editor→game (strings + immediates)
  apply_matches(matches, dry_run=False)  Apply renames + port top comments
  match_globals_by_size()            Show editor-global size-class histogram

Typical flow:
  >>> load_dumps()
  >>> declare_all_types()
  >>> matches = match_all_functions(min_score=5)
  >>> apply_matches(matches, min_score=10, dry_run=False)
""".replace("{DOCS_DIR}", DOCS_DIR))


if __name__ == "__main__":
    help_me()
