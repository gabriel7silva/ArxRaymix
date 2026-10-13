/*
 * Arx Raymix — HTTPS GET over WinHTTP for the update check and the launcher.
 *
 * Windows only. Depends on nothing but the C++ library and the Windows SDK so that
 * tools/launcher can compile it without the engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_PLATFORM_HTTPGET_H
#define ARX_PLATFORM_HTTPGET_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace http {

//! Shared with a UI thread: bytes so far, Content-Length (0 when unknown), and a cancel flag
//! the transfer checks between reads.
struct Progress {
	std::atomic<std::uint64_t> done { 0 };
	std::atomic<std::uint64_t> total { 0 };
	std::atomic<bool> cancel { false };
};

struct Request {
	std::string url;              //!< https:// only; redirects to other https hosts are followed
	std::string userAgent;
	std::string accept;           //!< Accept header, empty for none
	std::uint64_t maxBytes = 0;   //!< fail past this many bytes, 0 for no limit
	Progress * progress = nullptr;
};

/*!
 * Runs the request and hands the body to \a sink in chunks; \a sink returns false to abort.
 * Returns true for a complete 200 response. On failure \a error says what went wrong in
 * English, for the log.
 */
bool get(const Request & request, const std::function<bool(const char *, size_t)> & sink,
         std::string & error);

//! get() into a string.
bool get(const Request & request, std::string & body, std::string & error);

} // namespace http

#endif // ARX_PLATFORM_HTTPGET_H
