// Slim header-only port of 厉猛's (limen's) 2DFM file format definitions.
//
// Source: /mnt/c/dev/wanwan/2dfm/{2dfmFile.hpp, 2dfmCommon.hpp} (also at
// 2DFM_Player/Source/2dfm/). Only the structural pieces needed to *seek
// past* the common-resource section and *read names* are kept — script
// payloads, picture/sprite content, palette/sound binary data, and the
// axmol render dependencies are deliberately omitted.
//
// Used by FM2K_KgtParser to extract player/stage/demo name lists from a
// .kgt file pre-launch, so the launcher can populate dropdowns without
// having to first boot the game and ReadProcessMemory the in-memory
// buffers.

#pragma once
#include <cstdint>

typedef unsigned char byte;

namespace _2dfm {

#pragma pack(push, 1)

constexpr int SCRIPT_SIZE          = 39;
constexpr int SCRIPT_ITEM_SIZE     = 16;
constexpr int PICTURE_HEADER_SIZE  = 20;
constexpr int COLOR_SIZE           = 4;
constexpr int PALETTE_SIZE         = COLOR_SIZE * 256;     // 1024 bytes
constexpr int SOUND_ITEM_HEADER_SIZE = 42;

struct PictureHeader {
    int unknownFlag1;        // 0..3
    int width;               // 4..7
    int height;              // 8..11
    int hasPrivatePalette;   // 12..15
    int size;                // 16..19  — actual payload size in file
};

struct SoundItemHeader {
    int unknown;             // 0..3
    char name[32];           // 4..35
    int size;                // 36..39
    byte soundType;          // 40
    byte track;              // 41
};

#pragma pack(pop)

// Per-entry name slot — fixed 256-byte SJIS/GBK NUL-terminated string.
struct NameInfo {
    char name[256];
};
static_assert(sizeof(NameInfo) == 256, "NameInfo must be exactly 256 bytes");

// Per-entry reaction record.
struct ReactionItem {
    char reactionName[32];
    int  isHurtAction;
};
static_assert(sizeof(ReactionItem) == 36, "ReactionItem must be 36 bytes");

struct ThrowReaction {
    char name[32];
};
static_assert(sizeof(ThrowReaction) == 32, "ThrowReaction must be 32 bytes");

constexpr int KGT_FILE_HEADER_SIZE = 16 + 256;
struct KgtFileHeader {
    byte     fileSignature[16];
    NameInfo name;
};
static_assert(sizeof(KgtFileHeader) == KGT_FILE_HEADER_SIZE, "KgtFileHeader size");

struct RecoverTimeConfig {
    byte gap;
    byte attackRecoverTime;
    byte defenceRecoverTime;
    byte clashRecoverTime;
};

constexpr int DEMO_CONFIG_SIZE = 8;
struct GameDemoConfig {
    byte titleDemoId;
    byte storyModeCharSelectDemoId;
    byte oneVsOneModeCharSelectDemoId;
    byte teamModeCharSelectDemoId;
    byte continueDemoId;
    byte openingDemoId;
    byte unknownTag1;
    byte unknownTag2;
};

union ProjectBaseConfig {
    int32_t rawValue;
    struct {
        int encryptGame      : 1;
        int allowClash       : 1;
        int enableStoryMode  : 1;
        int enable1V1Mode    : 1;
        int enableTeamMode   : 1;
        int showHpAfterHpBar : 1;
        int pressToStart     : 1;
    } value;
};

struct CharSelectConfig {
    int16_t selectBoxStartX;
    int16_t selectBoxStartY;
    int16_t iconWidth;
    int16_t iconHeight;
    int16_t columnNum;
    int16_t rowNum;
    int16_t player1PortraitX;
    int16_t player1PortraitY;
    int16_t player1PortraitTeamOffsetX;
    int16_t player1PortraitTeamOffsetY;
    int16_t player2PortraitX;
    int16_t player2PortraitY;
    int16_t player2PortraitTeamOffsetX;
    int16_t player2PortraitTeamOffsetY;
};

// Slot counts — verified against IDA's g_*_file_buffer extents.
constexpr int maxPlayerNum        = 50;
constexpr int maxStageNum         = 50;
constexpr int maxDemoNum          = 100;
constexpr int maxReactionNum      = 200;
constexpr int maxThrowReactionNum = 200;

} // namespace _2dfm
