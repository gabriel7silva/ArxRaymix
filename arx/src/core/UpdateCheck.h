/*
 * Arx Raymix — asks GitHub once per launch whether a newer release exists, for the notice in
 * the main menu.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_CORE_UPDATECHECK_H
#define ARX_CORE_UPDATECHECK_H

#include <optional>
#include <string>

namespace updates {

struct Available {
	std::string tag;     //!< "v1.5"
	std::string pageUrl; //!< GitHub release page (the menu notice opens kModPageUrl instead)
};

/*!
 * Starts the check on a background thread. Does nothing outside Windows, when
 * config.misc.checkForUpdates is off, when ARX_NO_UPDATE_CHECK is set, or when the launcher
 * has already checked for this launch (it sets ARX_UPDATE_CHECKED).
 */
void startCheck();

//! A release newer than this build, once the check has found one.
std::optional<Available> available();

} // namespace updates

#endif // ARX_CORE_UPDATECHECK_H
