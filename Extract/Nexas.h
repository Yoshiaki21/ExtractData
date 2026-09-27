#pragma once

#include "ExtractBase.h"

// Giga NeXAS engine .pac archives whose index is stored
// Huffman-compressed at the end of the file.
//
// When an archive contains .spm sprite definitions, the sprite parts
// (body + expression overlays) are hidden and each composed frame is
// listed and extracted as a single image instead.
class CNexas final : public CExtractBase
{
public:
	bool Mount(CArcFile* archive) override;
	bool Decode(CArcFile* archive) override;
	bool Extract(CArcFile* archive) override;

private:
	struct Bitmap
	{
		u32 width = 0;
		u32 height = 0;
		std::vector<u8> pixels; // Top-down BGRA
	};

	// Returns either the cached body part or `scratch`, or nullptr on failure.
	const Bitmap* DecodePart(CArcFile* archive, size_t part, Bitmap* scratch);

	// The body part is shared by every frame of a sprite, so keep the last one around.
	YCString m_cached_archive_path;
	u64 m_cached_offset = 0;
	Bitmap m_cached_part;
};
