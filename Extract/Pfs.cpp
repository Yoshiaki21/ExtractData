#include "StdAfx.h"
#include "Extract/Pfs.h"

#include "ArcFile.h"
#include "Common.h"
#include "Error.h"
#include "Image.h"
#include "UI/ProgressBar.h"
#include "Utils/ImageUtils.h"

namespace
{
// "pf8", u32 index size, then the index itself (starting with the file count)
constexpr u64 index_offset = 7;

// SFileInfo::format of composed standing sprites
constexpr TCHAR sprite_format[] = _T("FG");

// Sprites exist in three sizes that differ only in resolution; only the largest is composed
constexpr char sprite_size[] = "z2";

// Folders whose sprites are composed, and the size that their body names carry.
// "fa" holds the message window faces, bust-up crops of the "no" bodies with their own faces.
struct SpriteFolder
{
	const char* folder;
	const char* body_size;
};

constexpr SpriteFolder sprite_folders[] = {
	{sprite_size, sprite_size},
	{"fa", "no"},
};

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

/// The game data is split into "xxx.pfs" (main) and "xxx.pfs.000", "xxx.pfs.001", ...
///
/// @param is_main Set to true for "xxx.pfs"
bool IsPfsPath(LPCTSTR path, bool* is_main)
{
	const std::string lower = ToLower(path);
	if (EndsWith(lower, ".pfs"))
	{
		*is_main = true;
		return true;
	}

	const size_t dot = lower.rfind('.');
	if (dot == std::string::npos || dot + 1 == lower.size() ||
	    !std::all_of(lower.begin() + dot + 1, lower.end(), [](char c) { return c >= '0' && c <= '9'; }))
	{
		return false;
	}

	*is_main = false;
	return EndsWith(lower.substr(0, dot), ".pfs");
}

bool Sha1(const u8* data, size_t size, CPfs::Key* out)
{
	HCRYPTPROV provider;
	if (!CryptAcquireContext(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
		return false;

	bool result = false;
	HCRYPTHASH hash;
	if (CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash))
	{
		DWORD hash_size = CPfs::key_size;
		result = CryptHashData(hash, data, static_cast<DWORD>(size), 0) &&
		         CryptGetHashParam(hash, HP_HASHVAL, out->data(), &hash_size, 0) &&
		         hash_size == CPfs::key_size;

		CryptDestroyHash(hash);
	}

	CryptReleaseContext(provider, 0);
	return result;
}

bool ReadIndex(CArcFile* archive, std::vector<u8>* index)
{
	char signature[3];
	u32 index_size;
	archive->SeekHed();
	if (archive->Read(signature, sizeof(signature)) != sizeof(signature) || std::memcmp(signature, "pf8", 3) != 0 ||
	    !archive->ReadU32(&index_size))
	{
		return false;
	}

	if (index_size < sizeof(u32) || index_size > archive->GetArcSize() - index_offset)
		return false;

	index->resize(index_size);
	archive->SeekHed(index_offset);
	return archive->Read(index->data(), index_size) == index_size;
}

// Index names are UTF-8, while this application works in the ANSI code page
std::string Utf8ToAnsi(const std::string& utf8)
{
	const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (wide_length <= 0)
		return utf8;

	std::wstring wide(wide_length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], wide_length);

	const int length = WideCharToMultiByte(CP_ACP, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
	std::string result(length, '\0');
	WideCharToMultiByte(CP_ACP, 0, wide.data(), wide_length, &result[0], length, nullptr, nullptr);
	return result;
}

bool ParseIndex(const std::vector<u8>& index, u64 archive_size, std::vector<SFileInfo>* file_infos)
{
	size_t pos = 0;
	const auto read_u32 = [&index, &pos](u32* out) {
		if (index.size() - pos < sizeof(u32))
			return false;

		std::memcpy(out, &index[pos], sizeof(u32));
		pos += sizeof(u32);
		return true;
	};

	// Each entry takes at least 16 bytes
	u32 num_files;
	if (!read_u32(&num_files) || num_files == 0 || num_files > index.size() / 16)
		return false;

	const u64 data_start = index_offset + index.size();

	file_infos->resize(num_files);
	for (auto& file_info : *file_infos)
	{
		u32 name_length;
		if (!read_u32(&name_length) || name_length == 0 || name_length > index.size() - pos)
			return false;

		const std::string name(reinterpret_cast<const char*>(&index[pos]), name_length);
		pos += name_length;

		u32 reserved;
		u32 offset;
		u32 size;
		if (!read_u32(&reserved) || !read_u32(&offset) || !read_u32(&size))
			return false;

		if (offset < data_start || static_cast<u64>(offset) + size > archive_size)
			return false;

		file_info.name = Utf8ToAnsi(name).c_str();
		file_info.start = offset;
		file_info.end = file_info.start + size;
		file_info.size_cmp = size;
		file_info.size_org = size;
	}

	return true;
}

/// Reads an entry and removes the XOR encryption, which restarts at the beginning of every entry.
bool ReadDecrypted(CArcFile* archive, u64 start, u32 size, const CPfs::Key& key, std::vector<u8>* out)
{
	out->resize(size);
	archive->SeekHed(start);
	if (archive->Read(out->data(), size) != size)
		return false;

	for (size_t i = 0; i < out->size(); i++)
		(*out)[i] ^= key[i % CPfs::key_size];

	return true;
}

/// A value of a Lua table constructor
struct LuaValue
{
	enum class Type
	{
		Scalar, // String, number or name
		Table,
	};

	Type type = Type::Scalar;
	std::string scalar;
	std::vector<LuaValue> items;
	std::vector<std::pair<std::string, LuaValue>> fields;

	const LuaValue* Field(const char* name) const
	{
		for (const auto& field : fields)
		{
			if (field.first == name)
				return &field.second;
		}

		return nullptr;
	}

	std::vector<std::string> Scalars() const
	{
		std::vector<std::string> result;
		for (const auto& item : items)
		{
			if (item.type == Type::Scalar)
				result.push_back(item.scalar);
		}

		return result;
	}
};

/// Just enough of Lua's syntax to read the table constructors of the game's data files.
class LuaTableReader
{
public:
	explicit LuaTableReader(const std::string& text)
	{
		Tokenize(text);
	}

	/// Reads the value assigned by "<name> = ..."
	bool ReadAssignment(const char* name, LuaValue* out)
	{
		for (size_t i = 0; i + 2 < m_tokens.size(); i++)
		{
			if (m_tokens[i].kind == Token::Kind::Name && m_tokens[i].text == name && m_tokens[i + 1].IsSymbol('='))
			{
				m_pos = i + 2;
				return ReadValue(out, 0);
			}
		}

		return false;
	}

private:
	struct Token
	{
		enum class Kind
		{
			Name,
			String,
			Number,
			Symbol,
		};

		Kind kind;
		std::string text;

		bool IsSymbol(char c) const { return kind == Kind::Symbol && text[0] == c; }
	};

	static bool IsDigit(char c) { return c >= '0' && c <= '9'; }
	static bool IsNameStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
	static bool IsNameChar(char c) { return IsNameStart(c) || IsDigit(c); }

	void Tokenize(const std::string& text)
	{
		size_t i = 0;
		const size_t n = text.size();

		while (i < n)
		{
			const char c = text[i];

			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			{
				i++;
			}
			else if (c == '-' && i + 1 < n && text[i + 1] == '-')
			{
				// Comment, either "--[[ ... ]]" or up to the end of the line
				const bool is_block = text.compare(i + 2, 2, "[[") == 0;
				const size_t end = is_block ? text.find("]]", i + 4) : text.find('\n', i);
				i = end == std::string::npos ? n : end + (is_block ? 2 : 1);
			}
			else if (c == '"' || c == '\'')
			{
				std::string str;
				for (i++; i < n && text[i] != c; i++)
				{
					if (text[i] == '\\' && i + 1 < n)
						i++;

					str += text[i];
				}

				i++;
				m_tokens.push_back({Token::Kind::String, std::move(str)});
			}
			else if (IsDigit(c) || (c == '-' && i + 1 < n && IsDigit(text[i + 1])))
			{
				const size_t start = i++;
				while (i < n && (IsNameChar(text[i]) || text[i] == '.'))
					i++;

				m_tokens.push_back({Token::Kind::Number, text.substr(start, i - start)});
			}
			else if (IsNameStart(c))
			{
				const size_t start = i;
				while (i < n && IsNameChar(text[i]))
					i++;

				m_tokens.push_back({Token::Kind::Name, text.substr(start, i - start)});
			}
			else
			{
				m_tokens.push_back({Token::Kind::Symbol, std::string(1, c)});
				i++;
			}
		}
	}

	bool ReadValue(LuaValue* out, int depth)
	{
		if (depth > 32 || m_pos >= m_tokens.size())
			return false;

		const Token& token = m_tokens[m_pos++];
		if (token.kind != Token::Kind::Symbol)
		{
			out->type = LuaValue::Type::Scalar;
			out->scalar = token.text;
			return true;
		}

		if (!token.IsSymbol('{'))
			return false;

		out->type = LuaValue::Type::Table;
		while (m_pos < m_tokens.size())
		{
			if (m_tokens[m_pos].IsSymbol('}'))
			{
				m_pos++;
				return true;
			}

			LuaValue value;
			if (m_tokens[m_pos].kind == Token::Kind::Name && m_pos + 1 < m_tokens.size() && m_tokens[m_pos + 1].IsSymbol('='))
			{
				std::string key = m_tokens[m_pos].text;
				m_pos += 2;
				if (!ReadValue(&value, depth + 1))
					return false;

				out->fields.emplace_back(std::move(key), std::move(value));
			}
			else
			{
				if (!ReadValue(&value, depth + 1))
					return false;

				out->items.push_back(std::move(value));
			}

			if (m_pos < m_tokens.size() && (m_tokens[m_pos].IsSymbol(',') || m_tokens[m_pos].IsSymbol(';')))
				m_pos++;
		}

		return false;
	}

	std::vector<Token> m_tokens;
	size_t m_pos = 0;
};

/// Sprite parts carry their screen position as a "comment" text chunk: "pos,x,y,width,height"
bool ReadPngPosition(const std::vector<u8>& png, s32* x, s32* y)
{
	if (png.size() < 8 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0)
		return false;

	size_t pos = 8;
	while (png.size() - pos >= 12)
	{
		const u32 length = (png[pos] << 24) | (png[pos + 1] << 16) | (png[pos + 2] << 8) | png[pos + 3];
		const u8* type = &png[pos + 4];
		if (length > png.size() - pos - 12 || std::memcmp(type, "IDAT", 4) == 0)
			return false;

		if (std::memcmp(type, "tEXt", 4) == 0)
		{
			const std::string text(reinterpret_cast<const char*>(&png[pos + 8]), length);
			const size_t separator = text.find('\0');
			if (separator != std::string::npos && text.compare(0, separator, "comment") == 0 &&
			    std::sscanf(text.c_str() + separator + 1, "pos,%d,%d", x, y) == 2)
			{
				return true;
			}
		}

		pos += 12 + length;
	}

	return false;
}

/// Replaces the standing sprite parts listed in `file_infos` with one entry per body/face combination.
///
/// The combinations are taken from the extras table "exfgtable", which is stored in a file named
/// "exlist" whose folder and extension differ between games ("pc\ja\extra\exlist.ipt",
/// "system\table\exlist.tbl", ...).
///
/// @param keys         Decryption keys by split archive ID
/// @param parts        Split archive ID of each entry of `file_infos`
/// @param hidden       Set to true for every entry that is consumed by a composed entry
/// @param sprites      Receives the composed entries
/// @param sprite_parts Receives the split archive ID of each composed entry
void ComposeSprites(CArcFile* archive, const std::vector<CPfs::Key>& keys, const std::vector<SFileInfo>& file_infos, const std::vector<u32>& parts,
                    std::vector<bool>* hidden, std::vector<SFileInfo>* sprites, std::vector<u32>* sprite_parts)
{
	std::unordered_map<std::string, size_t> entries_by_name;
	std::vector<size_t> table_candidates;

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		const std::string name = ToLower(file_infos[i].name.GetString());
		entries_by_name.emplace(name, i);

		const size_t file_name_pos = name.find_last_of('\\') + 1; // 0 when there is no folder
		const size_t extension_pos = name.find_last_of('.');
		const size_t stem_end = extension_pos != std::string::npos && extension_pos > file_name_pos ? extension_pos : name.size();
		if (name.compare(file_name_pos, stem_end - file_name_pos, "exlist") == 0)
			table_candidates.push_back(i);
	}

	// Prefer the Japanese table when there are several languages
	std::stable_partition(table_candidates.begin(), table_candidates.end(), [&file_infos](size_t i) {
		return ToLower(file_infos[i].name.GetString()).find("\\ja\\") != std::string::npos;
	});

	// Use the first candidate that actually holds the sprite table
	LuaValue table;
	const LuaValue* fg = nullptr;
	const LuaValue* sizes = nullptr;
	for (const size_t candidate : table_candidates)
	{
		std::vector<u8> table_data;
		const u32 table_part = parts[candidate];
		archive->SetArcsID(table_part);
		if (!ReadDecrypted(archive, file_infos[candidate].start, file_infos[candidate].size_cmp, keys[table_part], &table_data))
			continue;

		table = LuaValue();
		LuaTableReader reader(std::string(table_data.begin(), table_data.end()));
		if (!reader.ReadAssignment("exfgtable", &table))
			continue;

		fg = table.Field("fg");
		sizes = fg != nullptr ? fg->Field("size") : nullptr;
		if (sizes != nullptr)
			break;
	}

	if (sizes == nullptr)
		return;

	const std::vector<std::string> size_names = sizes->Scalars();
	if (std::find(size_names.begin(), size_names.end(), sprite_size) == size_names.end())
		return;

	struct Combination
	{
		size_t body;
		size_t face;
		std::string name;
	};

	std::vector<Combination> combinations;
	std::unordered_map<size_t, u32> uses;

	const auto find_entry = [&entries_by_name](const std::string& name) {
		const auto found = entries_by_name.find(ToLower(name));
		return found != entries_by_name.end() ? found->second : SIZE_MAX;
	};

	// Each character: path = ":fg/ame/", head = "ame_", fuku = {"02", ...} (outfits),
	// pose = {{"a","0",n}, ...}, face = { a = {"a0001", ...}, ... }
	for (const auto& character : fg->items)
	{
		const LuaValue* path = character.Field("path");
		const LuaValue* head = character.Field("head");
		const LuaValue* outfits = character.Field("fuku");
		const LuaValue* poses = character.Field("pose");
		const LuaValue* faces = character.Field("face");
		if (path == nullptr || head == nullptr || outfits == nullptr || poses == nullptr || faces == nullptr)
			continue;

		// ":fg/ame/" -> "image\fg\ame\"
		std::string character_dir = path->scalar;
		if (!character_dir.empty() && character_dir[0] == ':')
			character_dir.erase(0, 1);

		character_dir = "image/" + character_dir;
		std::replace(character_dir.begin(), character_dir.end(), '/', '\\');

		for (const auto& sprite_folder : sprite_folders)
		{
			// e.g. "image\fg\ame\z2\"
			const std::string dir = character_dir + sprite_folder.folder + "\\";

			for (const auto& outfit : outfits->Scalars())
			{
				for (const auto& pose : poses->items)
				{
					const std::vector<std::string> pose_fields = pose.Scalars();
					if (pose_fields.size() < 2)
						continue;

					const std::string& letter = pose_fields[0];
					const std::string body_stem = head->scalar + sprite_folder.body_size + letter + outfit + pose_fields[1] + "0";

					const size_t body = find_entry(dir + body_stem + ".png");
					const LuaValue* pose_faces = faces->Field(letter.c_str());
					if (body == SIZE_MAX || pose_faces == nullptr)
						continue;

					for (const auto& face_id : pose_faces->Scalars())
					{
						// Both parts are read from the same file when composing
						const size_t face = find_entry(dir + face_id + ".png");
						if (face == SIZE_MAX || parts[face] != parts[body])
							continue;

						combinations.push_back({body, face, dir + body_stem + "_" + face_id + ".png"});
						uses[body]++;
						uses[face]++;
					}
				}
			}
		}
	}

	std::set<std::string> used_names;
	for (const auto& combination : combinations)
	{
		if (!used_names.insert(ToLower(combination.name)).second)
			continue;

		const SFileInfo& body = file_infos[combination.body];
		const SFileInfo& face = file_infos[combination.face];

		SFileInfo file_info;
		file_info.name = combination.name.c_str();
		file_info.format = sprite_format;
		file_info.start = body.start;
		file_info.end = body.end;
		file_info.size_org = body.size_org;
		// Parts are shared by many combinations; spread their sizes so that the mount progress adds up
		file_info.size_cmp = body.size_cmp / uses[combination.body] + face.size_cmp / uses[combination.face];

		// [0] is the body and [1] the face
		for (const SFileInfo* part : {&body, &face})
		{
			file_info.starts.push_back(static_cast<u32>(part->start));
			file_info.sizes_cmp.push_back(part->size_cmp);
			file_info.sizes_org.push_back(part->size_org);
		}

		sprites->push_back(std::move(file_info));
		sprite_parts->push_back(parts[combination.body]);
		(*hidden)[combination.body] = true;
		(*hidden)[combination.face] = true;
	}
}

/// Hides the "no" and "z1" standing sprite parts, which are the "z2" parts at lower resolutions.
///
/// A part is only hidden when its "z2" counterpart exists, so no picture is lost. Other folders
/// such as "fa" (message window faces) are different pictures and are kept.
void HideLowResolutionSprites(const std::vector<SFileInfo>& file_infos, std::vector<bool>* hidden)
{
	static const std::string root = "image\\fg\\";

	std::unordered_set<std::string> names;
	for (const auto& file_info : file_infos)
		names.insert(ToLower(file_info.name.GetString()));

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		// "image\fg\<character>\<size>\<file>"
		const std::string name = ToLower(file_infos[i].name.GetString());
		if (name.compare(0, root.size(), root) != 0)
			continue;

		const size_t size_pos = name.find('\\', root.size());
		const size_t file_pos = size_pos != std::string::npos ? name.find('\\', size_pos + 1) : std::string::npos;
		if (file_pos == std::string::npos || name.find('\\', file_pos + 1) != std::string::npos)
			continue;

		const std::string character = name.substr(root.size(), size_pos - root.size());
		const std::string size = name.substr(size_pos + 1, file_pos - size_pos - 1);
		if (size != "no" && size != "z1")
			continue;

		// Bodies carry the size in their name ("ame_noa0900.png" -> "ame_z2a0900.png"); faces do not
		std::string file = name.substr(file_pos + 1);
		const std::string sized_prefix = character + "_" + size;
		if (file.compare(0, sized_prefix.size(), sized_prefix) == 0)
			file = character + "_" + sprite_size + file.substr(sized_prefix.size());

		if (names.count(root + character + "\\" + sprite_size + "\\" + file) != 0)
			(*hidden)[i] = true;
	}
}

bool ReportFailure(CArcFile* archive)
{
	CError error;
	error.Message(archive->GetProg()->GetHandle(), _T("Failed to extract %s"), archive->GetOpenFileInfo()->name.GetString());

	// The entry has been handled; falling back to the standard decoders would write encrypted data
	return true;
}
} // Anonymous namespace

/// Mounting
///
/// Opening "xxx.pfs" also opens "xxx.pfs.000", "xxx.pfs.001", ... from the same folder and lists
/// the files of all of them together. A numbered part that is opened directly is listed on its own.
bool CPfs::Mount(CArcFile* archive)
{
	bool is_main;
	if (!IsPfsPath(archive->GetArcPath(), &is_main))
		return false;

	std::vector<SFileInfo> file_infos;
	std::vector<u32> parts; // Split archive ID of each entry
	std::vector<Key> keys;  // By split archive ID
	std::unordered_set<std::string> names;

	if (!ReadPart(archive, &file_infos, &parts, &keys, &names))
	{
		archive->SeekHed();
		return false;
	}

	if (is_main)
	{
		const YCString main_path = archive->GetArcPath();

		for (u32 number = 0; number < 1000; number++)
		{
			TCHAR part_path[MAX_PATH];
			_stprintf(part_path, _T("%s.%03u"), main_path.GetString(), number);

			// CArcFile::Open() reports missing files, so check first
			if (!PathFileExists(part_path) || !archive->Open(part_path))
				break;

			archive->GetProg()->ReplaceAllFileSize(archive->GetArcSize());

			// A damaged part only loses its own files
			ReadPart(archive, &file_infos, &parts, &keys, &names);
		}
	}

	std::vector<bool> hidden(file_infos.size());
	std::vector<SFileInfo> sprites;
	std::vector<u32> sprite_parts;
	ComposeSprites(archive, keys, file_infos, parts, &hidden, &sprites, &sprite_parts);
	HideLowResolutionSprites(file_infos, &hidden);

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		if (!hidden[i])
		{
			archive->SetArcsID(parts[i]);
			archive->AddFileInfo(file_infos[i]);
		}
	}

	for (size_t i = 0; i < sprites.size(); i++)
	{
		archive->SetArcsID(sprite_parts[i]);
		archive->AddFileInfo(sprites[i]);
	}

	archive->SetFirstArc();
	return true;
}

/// Reads the index of the current split archive and appends its files.
///
/// @param names Lower-case names of the files listed so far; a file that already appears
///              in an earlier part is skipped (the parts share identical copies)
bool CPfs::ReadPart(CArcFile* archive, std::vector<SFileInfo>* file_infos, std::vector<u32>* parts, std::vector<Key>* keys, std::unordered_set<std::string>* names)
{
	const u32 part = archive->GetArcsID();
	if (keys->size() <= part)
		keys->resize(part + 1);

	std::vector<u8> index;
	std::vector<SFileInfo> part_infos;
	Key& key = (*keys)[part];
	if (!ReadIndex(archive, &index) || !ParseIndex(index, archive->GetArcSize(), &part_infos) || !Sha1(index.data(), index.size(), &key))
		return false;

	m_keys[archive->GetArcPath().GetString()] = key;

	for (auto& file_info : part_infos)
	{
		if (!names->insert(ToLower(file_info.name.GetString())).second)
			continue;

		file_infos->push_back(std::move(file_info));
		parts->push_back(part);
	}

	return true;
}

/// Decoding
///
/// Every entry is encrypted, so all of them are handled here.
bool CPfs::Decode(CArcFile* archive)
{
	bool is_main;
	if (!IsPfsPath(archive->GetArcPath(), &is_main))
		return false;

	const auto found = m_keys.find(archive->GetArcPath().GetString());
	if (found == m_keys.end())
		return false;

	const Key& key = found->second;
	const SFileInfo* file_info = archive->GetOpenFileInfo();

	if (file_info->format == sprite_format)
		return DecodeSprite(archive, key);

	std::vector<u8> data;
	if (!ReadDecrypted(archive, file_info->start, file_info->size_cmp, key, &data))
		return ReportFailure(archive);

	archive->OpenFile();
	archive->WriteFile(data.data(), static_cast<DWORD>(data.size()));
	archive->CloseFile();

	return true;
}

/// Extraction
///
/// Encrypted data is of no use, so extraction decrypts as well.
bool CPfs::Extract(CArcFile* archive)
{
	return Decode(archive);
}

/// Draws a face onto its body and writes the result as an image.
bool CPfs::DecodeSprite(CArcFile* archive, const Key& key)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();
	if (file_info->starts.size() != 2)
		return ReportFailure(archive);

	Bitmap body_scratch;
	Bitmap face_scratch;
	const Bitmap* body = DecodePart(archive, key, 0, &body_scratch);
	const Bitmap* face = body != nullptr ? DecodePart(archive, key, 1, &face_scratch) : nullptr;
	if (face == nullptr)
		return ReportFailure(archive);

	// Some faces (e.g. with tears or sweat) reach beyond the body, so the canvas covers both parts
	const s64 left = std::min<s64>(body->x, face->x);
	const s64 top = std::min<s64>(body->y, face->y);
	const s64 right = std::max<s64>(static_cast<s64>(body->x) + body->width, static_cast<s64>(face->x) + face->width);
	const s64 bottom = std::max<s64>(static_cast<s64>(body->y) + body->height, static_cast<s64>(face->y) + face->height);
	if (right - left > 16384 || bottom - top > 16384)
		return ReportFailure(archive);

	const u32 width = static_cast<u32>(right - left);
	const u32 height = static_cast<u32>(bottom - top);

	std::vector<u8> canvas;
	if (width == body->width && height == body->height)
	{
		canvas = body->pixels;
	}
	else
	{
		canvas.assign(static_cast<size_t>(width) * height * 4, 0);
		ImageUtils::BlendOver(canvas, width, body->pixels, body->width, body->height, static_cast<u32>(body->x - left), static_cast<u32>(body->y - top));
	}

	ImageUtils::BlendOver(canvas, width, face->pixels, face->width, face->height, static_cast<u32>(face->x - left), static_cast<u32>(face->y - top));

	CImage image;
	image.Init(archive, static_cast<s32>(width), static_cast<s32>(height), 32);
	image.WriteReverse(canvas.data(), canvas.size());
	image.Close();

	return true;
}

const CPfs::Bitmap* CPfs::DecodePart(CArcFile* archive, const Key& key, size_t part, Bitmap* scratch)
{
	const SFileInfo* file_info = archive->GetOpenFileInfo();
	const u64 offset = file_info->starts[part];

	const bool cacheable = part == 0;
	if (cacheable && m_cached_offset == offset && m_cached_archive_path == archive->GetArcPath() && !m_cached_body.pixels.empty())
		return &m_cached_body;

	Bitmap* bitmap = cacheable ? &m_cached_body : scratch;
	bitmap->pixels.clear();

	std::vector<u8> data;
	if (!ReadDecrypted(archive, offset, file_info->sizes_cmp[part], key, &data) ||
	    !ReadPngPosition(data, &bitmap->x, &bitmap->y) ||
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
