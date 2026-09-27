#include "StdAfx.h"
#include "Extract/Nexas.h"

#include "Arc/Zlib.h"
#include "ArcFile.h"
#include "Common.h"
#include "Error.h"
#include "Image.h"
#include "UI/ProgressBar.h"
#include "Utils/ImageUtils.h"

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

// SFileInfo::format of event CG differences that are composed onto their base image
constexpr TCHAR visual_format[] = _T("VISUAL");

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

	bool AtEnd() const { return m_pos == m_data.size(); }

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

// `Reader` is CArcFile or YCFile (for reading a sibling archive)
template <typename Reader>
bool ReadData(Reader* reader, u64 start, u32 packed_size, u32 original_size, bool compressed, std::vector<u8>* out)
{
	std::vector<u8> packed(packed_size);
	reader->SeekHed(start);
	if (reader->Read(packed.data(), packed_size) != packed_size)
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

template <typename Reader>
bool ReadEntry(Reader* reader, const SFileInfo& file_info, std::vector<u8>* out)
{
	return ReadData(reader, file_info.start, file_info.size_cmp, file_info.size_org, file_info.format == _T("zlib"), out);
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

bool ReportComposeFailure(CArcFile* archive)
{
	CError error;
	error.Message(archive->GetProg()->GetHandle(), _T("Failed to compose %s"), archive->GetOpenFileInfo()->name.GetString());

	// The entry has been handled; falling back to the standard decoders would write garbage
	return true;
}

/// Reads and validates the index of a NeXAS .pac archive.
///
/// @param reader       CArcFile or YCFile
/// @param archive_size Size of the archive in bytes
///
template <typename Reader>
bool ReadPacIndex(Reader* reader, u64 archive_size, std::vector<SFileInfo>* file_infos)
{
	if (archive_size < header_size + 4)
		return false;

	std::array<u8, header_size> header;
	reader->SeekHed();
	if (reader->Read(header.data(), header_size) != header_size || std::memcmp(header.data(), "PAC", 3) != 0)
		return false;

	u32 num_files;
	u32 pack_method;
	std::memcpy(&num_files, &header[4], sizeof(u32));
	std::memcpy(&pack_method, &header[8], sizeof(u32));

	if (num_files == 0 || (pack_method != PACK_NONE && pack_method != PACK_ZLIB))
		return false;

	// The last four bytes hold the size of the compressed index that precedes them
	u32 index_size;
	reader->SeekEnd(4);
	if (reader->Read(&index_size, sizeof(u32)) != sizeof(u32))
		return false;

	if (index_size == 0 || index_size > archive_size - header_size - 4)
		return false;

	std::vector<u8> packed_index(index_size);
	reader->SeekEnd(4 + static_cast<s64>(index_size));
	if (reader->Read(packed_index.data(), index_size) != index_size)
		return false;

	for (auto& byte : packed_index)
		byte ^= 0xFF;

	std::vector<u8> index(static_cast<size_t>(num_files) * entry_size);
	HuffmanDecoder decoder(packed_index);
	if (!decoder.Decode(index.data(), index.size()))
		return false;

	// Validate every entry before returning anything, so that other
	// "PAC" formats (e.g. CBaldr) still get a chance to mount.
	file_infos->assign(num_files, SFileInfo());
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
			return false;

		SFileInfo& file_info = (*file_infos)[i];
		file_info.name = name;
		file_info.start = offset;
		file_info.end = file_info.start + packed_size;
		file_info.size_cmp = packed_size;
		file_info.size_org = original_size;

		// Entries that were not worth compressing are stored as-is
		if (pack_method == PACK_ZLIB && packed_size != original_size)
			file_info.format = _T("zlib");
	}

	return true;
}

/// One row of the event CG table (e.g. visual.dat in Config.pac)
struct VisualRow
{
	std::string base;
	std::string diff; // Empty when the row shows the base image alone
	s32 x = 0;
	s32 y = 0;
};

bool ParseVisualTable(const std::vector<u8>& data, std::vector<VisualRow>* rows)
{
	// Column types: 1 = string, 2 = integer
	static constexpr std::array<s32, 14> expected_types{{2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2}};

	ByteReader reader(data, 0);

	s32 num_columns;
	if (!reader.ReadS32(&num_columns) || num_columns != static_cast<s32>(expected_types.size()))
		return false;

	for (const s32 expected : expected_types)
	{
		s32 type;
		if (!reader.ReadS32(&type) || type != expected)
			return false;
	}

	while (!reader.AtEnd())
	{
		// Colour/brightness parameters, base, diff, diff position, canvas size
		VisualRow row;
		s32 ignored;
		for (int i = 0; i < 8; i++)
		{
			if (!reader.ReadS32(&ignored))
				return false;
		}

		s32 width;
		s32 height;
		if (!reader.ReadString(&row.base) || !reader.ReadString(&row.diff) ||
		    !reader.ReadS32(&row.x) || !reader.ReadS32(&row.y) || !reader.ReadS32(&width) || !reader.ReadS32(&height))
		{
			return false;
		}

		rows->push_back(std::move(row));
	}

	return true;
}

/// Turns the event CG differences listed in `file_infos` into complete images.
///
/// The positions of the differences are not stored in the archive itself but in
/// "<archive name>.dat" inside Config.pac, which lives in the same folder.
void ComposeVisuals(CArcFile* archive, std::vector<SFileInfo>* file_infos)
{
	TCHAR table_name[MAX_PATH];
	lstrcpyn(table_name, PathFindFileName(archive->GetArcPath()), MAX_PATH - 4);
	PathRemoveExtension(table_name);
	lstrcat(table_name, _T(".dat"));

	TCHAR config_path[MAX_PATH];
	lstrcpyn(config_path, archive->GetArcPath(), MAX_PATH);
	PathRemoveFileSpec(config_path);
	if (!PathAppend(config_path, _T("Config.pac")))
		return;

	YCFile config;
	if (!config.Open(config_path, YCFile::modeRead | YCFile::shareDenyNone))
		return;

	std::vector<SFileInfo> config_infos;
	if (!ReadPacIndex(&config, config.GetLength(), &config_infos))
		return;

	const std::string lower_table_name = ToLower(table_name);
	const auto table = std::find_if(config_infos.begin(), config_infos.end(), [&lower_table_name](const SFileInfo& file_info) {
		return ToLower(file_info.name.GetString()) == lower_table_name;
	});
	if (table == config_infos.end())
		return;

	std::vector<u8> table_data;
	std::vector<VisualRow> rows;
	if (!ReadEntry(&config, *table, &table_data) || !ParseVisualTable(table_data, &rows))
		return;

	std::unordered_map<std::string, size_t> entries_by_name;
	for (size_t i = 0; i < file_infos->size(); i++)
		entries_by_name.emplace(ToLower((*file_infos)[i].name.GetString()), i);

	for (const auto& row : rows)
	{
		if (row.diff.empty() || row.x < 0 || row.y < 0)
			continue;

		const auto base = entries_by_name.find(ToLower(row.base));
		const auto diff = entries_by_name.find(ToLower(row.diff));
		if (base == entries_by_name.end() || diff == entries_by_name.end() || base->second == diff->second)
			continue;

		const SFileInfo& base_info = (*file_infos)[base->second];
		SFileInfo& diff_info = (*file_infos)[diff->second];
		if (base_info.format == visual_format || diff_info.format == visual_format)
			continue;

		// [0] is the base image and [1] the difference; the entry keeps its name
		for (const SFileInfo* source : {&base_info, static_cast<const SFileInfo*>(&diff_info)})
		{
			diff_info.starts.push_back(static_cast<u32>(source->start));
			diff_info.sizes_cmp.push_back(source->size_cmp);
			diff_info.sizes_org.push_back(source->size_org);
			diff_info.compress_checks.push_back(source->format == _T("zlib"));
		}

		diff_info.format = visual_format;
		diff_info.size_org = base_info.size_org;
		diff_info.key = static_cast<u32>(row.x);
		diff_info.type = static_cast<u32>(row.y);
	}
}

/// View of an uncompressed 24-bit BMP file
struct Bmp24
{
	u32 width = 0;
	u32 height = 0;
	bool bottom_up = true;
	size_t pitch = 0;
	const u8* pixels = nullptr;

	// Row `y` counted from the top
	const u8* Row(u32 y) const { return pixels + (bottom_up ? height - 1 - y : y) * pitch; }
};

bool ParseBmp24(const std::vector<u8>& data, Bmp24* bmp)
{
	if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
		return false;

	u32 pixel_offset;
	s32 width;
	s32 height;
	u16 bpp;
	u32 compression;
	std::memcpy(&pixel_offset, &data[10], sizeof(u32));
	std::memcpy(&width, &data[18], sizeof(s32));
	std::memcpy(&height, &data[22], sizeof(s32));
	std::memcpy(&bpp, &data[28], sizeof(u16));
	std::memcpy(&compression, &data[30], sizeof(u32));

	if (bpp != 24 || compression != BI_RGB || width <= 0 || height == 0 || width > 16384 || height > 16384 || height < -16384)
		return false;

	bmp->width = static_cast<u32>(width);
	bmp->height = static_cast<u32>(height < 0 ? -height : height);
	bmp->bottom_up = height > 0;
	bmp->pitch = (static_cast<size_t>(bmp->width) * 3 + 3) & ~static_cast<size_t>(3);

	if (pixel_offset > data.size() || data.size() - pixel_offset < bmp->pitch * bmp->height)
		return false;

	bmp->pixels = &data[pixel_offset];
	return true;
}
} // Anonymous namespace

/// Mounting
bool CNexas::Mount(CArcFile* archive)
{
	if (lstrcmpi(archive->GetArcExten(), _T(".pac")) != 0)
		return false;

	if (std::memcmp(archive->GetHeader(), "PAC", 3) != 0)
		return false;

	std::vector<SFileInfo> file_infos;
	if (!ReadPacIndex(archive, archive->GetArcSize(), &file_infos))
	{
		archive->SeekHed();
		return false;
	}

	std::vector<bool> hidden(file_infos.size());
	std::vector<SFileInfo> composed;
	ComposeSprites(archive, file_infos, &hidden, &composed);
	ComposeVisuals(archive, &file_infos);

	for (size_t i = 0; i < file_infos.size(); i++)
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
/// Composes sprite frames and event CG differences; other entries are left to the standard decoders.
bool CNexas::Decode(CArcFile* archive)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();

	if (file_info->starts.size() < 2)
		return false;

	if (lstrcmpi(archive->GetArcExten(), _T(".pac")) != 0 || std::memcmp(archive->GetHeader(), "PAC", 3) != 0)
		return false;

	if (file_info->format == composed_format)
		return DecodeSprite(archive);

	if (file_info->format == visual_format)
		return DecodeVisual(archive);

	return false;
}

/// Extraction
///
/// Composed entries have no original file, so they are always composed.
bool CNexas::Extract(CArcFile* archive)
{
	return Decode(archive);
}

/// Composes one .spm frame and writes it as an image.
bool CNexas::DecodeSprite(CArcFile* archive)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();
	const auto fail = [archive] { return ReportComposeFailure(archive); };

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

		ImageUtils::BlendOver(canvas, width, bitmap->pixels, bitmap->width, bitmap->height, static_cast<u32>(x), static_cast<u32>(y));
	}

	CImage image;
	image.Init(archive, static_cast<s32>(width), static_cast<s32>(height), 32);
	image.WriteReverse(canvas.data(), canvas.size());
	image.Close();

	return true;
}

/// Draws an event CG difference onto its base image and writes the result.
///
/// Black pixels of the difference are transparent.
bool CNexas::DecodeVisual(CArcFile* archive)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();
	const auto fail = [archive] { return ReportComposeFailure(archive); };

	// Many differences share one base image
	const u64 base_offset = file_info->starts[0];
	if (m_cached_base.empty() || m_cached_base_offset != base_offset || m_cached_base_path != archive->GetArcPath())
	{
		m_cached_base.clear();
		if (!ReadData(archive, base_offset, file_info->sizes_cmp[0], file_info->sizes_org[0], file_info->compress_checks[0] != 0, &m_cached_base))
		{
			m_cached_base.clear();
			return fail();
		}

		m_cached_base_path = archive->GetArcPath();
		m_cached_base_offset = base_offset;
	}

	std::vector<u8> diff_data;
	if (!ReadData(archive, file_info->starts[1], file_info->sizes_cmp[1], file_info->sizes_org[1], file_info->compress_checks[1] != 0, &diff_data))
		return fail();

	Bmp24 base;
	Bmp24 diff;
	if (!ParseBmp24(m_cached_base, &base) || !ParseBmp24(diff_data, &diff))
		return fail();

	const u32 x = file_info->key;
	const u32 y = file_info->type;
	if (static_cast<u64>(x) + diff.width > base.width || static_cast<u64>(y) + diff.height > base.height)
		return fail();

	// Bottom-up rows with BMP padding, as CImage::Write() expects
	std::vector<u8> canvas(base.pitch * base.height);
	const auto canvas_row = [&canvas, &base](u32 row) { return &canvas[(base.height - 1 - row) * base.pitch]; };

	for (u32 row = 0; row < base.height; row++)
		std::memcpy(canvas_row(row), base.Row(row), base.pitch);

	for (u32 row = 0; row < diff.height; row++)
	{
		const u8* src = diff.Row(row);
		u8* dst = canvas_row(y + row) + x * 3;

		for (u32 col = 0; col < diff.width; col++, src += 3, dst += 3)
		{
			if (src[0] != 0 || src[1] != 0 || src[2] != 0)
				std::memcpy(dst, src, 3);
		}
	}

	CImage image;
	image.Init(archive, static_cast<s32>(base.width), static_cast<s32>(base.height), 24);
	image.Write(canvas.data(), canvas.size());
	image.Close();

	return true;
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
	    !ImageUtils::DecodePng(data.data(), data.size(), &bitmap->width, &bitmap->height, &bitmap->pixels))
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
