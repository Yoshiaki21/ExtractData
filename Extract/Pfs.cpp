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

bool IsPfs(CArcFile* archive)
{
	return lstrcmpi(archive->GetArcExten(), _T(".pfs")) == 0 && std::memcmp(archive->GetHeader(), "pf8", 3) == 0;
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
	u32 index_size;
	archive->SeekHed(3);
	if (!archive->ReadU32(&index_size))
		return false;

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
/// The combinations are taken from the extras table ("pc\<language>\extra\exlist.ipt").
///
/// @param hidden   Set to true for every entry that is consumed by a composed entry
/// @param sprites  Receives the composed entries
void ComposeSprites(CArcFile* archive, const CPfs::Key& key, const std::vector<SFileInfo>& file_infos, std::vector<bool>* hidden, std::vector<SFileInfo>* sprites)
{
	std::unordered_map<std::string, size_t> entries_by_name;
	const SFileInfo* table_entry = nullptr;

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		const std::string name = ToLower(file_infos[i].name.GetString());
		entries_by_name.emplace(name, i);

		// Prefer the Japanese table when there are several languages
		if (EndsWith(name, "\\extra\\exlist.ipt") && (table_entry == nullptr || name.find("\\ja\\") != std::string::npos))
			table_entry = &file_infos[i];
	}

	if (table_entry == nullptr)
		return;

	std::vector<u8> table_data;
	if (!ReadDecrypted(archive, table_entry->start, table_entry->size_cmp, key, &table_data))
		return;

	LuaValue table;
	LuaTableReader reader(std::string(table_data.begin(), table_data.end()));
	if (!reader.ReadAssignment("exfgtable", &table))
		return;

	const LuaValue* fg = table.Field("fg");
	const LuaValue* sizes = fg != nullptr ? fg->Field("size") : nullptr;
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

		// ":fg/ame/" -> "image\fg\ame\z2\"
		std::string dir = path->scalar;
		if (!dir.empty() && dir[0] == ':')
			dir.erase(0, 1);

		dir = "image/" + dir + sprite_size + "/";
		std::replace(dir.begin(), dir.end(), '/', '\\');

		for (const auto& outfit : outfits->Scalars())
		{
			for (const auto& pose : poses->items)
			{
				const std::vector<std::string> pose_fields = pose.Scalars();
				if (pose_fields.size() < 2)
					continue;

				const std::string& letter = pose_fields[0];
				const std::string body_stem = head->scalar + sprite_size + letter + outfit + pose_fields[1] + "0";

				const size_t body = find_entry(dir + body_stem + ".png");
				const LuaValue* pose_faces = faces->Field(letter.c_str());
				if (body == SIZE_MAX || pose_faces == nullptr)
					continue;

				for (const auto& face_id : pose_faces->Scalars())
				{
					const size_t face = find_entry(dir + face_id + ".png");
					if (face == SIZE_MAX)
						continue;

					combinations.push_back({body, face, dir + body_stem + "_" + face_id + ".png"});
					uses[body]++;
					uses[face]++;
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
		(*hidden)[combination.body] = true;
		(*hidden)[combination.face] = true;
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
bool CPfs::Mount(CArcFile* archive)
{
	if (!IsPfs(archive))
		return false;

	std::vector<u8> index;
	std::vector<SFileInfo> file_infos;
	Key key;
	if (!ReadIndex(archive, &index) || !ParseIndex(index, archive->GetArcSize(), &file_infos) || !Sha1(index.data(), index.size(), &key))
	{
		archive->SeekHed();
		return false;
	}

	m_keys[archive->GetArcPath().GetString()] = key;

	std::vector<bool> hidden(file_infos.size());
	std::vector<SFileInfo> sprites;
	ComposeSprites(archive, key, file_infos, &hidden, &sprites);

	for (size_t i = 0; i < file_infos.size(); i++)
	{
		if (!hidden[i])
			archive->AddFileInfo(file_infos[i]);
	}

	for (auto& file_info : sprites)
		archive->AddFileInfo(file_info);

	return true;
}

/// Decoding
///
/// Every entry is encrypted, so all of them are handled here.
bool CPfs::Decode(CArcFile* archive)
{
	if (!IsPfs(archive))
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
