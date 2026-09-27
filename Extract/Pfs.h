#pragma once

#include "ExtractBase.h"

struct SFileInfo;

// Artemis engine .pfs archives ("pf8"), whose entries are XORed with the SHA-1 of the index.
// The game data is split into "xxx.pfs", "xxx.pfs.000", "xxx.pfs.001", ...; opening "xxx.pfs"
// lists all of them together.
//
// Standing sprites are stored as body and face parts that carry their screen position
// in a PNG text chunk. When the extras table (exlist.ipt) is present, every body/face
// combination of the largest size ("z2") is listed and extracted as one composed image.
class CPfs final : public CExtractBase
{
public:
	static constexpr size_t key_size = 20;
	using Key = std::array<u8, key_size>;

	bool Mount(CArcFile* archive) override;
	bool Decode(CArcFile* archive) override;
	bool Extract(CArcFile* archive) override;

private:
	struct Bitmap
	{
		u32 width = 0;
		u32 height = 0;
		s32 x = 0; // Screen position from the PNG comment
		s32 y = 0;
		std::vector<u8> pixels; // Top-down BGRA
	};

	bool ReadPart(CArcFile* archive, std::vector<SFileInfo>* file_infos, std::vector<u32>* parts, std::vector<Key>* keys, std::unordered_set<std::string>* names);
	bool DecodeSprite(CArcFile* archive, const Key& key);

	// Returns either the cached body or `scratch`, or nullptr on failure.
	const Bitmap* DecodePart(CArcFile* archive, const Key& key, size_t part, Bitmap* scratch);

	// Decryption keys by archive path
	std::map<std::string, Key> m_keys;

	// Many faces are drawn onto the same body, so keep the last one around.
	YCString m_cached_archive_path;
	u64 m_cached_offset = 0;
	Bitmap m_cached_body;
};
