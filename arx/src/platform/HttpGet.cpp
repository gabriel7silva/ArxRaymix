/*
 * Arx Raymix — HTTPS GET over WinHTTP for the update check and the launcher.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "platform/HttpGet.h"

#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace http {

namespace {

std::wstring widen(std::string_view text) {
	if(text.empty()) {
		return std::wstring();
	}
	int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
	std::wstring out(size_t(length > 0 ? length : 0), L'\0');
	if(length > 0) {
		MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length);
	}
	return out;
}

struct HandleCloser {
	void operator()(void * handle) const noexcept {
		if(handle) {
			WinHttpCloseHandle(handle);
		}
	}
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::string lastError(const char * what) {
	return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

} // anonymous namespace

bool get(const Request & request, const std::function<bool(const char *, size_t)> & sink,
         std::string & error) {

	if(request.url.compare(0, 8, "https://") != 0) {
		error = "only https:// URLs are allowed";
		return false;
	}

	std::wstring url = widen(request.url);
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	parts.dwHostNameLength = DWORD(-1);
	parts.dwUrlPathLength = DWORD(-1);
	parts.dwExtraInfoLength = DWORD(-1);
	if(!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
		error = "malformed URL";
		return false;
	}
	std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
	if(parts.lpszExtraInfo && parts.dwExtraInfoLength) {
		path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	}
	if(path.empty()) {
		path = L"/";
	}

	Handle session(WinHttpOpen(widen(request.userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if(!session) {
		// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1.
		session.reset(WinHttpOpen(widen(request.userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	}
	if(!session) {
		error = lastError("WinHttpOpen");
		return false;
	}
	// Resolve, connect, send, receive. Short enough that an offline machine does not wait long.
	WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
	#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
	protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	#endif
	if(!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}

	Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
	if(!connection) {
		error = lastError("WinHttpConnect");
		return false;
	}

	Handle req(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
	                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if(!req) {
		error = lastError("WinHttpOpenRequest");
		return false;
	}
	// Redirects (GitHub sends release assets to another host) stay on https.
	DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption(req.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

	std::wstring headers;
	if(!request.accept.empty()) {
		headers = L"Accept: " + widen(request.accept) + L"\r\n";
	}
	if(!WinHttpSendRequest(req.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
	                       headers.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
	   || !WinHttpReceiveResponse(req.get(), nullptr)) {
		error = lastError("request");
		return false;
	}

	DWORD status = 0;
	DWORD statusSize = sizeof(status);
	if(!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
		error = lastError("WinHttpQueryHeaders");
		return false;
	}
	if(status != 200) {
		error = "HTTP status " + std::to_string(status);
		return false;
	}

	if(request.progress) {
		wchar_t lengthText[32] = {};
		DWORD lengthSize = sizeof(lengthText);
		if(WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
		                       lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
			request.progress->total = std::uint64_t(_wcstoui64(lengthText, nullptr, 10));
		}
	}

	std::vector<char> buffer(64 * 1024);
	std::uint64_t received = 0;
	for(;;) {
		if(request.progress && request.progress->cancel) {
			error = "cancelled";
			return false;
		}
		DWORD read = 0;
		if(!WinHttpReadData(req.get(), buffer.data(), DWORD(buffer.size()), &read)) {
			error = lastError("WinHttpReadData");
			return false;
		}
		if(read == 0) {
			break;
		}
		received += read;
		if(request.maxBytes && received > request.maxBytes) {
			error = "response larger than expected";
			return false;
		}
		if(!sink(buffer.data(), read)) {
			error = "write failed";
			return false;
		}
		if(request.progress) {
			request.progress->done = received;
		}
	}

	return true;
}

bool get(const Request & request, std::string & body, std::string & error) {
	body.clear();
	return get(request, [&body](const char * data, size_t size) {
		body.append(data, size);
		return true;
	}, error);
}

} // namespace http
