/*
 * Arx Remaster — RTX Remix M1 probe entry point
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXPROBE_H
#define ARX_GRAPHICS_REMIX_REMIXPROBE_H

#include "Configure.h"
#include "util/cmdline/CommandLine.h"

#if ARX_HAVE_RTX_REMIX

namespace remix {

[[nodiscard]] bool probeRequested();
[[nodiscard]] ExitStatus runProbe();

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXPROBE_H
