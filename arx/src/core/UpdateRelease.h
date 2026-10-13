/*
 * Arx Raymix — what the updater reads from a GitHub release and from a package.
 *
 * Header-only and free of engine and platform headers so the launcher (tools/launcher), the
 * in-game check (core/UpdateCheck.cpp) and arxtest all share one copy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_CORE_UPDATERELEASE_H
#define ARX_CORE_UPDATERELEASE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace updates {

//! Repository whose releases the launcher and the main menu look at.
inline constexpr std::string_view kLatestReleaseApi =
	"https://api.github.com/repos/gabriel7silva/ArxRaymix/releases/latest";

//! Where players download Arx Raymix: the main-menu "new version available" notice opens it.
inline constexpr std::string_view kModPageUrl = "https://www.nexusmods.com/arxfatalis/mods/37";

//! kLatestReleaseApi, or ARX_UPDATE_API when set (https only), for testing against another repository.
inline std::string latestReleaseApi() {
	const char * override = std::getenv("ARX_UPDATE_API");
	if(override && std::string_view(override).compare(0, 8, "https://") == 0) {
		return override;
	}
	return std::string(kLatestReleaseApi);
}

//! Written by scripts/package.ps1 next to arx.exe: the version on the first line, then one
//! path per line (relative, forward slashes) for every file the package installed.
inline constexpr std::string_view kPackageManifest = "package.txt";

/*!
 * "v1.5", "1.5" or "1.5.2.1" packed the way core/Version.cpp packs arx_version_number
 * (16 bits per component, major first). Anything else, including pre-release suffixes such
 * as "v1.5-beta", gives 0, which never compares newer than an installed version.
 */
inline std::uint64_t parseVersion(std::string_view text) {
	if(!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
		text.remove_prefix(1);
	}
	if(text.empty()) {
		return 0;
	}
	std::uint64_t packed = 0;
	int component = 0;
	std::uint32_t value = 0;
	bool haveDigit = false;
	for(size_t i = 0; i <= text.size(); i++) {
		if(i == text.size() || text[i] == '.') {
			if(!haveDigit || component >= 4) {
				return 0;
			}
			packed |= std::uint64_t(value) << (48 - 16 * component);
			component++;
			value = 0;
			haveDigit = false;
		} else if(text[i] >= '0' && text[i] <= '9') {
			value = value * 10 + std::uint32_t(text[i] - '0');
			if(value > 0xffff) {
				return 0;
			}
			haveDigit = true;
		} else {
			return 0;
		}
	}
	return packed;
}

namespace json {

//! Just enough JSON for the GitHub releases API: objects, arrays, strings, and scalars kept
//! as their source text.
struct Value {

	enum Kind { Null, Scalar, String, Array, Object };

	Kind kind = Null;
	std::string text;
	std::vector<Value> items;
	//! In source order; a vector because std::map of an incomplete type is not portable.
	std::vector<std::pair<std::string, Value>> members;

	const Value * get(std::string_view key) const {
		if(kind != Object) {
			return nullptr;
		}
		const Value * found = nullptr;
		for(const auto & member : members) {
			if(member.first == key) {
				found = &member.second; // the last duplicate wins, as in most parsers
			}
		}
		return found;
	}

	std::string string(std::string_view key) const {
		const Value * value = get(key);
		return (value && value->kind == String) ? value->text : std::string();
	}

};

class Parser {

	std::string_view m_in;
	size_t m_pos = 0;
	int m_depth = 0;

	void skipSpace() {
		while(m_pos < m_in.size() && (m_in[m_pos] == ' ' || m_in[m_pos] == '\t'
		                              || m_in[m_pos] == '\n' || m_in[m_pos] == '\r')) {
			m_pos++;
		}
	}

	bool consume(char c) {
		skipSpace();
		if(m_pos < m_in.size() && m_in[m_pos] == c) {
			m_pos++;
			return true;
		}
		return false;
	}

	static void appendUtf8(std::string & out, std::uint32_t cp) {
		if(cp < 0x80) {
			out += char(cp);
		} else if(cp < 0x800) {
			out += char(0xc0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3f));
		} else if(cp < 0x10000) {
			out += char(0xe0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		} else {
			out += char(0xf0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3f));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		}
	}

	bool hex4(std::uint32_t & out) {
		if(m_in.size() - m_pos < 4) {
			return false;
		}
		out = 0;
		for(int i = 0; i < 4; i++) {
			char c = m_in[m_pos++];
			out <<= 4;
			if(c >= '0' && c <= '9') {
				out |= std::uint32_t(c - '0');
			} else if(c >= 'a' && c <= 'f') {
				out |= std::uint32_t(c - 'a' + 10);
			} else if(c >= 'A' && c <= 'F') {
				out |= std::uint32_t(c - 'A' + 10);
			} else {
				return false;
			}
		}
		return true;
	}

	bool parseString(std::string & out) {
		// The opening quote has been consumed.
		while(m_pos < m_in.size()) {
			char c = m_in[m_pos++];
			if(c == '"') {
				return true;
			}
			if(c != '\\') {
				out += c;
				continue;
			}
			if(m_pos >= m_in.size()) {
				return false;
			}
			char e = m_in[m_pos++];
			switch(e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					std::uint32_t cp = 0;
					if(!hex4(cp)) {
						return false;
					}
					if(cp >= 0xd800 && cp < 0xdc00 && m_in.substr(m_pos, 2) == "\\u") {
						m_pos += 2;
						std::uint32_t low = 0;
						if(!hex4(low) || low < 0xdc00 || low >= 0xe000) {
							return false;
						}
						cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
					}
					appendUtf8(out, cp);
					break;
				}
				default: return false;
			}
		}
		return false;
	}

	bool parseValue(Value & out) {
		if(++m_depth > 64) {
			return false;
		}
		skipSpace();
		if(m_pos >= m_in.size()) {
			return false;
		}
		bool ok = true;
		char c = m_in[m_pos];
		if(c == '{') {
			m_pos++;
			out.kind = Value::Object;
			if(!consume('}')) {
				do {
					std::string key;
					Value member;
					ok = consume('"') && parseString(key) && consume(':') && parseValue(member);
					if(ok) {
						out.members.emplace_back(std::move(key), std::move(member));
					}
				} while(ok && consume(','));
				ok = ok && consume('}');
			}
		} else if(c == '[') {
			m_pos++;
			out.kind = Value::Array;
			if(!consume(']')) {
				do {
					out.items.emplace_back();
					ok = parseValue(out.items.back());
				} while(ok && consume(','));
				ok = ok && consume(']');
			}
		} else if(c == '"') {
			m_pos++;
			out.kind = Value::String;
			ok = parseString(out.text);
		} else {
			size_t start = m_pos;
			while(m_pos < m_in.size() && m_in[m_pos] != ',' && m_in[m_pos] != '}'
			      && m_in[m_pos] != ']' && m_in[m_pos] != ' ' && m_in[m_pos] != '\n'
			      && m_in[m_pos] != '\r' && m_in[m_pos] != '\t') {
				m_pos++;
			}
			out.text = std::string(m_in.substr(start, m_pos - start));
			out.kind = (out.text == "null") ? Value::Null : Value::Scalar;
			ok = !out.text.empty();
		}
		m_depth--;
		return ok;
	}

public:

	explicit Parser(std::string_view in) : m_in(in) { }

	std::optional<Value> parse() {
		Value root;
		if(!parseValue(root)) {
			return std::nullopt;
		}
		skipSpace();
		if(m_pos != m_in.size()) {
			return std::nullopt;
		}
		return root;
	}

};

} // namespace json

struct ReleaseAsset {
	std::string name;
	std::string url;
	std::uint64_t size = 0;
};

struct Release {

	std::string tag;
	std::uint64_t version = 0; //!< parseVersion(tag)
	std::string pageUrl;
	std::vector<ReleaseAsset> assets;

	//! The player package, "ArxRaymix-<tag>.zip".
	const ReleaseAsset * package() const {
		return find("ArxRaymix-" + tag + ".zip");
	}

	//! Its checksum, "ArxRaymix-<tag>.zip.sha256".
	const ReleaseAsset * checksum() const {
		return find("ArxRaymix-" + tag + ".zip.sha256");
	}

	const ReleaseAsset * find(std::string_view name) const {
		for(const ReleaseAsset & asset : assets) {
			if(asset.name == name) {
				return &asset;
			}
		}
		return nullptr;
	}

};

/*!
 * Reads the body of GET /repos/{owner}/{repo}/releases/latest. Drafts and pre-releases are
 * refused even though that endpoint never returns them, and so is a tag parseVersion rejects.
 */
inline std::optional<Release> parseRelease(std::string_view body) {

	std::optional<json::Value> root = json::Parser(body).parse();
	if(!root || root->kind != json::Value::Object) {
		return std::nullopt;
	}
	for(const char * flag : { "draft", "prerelease" }) {
		const json::Value * value = root->get(flag);
		if(value && value->text == "true") {
			return std::nullopt;
		}
	}

	Release release;
	release.tag = root->string("tag_name");
	release.version = parseVersion(release.tag);
	release.pageUrl = root->string("html_url");
	if(release.version == 0) {
		return std::nullopt;
	}

	if(const json::Value * assets = root->get("assets"); assets && assets->kind == json::Value::Array) {
		for(const json::Value & item : assets->items) {
			ReleaseAsset asset;
			asset.name = item.string("name");
			asset.url = item.string("browser_download_url");
			if(const json::Value * size = item.get("size"); size && size->kind == json::Value::Scalar) {
				for(char c : size->text) {
					if(c < '0' || c > '9') {
						break;
					}
					asset.size = asset.size * 10 + std::uint64_t(c - '0');
				}
			}
			if(!asset.name.empty() && asset.url.compare(0, 8, "https://") == 0) {
				release.assets.push_back(std::move(asset));
			}
		}
	}

	return release;
}

//! The first 64 hex digits of a "<hash>  <file>" line (sha256sum / Get-FileHash style),
//! lowercased, or an empty string.
inline std::string parseSha256(std::string_view text) {
	size_t start = 0;
	while(start < text.size() && (text[start] == ' ' || text[start] == '\t'
	                              || text[start] == '\r' || text[start] == '\n'
	                              || text[start] == '\xef' || text[start] == '\xbb'
	                              || text[start] == '\xbf')) {
		start++;
	}
	std::string hash;
	for(size_t i = start; i < text.size() && hash.size() < 64; i++) {
		char c = text[i];
		if(c >= 'A' && c <= 'F') {
			c = char(c - 'A' + 'a');
		}
		if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return std::string();
		}
		hash += c;
	}
	if(hash.size() != 64) {
		return std::string();
	}
	size_t end = start + 64;
	if(end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r'
	   && text[end] != '\n' && text[end] != '*') {
		return std::string();
	}
	return hash;
}

/*!
 * A zip entry path made relative to the package root: backslashes become slashes and the
 * single top folder the package is zipped in ("ArxRaymix-v1.5/") is dropped. Returns an
 * empty string for the top folder itself, for directories, and for anything that could land
 * outside the install folder (absolute paths, drive letters, "..", empty components).
 */
inline std::string packageRelativePath(std::string_view entry) {
	std::string path(entry);
	for(char & c : path) {
		if(c == '\\') {
			c = '/';
		}
	}
	size_t slash = path.find('/');
	if(slash == std::string::npos || slash == 0) {
		return std::string();
	}
	path.erase(0, slash + 1);
	if(path.empty() || path.back() == '/' || path.front() == '/') {
		return std::string();
	}
	size_t begin = 0;
	while(begin <= path.size()) {
		size_t end = path.find('/', begin);
		if(end == std::string::npos) {
			end = path.size();
		}
		std::string_view part = std::string_view(path).substr(begin, end - begin);
		if(part.empty() || part == "." || part == ".." || part.find(':') != std::string_view::npos) {
			return std::string();
		}
		begin = end + 1;
	}
	return path;
}

//! package.txt: the version, then the installed files.
struct PackageManifest {
	std::string version;
	std::vector<std::string> files;
};

inline std::optional<PackageManifest> parseManifest(std::string_view text) {
	PackageManifest manifest;
	bool first = true;
	while(!text.empty()) {
		size_t eol = text.find('\n');
		std::string_view line = text.substr(0, eol);
		text = (eol == std::string_view::npos) ? std::string_view() : text.substr(eol + 1);
		while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.remove_suffix(1);
		}
		if(first) {
			if(line.size() >= 3 && line.substr(0, 3) == "\xef\xbb\xbf") {
				line.remove_prefix(3);
			}
			manifest.version = std::string(line);
			first = false;
			continue;
		}
		if(line.empty() || line.front() == '#') {
			continue;
		}
		// Reuse the zip path rules: prefix a dummy top folder.
		std::string file = packageRelativePath("x/" + std::string(line));
		if(file.empty()) {
			return std::nullopt;
		}
		manifest.files.push_back(std::move(file));
	}
	if(manifest.version.empty()) {
		return std::nullopt;
	}
	return manifest;
}

} // namespace updates

#endif // ARX_CORE_UPDATERELEASE_H
