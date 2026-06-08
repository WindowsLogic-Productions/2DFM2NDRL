#pragma once
#include <cstdint>

// Simple input capture - just reads keyboard and returns input bits
// Input bit layout (matches FM2K):
//   0x001 = Left
//   0x002 = Right
//   0x004 = Up
//   0x008 = Down
//   0x010 = Button 1
//   0x020 = Button 2
//   0x040 = Button 3
//   0x080 = Button 4
//   0x100 = Button 5
//   0x200 = Button 6
//   0x400 = Pause

uint16_t Input_CaptureLocal();

// Sample ONE player's bound input (binder slot `player`), with the same
// focus guard + CSS START mask as Input_CaptureLocal. Returns 0 if our
// window isn't focused or that slot has no bindings. Used by the local-2P
// stress path so P2 reads its OWN bindings (e.g. keyboard) instead of being
// shadowed by P1's controller. Assumes the binder was already Init/Load'd
// this frame by an Input_CaptureLocal() call (the stress path does that).
uint16_t Input_CaptureLocalPlayer(int player);
