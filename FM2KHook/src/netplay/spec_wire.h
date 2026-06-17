#pragma once
// Spectator wire codecs (implemented in spec_wire.cpp). The SessionEvent_*
// encode/decode functions are declared in spectator_node.h (their long-standing
// home); this header only adds the zero-RLE snapshot codec, which spectator_node.cpp
// calls from SendSnapshotTo (compress) and the SNAPSHOT_END handler (decompress).
#include <cstdint>
#include <cstddef>
#include <vector>

void ZeroRleCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);
bool ZeroRleDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
