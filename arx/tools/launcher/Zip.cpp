/*
 * Arx Raymix launcher — reads the player package zip (stored or deflated entries, no zip64).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Zip.h"

#include <algorithm>
#include <fstream>

#include <zlib.h>

namespace zip {

namespace {

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;

std::uint16_t le16(const unsigned char * p) {
	return std::uint16_t(p[0] | (p[1] << 8));
}

std::uint32_t le32(const unsigned char * p) {
	return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)
	       | (std::uint32_t(p[3]) << 24);
}

bool readAt(std::ifstream & in, std::uint64_t offset, void * out, size_t size) {
	in.clear();
	in.seekg(std::streamoff(offset));
	in.read(static_cast<char *>(out), std::streamsize(size));
	return size_t(in.gcount()) == size;
}

} // anonymous namespace

bool list(const std::filesystem::path & archive, std::vector<Entry> & entries, std::string & error) {

	entries.clear();
	std::ifstream in(archive, std::ios::binary);
	if(!in) {
		error = "cannot open the package";
		return false;
	}
	in.seekg(0, std::ios::end);
	std::uint64_t fileSize = std::uint64_t(in.tellg());

	// The end record is 22 bytes plus a comment of up to 64 KiB.
	size_t tail = size_t(std::min<std::uint64_t>(fileSize, 22 + 0xffff));
	std::vector<unsigned char> buffer(tail);
	if(tail < 22 || !readAt(in, fileSize - tail, buffer.data(), tail)) {
		error = "package is not a zip file";
		return false;
	}
	const unsigned char * end = nullptr;
	for(size_t i = tail - 22 + 1; i-- > 0;) {
		if(le32(&buffer[i]) == kEndOfCentralDirectory) {
			end = &buffer[i];
			break;
		}
	}
	if(!end) {
		error = "package is not a zip file";
		return false;
	}
	std::uint16_t count = le16(end + 10);
	std::uint32_t directorySize = le32(end + 12);
	std::uint32_t directoryOffset = le32(end + 16);
	if(count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
		error = "zip64 packages are not supported";
		return false;
	}
	if(std::uint64_t(directoryOffset) + directorySize > fileSize) {
		error = "package is truncated";
		return false;
	}

	std::vector<unsigned char> directory(directorySize);
	if(!readAt(in, directoryOffset, directory.data(), directory.size())) {
		error = "package is truncated";
		return false;
	}
	size_t pos = 0;
	for(std::uint16_t i = 0; i < count; i++) {
		if(pos + 46 > directory.size() || le32(&directory[pos]) != kCentralHeader) {
			error = "package directory is damaged";
			return false;
		}
		const unsigned char * h = &directory[pos];
		Entry entry;
		entry.method = le16(h + 10);
		entry.crc = le32(h + 16);
		entry.compressedSize = le32(h + 20);
		entry.size = le32(h + 24);
		std::uint16_t nameLength = le16(h + 28);
		std::uint16_t extraLength = le16(h + 30);
		std::uint16_t commentLength = le16(h + 32);
		entry.localHeader = le32(h + 42);
		if(pos + 46 + nameLength + extraLength + commentLength > directory.size()) {
			error = "package directory is damaged";
			return false;
		}
		entry.name.assign(reinterpret_cast<const char *>(h + 46), nameLength);
		if(entry.compressedSize == 0xffffffff || entry.size == 0xffffffff || entry.localHeader == 0xffffffff) {
			error = "zip64 packages are not supported";
			return false;
		}
		entries.push_back(std::move(entry));
		pos += 46 + nameLength + extraLength + commentLength;
	}

	return true;
}

bool extract(const std::filesystem::path & archive, const Entry & entry,
             const std::filesystem::path & target, std::string & error) {

	if(entry.method != 0 && entry.method != 8) {
		error = "unsupported compression in " + entry.name;
		return false;
	}

	std::ifstream in(archive, std::ios::binary);
	unsigned char local[30];
	if(!in || !readAt(in, entry.localHeader, local, sizeof(local)) || le32(local) != kLocalHeader) {
		error = "damaged entry " + entry.name;
		return false;
	}
	std::uint64_t dataOffset = entry.localHeader + 30 + le16(local + 26) + le16(local + 28);
	in.clear();
	in.seekg(std::streamoff(dataOffset));

	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	std::ofstream out(target, std::ios::binary | std::ios::trunc);
	if(!out) {
		error = "cannot write " + entry.name;
		return false;
	}

	std::vector<char> input(256 * 1024);
	std::vector<char> output(256 * 1024);
	std::uint64_t remaining = entry.compressedSize;
	std::uint64_t written = 0;
	uLong crc = crc32(0L, Z_NULL, 0);

	z_stream stream = {};
	bool inflating = (entry.method == 8);
	if(inflating && inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
		error = "zlib init failed";
		return false;
	}

	bool ok = true;
	bool finished = false;
	while(ok && !finished) {
		size_t chunk = size_t(std::min<std::uint64_t>(remaining, input.size()));
		if(chunk > 0) {
			in.read(input.data(), std::streamsize(chunk));
			if(size_t(in.gcount()) != chunk) {
				ok = false;
				break;
			}
			remaining -= chunk;
		}
		if(!inflating) {
			out.write(input.data(), std::streamsize(chunk));
			crc = crc32(crc, reinterpret_cast<const Bytef *>(input.data()), uInt(chunk));
			written += chunk;
			finished = (remaining == 0);
			continue;
		}
		stream.next_in = reinterpret_cast<Bytef *>(input.data());
		stream.avail_in = uInt(chunk);
		do {
			stream.next_out = reinterpret_cast<Bytef *>(output.data());
			stream.avail_out = uInt(output.size());
			int result = inflate(&stream, Z_NO_FLUSH);
			if(result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
				ok = false;
				break;
			}
			size_t produced = output.size() - stream.avail_out;
			out.write(output.data(), std::streamsize(produced));
			crc = crc32(crc, reinterpret_cast<const Bytef *>(output.data()), uInt(produced));
			written += produced;
			if(result == Z_STREAM_END) {
				finished = true;
				break;
			}
			if(result == Z_BUF_ERROR && chunk == 0) {
				ok = false; // input ran out before the end of the stream
				break;
			}
		} while(stream.avail_out == 0 || stream.avail_in > 0);
	}
	if(inflating) {
		inflateEnd(&stream);
	}
	out.close();

	if(!ok || !out || written != entry.size || std::uint32_t(crc) != entry.crc) {
		error = "damaged entry " + entry.name;
		std::filesystem::remove(target, ec);
		return false;
	}
	return true;
}

} // namespace zip
