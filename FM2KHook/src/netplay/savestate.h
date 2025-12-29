#pragma once
#include "shared_mem.h"
#include <cstdint>

// Minimal savestate API for GekkoNet rollback
void SaveState_Init();
bool SaveState_Save(int frame);
bool SaveState_Load(int frame);
uint32_t SaveState_GetLastChecksum(int frame);
