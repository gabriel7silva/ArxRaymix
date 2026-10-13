/*
 * Arx Raymix — encoding check for the localisation files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_CORE_LOCALISATIONENCODING_H
#define ARX_CORE_LOCALISATIONENCODING_H

#include <string_view>

//! True when \a data should be read as UTF-16 LE. The original loc.pak files are UTF-16 LE
//! without a BOM, so besides the FF FE mark this trusts a NUL second byte: every utext file
//! opens with an ASCII '[' (5B 00), and a NUL never appears in UTF-8 text.
inline bool looksLikeUtf16Le(std::string_view data) {
	if(data.size() < 2) {
		return false;
	}
	if(static_cast<unsigned char>(data[0]) == 0xff && static_cast<unsigned char>(data[1]) == 0xfe) {
		return true;
	}
	return data[1] == '\0';
}

#endif // ARX_CORE_LOCALISATIONENCODING_H
