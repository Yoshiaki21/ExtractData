#include "StdAfx.h"
#include "Extract/Nexas.h"

#include "ArcFile.h"
#include "Common.h"

namespace
{
constexpr u32 header_size = 12;
constexpr size_t entry_name_size = 64;
constexpr size_t entry_size = entry_name_size + 12;

enum PackMethod : u32
{
	PACK_NONE = 0,
	PACK_ZLIB = 4,
};

class BitReader
{
public:
	explicit BitReader(const std::vector<u8>& data) : m_data{data} {}

	// Returns false once the input is exhausted.
	bool GetBits(u32 count, u32* out)
	{
		u32 value = 0;
		for (u32 i = 0; i < count; i++)
		{
			if (m_bits_left == 0)
			{
				if (m_pos >= m_data.size())
					return false;

				m_current = m_data[m_pos++];
				m_bits_left = 8;
			}

			m_bits_left--;
			value = (value << 1) | ((m_current >> m_bits_left) & 1);
		}

		*out = value;
		return true;
	}

private:
	const std::vector<u8>& m_data;
	size_t m_pos = 0;
	u8 m_current = 0;
	u32 m_bits_left = 0;
};

class HuffmanDecoder
{
public:
	explicit HuffmanDecoder(const std::vector<u8>& data) : m_reader{data} {}

	bool Decode(u8* dst, size_t dst_size)
	{
		u32 root;
		if (!CreateTree(&root))
			return false;

		for (size_t i = 0; i < dst_size; i++)
		{
			u32 node = root;
			while (node >= 256)
			{
				u32 bit;
				if (!m_reader.GetBits(1, &bit))
					return false;

				node = bit != 0 ? m_rhs[node - 256] : m_lhs[node - 256];
			}

			dst[i] = static_cast<u8>(node);
		}

		return true;
	}

private:
	// Branch nodes are numbered from 256 upwards; leaves are byte values.
	bool CreateTree(u32* node)
	{
		u32 bit;
		if (!m_reader.GetBits(1, &bit))
			return false;

		if (bit == 0)
			return m_reader.GetBits(8, node);

		if (m_next_node >= 256 + m_lhs.size())
			return false;

		const u32 branch = m_next_node++;
		if (!CreateTree(&m_lhs[branch - 256]) || !CreateTree(&m_rhs[branch - 256]))
			return false;

		*node = branch;
		return true;
	}

	BitReader m_reader;
	std::array<u32, 256> m_lhs{};
	std::array<u32, 256> m_rhs{};
	u32 m_next_node = 256;
};
} // Anonymous namespace

/// Mounting
bool CNexas::Mount(CArcFile* archive)
{
	if (lstrcmpi(archive->GetArcExten(), _T(".pac")) != 0)
		return false;

	if (std::memcmp(archive->GetHeader(), "PAC", 3) != 0)
		return false;

	const u64 archive_size = archive->GetArcSize();
	if (archive_size < header_size + 4)
		return false;

	u32 num_files;
	u32 pack_method;
	archive->SeekHed(4);
	archive->ReadU32(&num_files);
	archive->ReadU32(&pack_method);

	if (num_files == 0 || (pack_method != PACK_NONE && pack_method != PACK_ZLIB))
	{
		archive->SeekHed();
		return false;
	}

	// The last four bytes hold the size of the compressed index that precedes them
	u32 index_size;
	archive->SeekEnd(4);
	archive->ReadU32(&index_size);

	if (index_size == 0 || index_size > archive_size - header_size - 4)
	{
		archive->SeekHed();
		return false;
	}

	std::vector<u8> packed_index(index_size);
	archive->SeekEnd(4 + static_cast<s64>(index_size));
	archive->Read(packed_index.data(), index_size);

	for (auto& byte : packed_index)
		byte ^= 0xFF;

	std::vector<u8> index(static_cast<size_t>(num_files) * entry_size);
	HuffmanDecoder decoder(packed_index);
	if (!decoder.Decode(index.data(), index.size()))
	{
		archive->SeekHed();
		return false;
	}

	// Validate every entry before adding anything, so that other
	// "PAC" formats (e.g. CBaldr) still get a chance to mount.
	std::vector<SFileInfo> file_infos(num_files);
	for (u32 i = 0; i < num_files; i++)
	{
		const u8* entry = &index[i * entry_size];

		char name[entry_name_size + 1] = {};
		std::memcpy(name, entry, entry_name_size);

		u32 offset;
		u32 original_size;
		u32 packed_size;
		std::memcpy(&offset, &entry[entry_name_size], sizeof(u32));
		std::memcpy(&original_size, &entry[entry_name_size + 4], sizeof(u32));
		std::memcpy(&packed_size, &entry[entry_name_size + 8], sizeof(u32));

		if (name[0] == '\0' || offset < header_size || static_cast<u64>(offset) + packed_size > archive_size)
		{
			archive->SeekHed();
			return false;
		}

		SFileInfo& file_info = file_infos[i];
		file_info.name = name;
		file_info.start = offset;
		file_info.end = file_info.start + packed_size;
		file_info.size_cmp = packed_size;
		file_info.size_org = original_size;

		// Entries that were not worth compressing are stored as-is
		if (pack_method == PACK_ZLIB && packed_size != original_size)
			file_info.format = _T("zlib");
	}

	for (auto& file_info : file_infos)
		archive->AddFileInfo(file_info);

	return true;
}
