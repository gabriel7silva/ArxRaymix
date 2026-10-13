/*
 * Arx Raymix launcher — reads the player package zip (stored or deflated entries, no zip64).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TOOLS_LAUNCHER_ZIP_H
#define ARX_TOOLS_LAUNCHER_ZIP_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zip {

struct Entry {
	std::string name;              //!< as stored, UTF-8 or ASCII
	std::uint16_t method = 0;      //!< 0 stored, 8 deflate
	std::uint32_t crc = 0;
	std::uint64_t compressedSize = 0;
	std::uint64_t size = 0;
	std::uint64_t localHeader = 0; //!< offset of the local file header
};

//! Reads the central directory.
bool list(const std::filesystem::path & archive, std::vector<Entry> & entries, std::string & error);

//! Writes one entry to \a target (parent folders created) and checks its CRC-32.
bool extract(const std::filesystem::path & archive, const Entry & entry,
             const std::filesystem::path & target, std::string & error);

} // namespace zip

#endif // ARX_TOOLS_LAUNCHER_ZIP_H
