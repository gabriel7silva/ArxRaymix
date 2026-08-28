/*
 * Arx Remaster — shared Remix conversion state and the --remix-debug option.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixConvert.h"

#if ARX_HAVE_RTX_REMIX

#include <cmath>
#include <cstdlib>
#include <string>

#include "platform/ProgramOptions.h"
#include "util/cmdline/CommandLineException.h"

namespace remix {

unsigned g_debugFlags = 0;
Vec3f g_worldOrigin(0.f);

Vec3f snapToOriginGrid(const Vec3f & pos) {
	return Vec3f(std::floor(pos.x / kOriginGrid) * kOriginGrid,
	             std::floor(pos.y / kOriginGrid) * kOriginGrid,
	             std::floor(pos.z / kOriginGrid) * kOriginGrid);
}

namespace {

void setDebugFlags(const std::string & mask) {
	
	const char * begin = mask.c_str();
	char * end = nullptr;
	// Base 0: accept 12, 0xC and 014 so a bit list is easy to type either way.
	const unsigned long parsed = std::strtoul(begin, &end, 0);
	if(end == begin || (end && *end != '\0')) {
		throw util::cmdline::error(util::cmdline::error::invalid_cmd_syntax,
		                           "invalid Remix debug mask \"" + mask + "\"");
	}
	
	g_debugFlags = unsigned(parsed);
}

ARX_PROGRAM_OPTION_ARG("remix-debug", "", "RTX Remix debug bitmask: 1=probe triangle, 2=no materials,"
                       " 4=rebase world origin, 8=explicit matrices, 16=probe-style instances,"
                       " 32=fixed exposure, 64=transpose matrices, 128=Remix editor UI (Alt+X), 256=material without texture, 512=restore the NEVER alpha test bug, 1024=alpha state from draw call, 2048=re-attach the albedo-killing blend state, 4096=world-space entities (no skinning), 8192=sRGB DX10 textures, 16384=no normal maps, 32768=flip normal green, 65536=parallax,"
                       " 131072=world-space billboards, 262144=let the Remix developer menu win",
                       &setDebugFlags, "MASK")

} // namespace

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
