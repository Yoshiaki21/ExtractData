#pragma once

namespace ImageUtils
{
// Decodes a PNG file in memory into top-down, straight-alpha BGRA pixels.
bool DecodePng(const u8* data, size_t size, u32* width, u32* height, std::vector<u8>* bgra);

// Draws `src` over `dst` (both top-down, straight-alpha BGRA) with its top-left corner at (x, y).
// `src` must fit entirely inside `dst`.
void BlendOver(std::vector<u8>& dst, u32 dst_width, const std::vector<u8>& src, u32 src_width, u32 src_height, u32 x, u32 y);
}
