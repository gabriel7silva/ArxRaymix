/*
 * Arx Raymix — asks GitHub once per launch whether a newer release exists, for the notice in
 * the main menu.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/UpdateCheck.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include "core/Config.h"
#include "core/UpdateRelease.h"
#include "core/Version.h"
#include "io/log/Logger.h"
#include "platform/Platform.h"

#if ARX_PLATFORM == ARX_PLATFORM_WIN32
#include "platform/HttpGet.h"
#endif

namespace updates {

namespace {

struct State {
	std::mutex mutex;
	std::optional<Available> available;
};

// Shared with the detached thread so that neither side outlives the other's data.
std::shared_ptr<State> g_state = std::make_shared<State>();
bool g_started = false;

#if ARX_PLATFORM == ARX_PLATFORM_WIN32
bool envSet(const char * name) {
	const char * value = std::getenv(name);
	return value && *value && *value != '0';
}
#endif

} // anonymous namespace

void startCheck() {

	if(g_started) {
		return;
	}
	g_started = true;

	#if ARX_PLATFORM == ARX_PLATFORM_WIN32

	if(!config.misc.checkForUpdates || envSet("ARX_NO_UPDATE_CHECK") || envSet("ARX_UPDATE_CHECKED")) {
		return;
	}

	std::thread([state = g_state]() {
		http::Request request;
		request.url = latestReleaseApi();
		request.userAgent = "ArxRaymix/" + arx_version;
		request.accept = "application/vnd.github+json";
		request.maxBytes = 1024 * 1024;
		std::string body;
		std::string error;
		if(!http::get(request, body, error)) {
			LogInfo << "Update check: " << error;
			return;
		}
		std::optional<Release> release = parseRelease(body);
		if(!release) {
			LogInfo << "Update check: no usable release";
			return;
		}
		if(release->version <= arx_version_number) {
			LogInfo << "Update check: " << release->tag << " is not newer than this build";
			return;
		}
		LogInfo << "Update check: " << release->tag << " is available";
		std::lock_guard<std::mutex> lock(state->mutex);
		state->available = Available { release->tag, release->pageUrl };
	}).detach();

	#endif

}

std::optional<Available> available() {
	std::lock_guard<std::mutex> lock(g_state->mutex);
	return g_state->available;
}

} // namespace updates
