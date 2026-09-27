#include "StdAfx.h"
#include "Extract/Nexas.h"

#include "Arc/Zlib.h"
#include "ArcFile.h"
#include "Common.h"
#include "Error.h"
#include "Image.h"
#include "UI/ProgressBar.h"

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

// SFileInfo::format of entries that are composed from an .spm sprite frame
constexpr TCHAR composed_format[] = _T("SPM");

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

/// .spm sprite definition ("SPM VER-2.00")
///
/// Each frame is one complete sprite (e.g. body + expression) built from
/// parts that are placed relative to a common origin.
struct SpmPart
{
	u32 image = 0;
	s32 left = 0;
	s32 top = 0;
	s32 right = 0;
	s32 bottom = 0;
};

struct SpmFrame
{
	s32 width = 0;
	s32 height = 0;
	s32 left = 0;
	s32 top = 0;
	std::vector<SpmPart> parts;
};

struct Spm
{
	std::vector<SpmFrame> frames;
	std::vector<std::string> images;
	std::vector<std::string> labels; // Per frame, may be empty
};

class ByteReader
{
public:
	ByteReader(const std::vector<u8>& data, size_t pos) : m_data{data}, m_pos{pos} {}

	bool ReadS32(s32* out)
	{
		if (m_data.size() - m_pos < sizeof(s32))
			return false;

		std::memcpy(out, &m_data[m_pos], sizeof(s32));
		m_pos += sizeof(s32);
		return true;
	}

	bool ReadCount(size_t element_size, u32* out)
	{
		s32 count;
		if (!ReadS32(&count) || count < 0)
			return false;

		// Reject counts that could never fit in the remaining data
		if (static_cast<u64>(count) * element_size > m_data.size() - m_pos)
			return false;

		*out = static_cast<u32>(count);
		return true;
	}

	bool ReadString(std::string* out)
	{
		const auto begin = m_data.begin() + m_pos;
		const auto end = std::find(begin, m_data.end(), u8{0});
		if (end == m_data.end())
			return false;

		out->assign(begin, end);
		m_pos = static_cast<size_t>(end - m_data.begin()) + 1;
		return true;
	}

private:
	const std::vector<u8>& m_data;
	size_t m_pos;
};

bool ParseSpm(const std::vector<u8>& data, Spm* spm)
{
	static constexpr char signature[] = "SPM VER-2.00";
	if (data.size() < sizeof(signature) || std::memcmp(data.data(), signature, sizeof(signature)) != 0)
		return false;

	ByteReader reader(data, sizeof(signature));

	u32 num_frames;
	if (!reader.ReadCount(11 * sizeof(s32), &num_frames) || num_frames == 0)
		return false;

	spm->frames.resize(num_frames);
	for (auto& frame : spm->frames)
	{
		std::array<s32, 11> header;
		for (auto& value : header)
		{
			if (!reader.ReadS32(&value))
				return false;
		}

		const s32 num_parts = header[0];
		frame.width = header[1];
		frame.height = header[2];
		frame.left = header[3];
		frame.top = header[4];

		if (num_parts < 1 || num_parts > 16 || frame.width <= 0 || frame.height <= 0 || frame.width > 16384 || frame.height > 16384)
			return false;

		frame.parts.resize(num_parts);
		for (auto& part : frame.parts)
		{
			std::array<s32, 14> fields;
			for (auto& value : fields)
			{
				if (!reader.ReadS32(&value))
					return false;
			}

			if (fields[0] < 0)
				return false;

			part.image = static_cast<u32>(fields[0]);
			part.left = fields[1];
			part.top = fields[2];
			part.right = fields[3];
			part.bottom = fields[4];
		}
	}

	u32 num_images;
	if (!reader.ReadCount(1, &num_images))
		return false;

	spm->images.resize(num_images);
	for (auto& image : spm->images)
	{
		if (!reader.ReadString(&image))
			return false;
	}

	for (const auto& frame : spm->frames)
	{
		for (const auto& part : frame.parts)
		{
			if (part.image >= num_images)
				return false;
		}
	}

	// Labels (expression names) are optional; a damaged table only loses the names.
	spm->labels.assign(num_frames, std::string());

	u32 num_groups;
	if (!reader.ReadCount(sizeof(s32), &num_groups))
		return true;

	for (u32 group = 0; group < num_groups; group++)
	{
		u32 num_labels;
		if (!reader.ReadCount(1, &num_labels))
			return true;

		for (u32 i = 0; i < num_labels; i++)
		{
			std::string label;
			u32 num_refs;
			if (!reader.ReadString(&label) || !reader.ReadCount(4 * sizeof(s32), &num_refs))
				return true;

			// Each reference ends with the index of the frame it names
			s32 frame_index = -1;
			for (u32 ref = 0; ref < num_refs; ref++)
			{
				std::array<s32, 4> fields;
				for (auto& value : fields)
					reader.ReadS32(&value);

				frame_index = fields[3];
			}

			if (frame_index >= 0 && static_cast<u32>(frame_index) < num_frames)
				spm->labels[frame_index] = label;
		}
	}

	return true;
}

bool ReadData(CArcFile* archive, u64 start, u32 packed_size, u32 original_size, bool compressed, std::vector<u8>* out)
{
	std::vector<u8> packed(packed_size);
	archive->SeekHed(start);
	if (archive->Read(packed.data(), packed_size) != packed_size)
		return false;

	if (!compressed)
	{
		*out = std::move(packed);
		return true;
	}

	out->resize(original_size);
	CZlib zlib;
	return zlib.Decompress(out->data(), original_size, packed.data(), packed_size) == Z_OK;
}

bool ReadEntry(CArcFile* archive, const SFileInfo& file_info, std::vector<u8>* out)
{
	return ReadData(archive, file_info.start, file_info.size_cmp, file_info.size_org, file_info.format == _T("zlib"), out);
}

bool ReadPngSize(CArcFile* archive, const SFileInfo& file_info, u32* width, u32* height)
{
	std::vector<u8> data;
	if (file_info.format == _T("zlib"))
	{
		if (!ReadEntry(archive, file_info, &data))
			return false;
	}
	else
	{
		// Only the IHDR chunk is needed
		if (file_info.size_cmp < 24 || !ReadData(archive, file_info.start, 24, 24, false, &data))
			return false;
	}

	if (data.size() < 24 || std::memcmp(data.data(), "\x89PNG", 4) != 0 || std::memcmp(&data[12], "IHDR", 4) != 0)
		return false;

	*width = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
	*height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
	return true;
}

std::string ToLower(std::string str)
{
	for (auto& c : str)
	{
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}

	return str;
}

bool EndsWith(const std::string& str, const std::string& suffix)
{
	return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HasNonAscii(const std::string& str)
{
	return std::any_of(str.begin(), str.end(), [](char c) { return static_cast<u8>(c) >= 0x80; });
}

// Replaces characters that are not allowed in file names, without
// breaking up Shift-JIS double-byte characters (whose trail byte may be '\').
std::string SanitizeFileName(const std::string& str)
{
	static constexpr char invalid_chars[] = "\\/:*?\"<>|";

	std::string result;
	for (size_t i = 0; i < str.size(); i++)
	{
		const char c = str[i];
		if (IsDBCSLeadByteEx(932, static_cast<BYTE>(c)) && i + 1 < str.size())
		{
			result += c;
			result += str[++i];
		}
		else if (static_cast<u8>(c) < 0x20 || std::strchr(invalid_chars, c) != nullptr)
		{
			result += '_';
		}
		else
		{
			result += c;
		}
	}

	return result;
}

// Part image names of a frame, ignoring the trailing '_' of the "_" variant
// sprites so that they can be matched against their plain counterparts.
std::vector<std::string> VariantKey(const Spm& spm, const SpmFrame& frame)
{
	std::vector<std::string> key;
	for (const auto& part : frame.parts)
	{
		std::string name = ToLower(spm.images[part.image]);
		if (EndsWith(name, "_.png"))
			name.erase(name.size() - 5, 1);

		key.push_back(std::move(name));
	}

	return key;
}

/// Replaces the sprite parts listed in `file_infos` with one entry per .spm frame.
///
/// @param hidden   Set to true for every entry that is consumed by a composed entry
/// @param composed Receives the composed entries
void ComposeSprites(CArcFile* archive, const std::vector<SFileInfo>& file_infos, std::vector<bool>* hidden, std::vector<SFileInfo>* composed)
{
	struct ParsedSpm
	{
		size_t entry;
		Spm spm;
	};

	std::unordered_map<std::string, size_t> entries_by_name;
	std::map<std::string, ParsedSpm> spms;

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		const std::string name = ToLower(file_infos[i].name.GetString());
		entries_by_name.emplace(name, i);

		if (!EndsWith(name, ".spm"))
			continue;

		std::vector<u8> data;
		ParsedSpm parsed{i};
		if (ReadEntry(archive, file_infos[i], &data) && ParseSpm(data, &parsed.spm))
			spms.emplace(name, std::move(parsed));
	}

	std::unordered_map<size_t, std::pair<u32, u32>> png_sizes;
	std::set<std::string> used_names;

	for (const auto& [spm_name, parsed] : spms)
	{
		const Spm& spm = parsed.spm;

		// "xxx_.spm" only carries file names as labels; the expression
		// names are taken from the matching frame of "xxx.spm".
		std::map<std::vector<std::string>, std::string> partner_labels;
		if (EndsWith(spm_name, "_.spm"))
		{
			const std::string partner_name = spm_name.substr(0, spm_name.size() - 5) + ".spm";
			const auto partner = spms.find(partner_name);
			if (partner != spms.end())
			{
				const Spm& partner_spm = partner->second.spm;
				for (size_t f = 0; f < partner_spm.frames.size(); f++)
					partner_labels.emplace(VariantKey(partner_spm, partner_spm.frames[f]), partner_spm.labels[f]);
			}
		}

		// Sprite definitions are never useful on their own, even when all of their frames are broken
		(*hidden)[parsed.entry] = true;

		for (size_t f = 0; f < spm.frames.size(); f++)
		{
			const SpmFrame& frame = spm.frames[f];

			// Resolve and validate every part. Some sprites place an overlay using the
			// rectangle of the whole body; those frames are broken and are skipped.
			std::vector<size_t> part_entries;
			for (const auto& part : frame.parts)
			{
				const auto found = entries_by_name.find(ToLower(spm.images[part.image]));
				if (found == entries_by_name.end())
					break;

				auto size = png_sizes.find(found->second);
				if (size == png_sizes.end())
				{
					u32 width = 0;
					u32 height = 0;
					ReadPngSize(archive, file_infos[found->second], &width, &height);
					size = png_sizes.emplace(found->second, std::make_pair(width, height)).first;
				}

				const s64 x = static_cast<s64>(part.left) - frame.left;
				const s64 y = static_cast<s64>(part.top) - frame.top;
				const s64 width = static_cast<s64>(part.right) - part.left;
				const s64 height = static_cast<s64>(part.bottom) - part.top;

				if (width != size->second.first || height != size->second.second ||
				    x < 0 || y < 0 || x + width > frame.width || y + height > frame.height)
				{
					break;
				}

				part_entries.push_back(found->second);
			}

			if (part_entries.size() != frame.parts.size())
				continue;

			std::string label = spm.labels[f];
			if (!HasNonAscii(label))
			{
				const auto partner = partner_labels.find(VariantKey(spm, frame));
				label = partner != partner_labels.end() ? partner->second : std::string();
			}

			// A label without any Japanese is just a part name, which the file name already has
			if (!HasNonAscii(label))
				label.clear();

			// Name the image after its topmost part plus the expression name,
			// e.g. "miko_0202dy2_<label>_B.png" for the "_" variant
			const SFileInfo& top_part = file_infos[part_entries.back()];
			std::string stem = top_part.name.GetString();
			stem = stem.substr(0, stem.size() - std::strlen(PathFindExtension(stem.c_str())));

			const bool is_variant = EndsWith(stem, "_");
			if (is_variant)
				stem.pop_back();

			if (!label.empty())
				stem += "_" + SanitizeFileName(label);

			if (is_variant)
				stem += "_B";

			std::string name = stem + ".png";
			for (int n = 2; !used_names.insert(ToLower(name)).second; n++)
				name = stem + "_" + std::to_string(n) + ".png";

			const SFileInfo& spm_entry = file_infos[parsed.entry];

			SFileInfo file_info;
			file_info.name = name.c_str();
			file_info.format = composed_format;
			file_info.start = spm_entry.start;
			file_info.end = spm_entry.end;
			file_info.size_org = static_cast<u32>(frame.width) * static_cast<u32>(frame.height) * 4;
			// Only count the topmost part so that the mount progress does not exceed the archive size
			file_info.size_cmp = top_part.size_cmp;
			file_info.key = static_cast<u32>(f);

			const auto add_source = [&file_info, &file_infos](size_t entry) {
				file_info.starts.push_back(static_cast<u32>(file_infos[entry].start));
				file_info.sizes_cmp.push_back(file_infos[entry].size_cmp);
				file_info.sizes_org.push_back(file_infos[entry].size_org);
				file_info.compress_checks.push_back(file_infos[entry].format == _T("zlib"));
			};

			// [0] is the .spm itself, followed by the parts in drawing order
			add_source(parsed.entry);
			for (const size_t entry : part_entries)
			{
				add_source(entry);
				(*hidden)[entry] = true;
			}

			composed->push_back(std::move(file_info));
		}
	}
}

bool DecodePng(const std::vector<u8>& data, u32* width, u32* height, std::vector<u8>* bgra)
{
	png_image image{};
	image.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_memory(&image, data.data(), data.size()))
		return false;

	image.format = PNG_FORMAT_BGRA;
	bgra->resize(PNG_IMAGE_SIZE(image));

	if (!png_image_finish_read(&image, nullptr, bgra->data(), 0, nullptr))
	{
		png_image_free(&image);
		return false;
	}

	*width = image.width;
	*height = image.height;
	return true;
}

/// Draws `src` over `dst` (both straight-alpha BGRA) at (x, y).
void BlendOver(std::vector<u8>& dst, u32 dst_width, const std::vector<u8>& src, u32 src_width, u32 src_height, u32 x, u32 y)
{
	for (u32 row = 0; row < src_height; row++)
	{
		const u8* s = &src[static_cast<size_t>(row) * src_width * 4];
		u8* d = &dst[(static_cast<size_t>(y + row) * dst_width + x) * 4];

		for (u32 col = 0; col < src_width; col++, s += 4, d += 4)
		{
			const u32 src_alpha = s[3];
			const u32 dst_alpha = d[3];

			if (src_alpha == 0)
				continue;

			if (src_alpha == 255 || dst_alpha == 0)
			{
				std::memcpy(d, s, 4);
				continue;
			}

			// Weights scaled by 255 * 255
			const u32 src_weight = src_alpha * 255;
			const u32 dst_weight = dst_alpha * (255 - src_alpha);
			const u32 out_weight = src_weight + dst_weight;

			for (int c = 0; c < 3; c++)
				d[c] = static_cast<u8>((s[c] * src_weight + d[c] * dst_weight + out_weight / 2) / out_weight);

			d[3] = static_cast<u8>((out_weight + 127) / 255);
		}
	}
}
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

	std::vector<bool> hidden(num_files);
	std::vector<SFileInfo> composed;
	ComposeSprites(archive, file_infos, &hidden, &composed);

	for (u32 i = 0; i < num_files; i++)
	{
		if (!hidden[i])
			archive->AddFileInfo(file_infos[i]);
	}

	for (auto& file_info : composed)
		archive->AddFileInfo(file_info);

	return true;
}

/// Decoding
///
/// Composes one .spm frame and writes it as an image.
bool CNexas::Decode(CArcFile* archive)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();

	if (file_info->format != composed_format || file_info->starts.size() < 2)
		return false;

	if (lstrcmpi(archive->GetArcExten(), _T(".pac")) != 0 || std::memcmp(archive->GetHeader(), "PAC", 3) != 0)
		return false;

	const auto fail = [archive, file_info] {
		CError error;
		error.Message(archive->GetProg()->GetHandle(), _T("Failed to compose %s"), file_info->name.GetString());
		return true;
	};

	std::vector<u8> spm_data;
	Spm spm;
	if (!ReadData(archive, file_info->starts[0], file_info->sizes_cmp[0], file_info->sizes_org[0], file_info->compress_checks[0] != 0, &spm_data) ||
	    !ParseSpm(spm_data, &spm) || file_info->key >= spm.frames.size())
	{
		return fail();
	}

	const SpmFrame& frame = spm.frames[file_info->key];
	if (frame.parts.size() != file_info->starts.size() - 1)
		return fail();

	const u32 width = static_cast<u32>(frame.width);
	const u32 height = static_cast<u32>(frame.height);
	std::vector<u8> canvas(static_cast<size_t>(width) * height * 4);

	for (size_t i = 0; i < frame.parts.size(); i++)
	{
		const SpmPart& part = frame.parts[i];

		Bitmap scratch;
		const Bitmap* bitmap = DecodePart(archive, i + 1, &scratch);
		if (bitmap == nullptr)
			return fail();

		const s64 x = static_cast<s64>(part.left) - frame.left;
		const s64 y = static_cast<s64>(part.top) - frame.top;
		if (x < 0 || y < 0 || x + bitmap->width > width || y + bitmap->height > height)
			return fail();

		BlendOver(canvas, width, bitmap->pixels, bitmap->width, bitmap->height, static_cast<u32>(x), static_cast<u32>(y));
	}

	CImage image;
	image.Init(archive, static_cast<s32>(width), static_cast<s32>(height), 32);
	image.WriteReverse(canvas.data(), canvas.size());
	image.Close();

	return true;
}

/// Extraction
///
/// Composed entries have no original file, so they are always composed.
bool CNexas::Extract(CArcFile* archive)
{
	return Decode(archive);
}

const CNexas::Bitmap* CNexas::DecodePart(CArcFile* archive, size_t part, Bitmap* scratch)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();
	const u64 offset = file_info->starts[part];

	// Only the body (the first part) is shared between frames
	const bool cacheable = part == 1;
	if (cacheable && m_cached_offset == offset && m_cached_archive_path == archive->GetArcPath() && !m_cached_part.pixels.empty())
		return &m_cached_part;

	std::vector<u8> data;
	Bitmap* bitmap = cacheable ? &m_cached_part : scratch;
	bitmap->pixels.clear();

	if (!ReadData(archive, offset, file_info->sizes_cmp[part], file_info->sizes_org[part], file_info->compress_checks[part] != 0, &data) ||
	    !DecodePng(data, &bitmap->width, &bitmap->height, &bitmap->pixels))
	{
		bitmap->pixels.clear();
		return nullptr;
	}

	if (cacheable)
	{
		m_cached_archive_path = archive->GetArcPath();
		m_cached_offset = offset;
	}

	return bitmap;
}
