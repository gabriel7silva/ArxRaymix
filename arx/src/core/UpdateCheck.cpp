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
	//! What the check found, logged by the main thread: the detached thread may still be waiting
	//! on the network when the game exits, and must not touch the logger (or any other static)
	//! once static destruction has begun.
	std::string message;
	bool logged = false;
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

	// Everything the thread needs is copied in: arx_version is a static string.
	http::Request request;
	request.url = latestReleaseApi();
	request.userAgent = "ArxRaymix/" + arx_version;
	request.accept = "application/vnd.github+json";
	request.maxBytes = 1024 * 1024;
	const std::uint64_t thisVersion = arx_version_number;
	
	std::thread([state = g_state, request = std::move(request), thisVersion]() {
		std::string body;
		std::string error;
		std::optional<Available> found;
		std::string message;
		if(!http::get(request, body, error)) {
			message = "Update check: " + error;
		} else if(std::optional<Release> release = parseRelease(body); !release) {
			message = "Update check: no usable release";
		} else if(release->version <= thisVersion) {
			message = "Update check: " + release->tag + " is not newer than this build";
		} else {
			message = "Update check: " + release->tag + " is available";
			found = Available { release->tag, release->pageUrl };
		}
		std::lock_guard<std::mutex> lock(state->mutex);
		state->available = std::move(found);
		state->message = std::move(message);
	}).detach();

	#endif

}

std::optional<Available> available() {
	std::lock_guard<std::mutex> lock(g_state->mutex);
	if(!g_state->logged && !g_state->message.empty()) {
		g_state->logged = true;
		LogInfo << g_state->message;
	}
	return g_state->available;
}

} // namespace updates
