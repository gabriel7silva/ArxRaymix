/*
 * Arx Raymix launcher (ArxRaymix.exe).
 *
 * Starts arx.exe, and before that asks GitHub whether a newer release exists. When the player
 * agrees it downloads the release zip, checks its SHA-256, moves the files the current package
 * installed into backup\, puts the new ones in place and starts the game. Only files listed in
 * package.txt (scripts/package.ps1) are touched: cfg.ini and saves live under Documents, and
 * the original Arx Fatalis data lives in the Steam / GOG folder.
 *
 *   ArxRaymix.exe [arx.exe arguments]
 *   ArxRaymix.exe --rollback     restore backup\ and skip the version that was rolled back
 *   ArxRaymix.exe --no-update    start the game without checking
 *   ArxRaymix.exe --install <zip> install a package downloaded by hand (e.g. from Nexus Mods),
 *                                checked against <zip>.sha256 when that file is next to it
 *
 * Any failure (offline, GitHub down, bad download) ends in starting the installed version.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// The engine builds for older Windows; the task dialogs need Vista's headers.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>

#include "core/UpdateRelease.h"
#include "platform/HttpGet.h"
#include "Zip.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------
// Text

bool g_portuguese = false;

const wchar_t * tr(const wchar_t * english, const wchar_t * portuguese) {
	return g_portuguese ? portuguese : english;
}

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

std::string utf8(std::wstring_view text) {
	if(text.empty()) {
		return std::string();
	}
	int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size_t(length > 0 ? length : 0), '\0');
	if(length > 0) {
		WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length, nullptr, nullptr);
	}
	return out;
}

fs::path pathFromUtf8(std::string_view text) {
	return fs::path(widen(text));
}
// ---------------------------------------------------------------------------------------------
// Log: launcher.log next to the exe, rewritten on every start.

std::ofstream g_log;

void log(const std::string & line) {
	if(g_log) {
		g_log << line << std::endl;
	}
}

// ---------------------------------------------------------------------------------------------
// Files

fs::path g_dir;

std::optional<std::string> readFile(const fs::path & file) {
	std::ifstream in(file, std::ios::binary);
	if(!in) {
		return std::nullopt;
	}
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

std::optional<updates::PackageManifest> readManifest(const fs::path & folder) {
	std::optional<std::string> text = readFile(folder / std::string(updates::kPackageManifest));
	if(!text) {
		return std::nullopt;
	}
	return updates::parseManifest(*text);
}

//! launcher.ini holds one setting: the version the player chose to skip.
fs::path settingsFile() {
	return g_dir / "launcher.ini";
}

std::string skippedVersion() {
	std::optional<std::string> text = readFile(settingsFile());
	if(!text) {
		return std::string();
	}
	std::string_view view = *text;
	constexpr std::string_view key = "skip=";
	size_t pos = view.find(key);
	if(pos == std::string_view::npos) {
		return std::string();
	}
	view.remove_prefix(pos + key.size());
	return std::string(view.substr(0, view.find_first_of("\r\n")));
}

void setSkippedVersion(const std::string & tag) {
	std::ofstream out(settingsFile(), std::ios::binary | std::ios::trunc);
	out << "skip=" << tag << "\r\n";
}

std::string sha256(const fs::path & file) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::string hex;
	if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
		return hex;
	}
	if(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
		std::ifstream in(file, std::ios::binary);
		std::vector<char> buffer(1024 * 1024);
		bool ok = bool(in);
		while(ok && in) {
			in.read(buffer.data(), std::streamsize(buffer.size()));
			std::streamsize got = in.gcount();
			if(got > 0) {
				ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(got), 0) == 0;
			}
		}
		unsigned char digest[32];
		if(ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
			static const char digits[] = "0123456789abcdef";
			for(unsigned char byte : digest) {
				hex += digits[byte >> 4];
				hex += digits[byte & 15];
			}
		}
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return hex;
}

//! True while arx.exe is running: a mapped image cannot be opened for writing.
bool gameIsRunning() {
	HANDLE file = CreateFileW((g_dir / "arx.exe").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
	                          FILE_ATTRIBUTE_NORMAL, nullptr);
	if(file == INVALID_HANDLE_VALUE) {
		return GetLastError() == ERROR_SHARING_VIOLATION;
	}
	CloseHandle(file);
	return false;
}

bool moveFile(const fs::path & from, const fs::path & to) {
	std::error_code ec;
	fs::create_directories(to.parent_path(), ec);
	// MoveFileEx also renames the running ArxRaymix.exe, which is how the launcher updates itself.
	if(!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		log("move failed: " + utf8(from.wstring()) + " -> " + utf8(to.wstring()) + " (error "
		    + std::to_string(GetLastError()) + ")");
		return false;
	}
	return true;
}

// Every file under folder, relative, with forward slashes.
std::vector<std::string> filesUnder(const fs::path & folder) {
	std::vector<std::string> files;
	std::error_code ec;
	for(fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
		if(it->is_regular_file(ec)) {
			files.push_back(utf8(fs::relative(it->path(), folder, ec).generic_wstring()));
		}
	}
	return files;
}

/*!
 * Moves the files of the installed package (and anything the incoming one would overwrite)
 * from g_dir into \a aside, then moves every file of \a incoming into g_dir. If any step fails,
 * puts everything back as it was.
 */
bool swapIn(const fs::path & incoming, const std::vector<std::string> & installed,
            const fs::path & aside, std::string & error) {

	std::set<std::string> outgoing(installed.begin(), installed.end());
	outgoing.insert(std::string(updates::kPackageManifest));
	std::vector<std::string> newFiles = filesUnder(incoming);
	for(const std::string & file : newFiles) {
		outgoing.insert(file);
	}

	std::error_code ec;
	fs::remove_all(aside, ec);

	std::vector<std::string> movedOut;
	std::vector<std::string> movedIn;
	auto undo = [&]() {
		for(const std::string & file : movedIn) {
			fs::remove(g_dir / pathFromUtf8(file), ec);
		}
		for(const std::string & file : movedOut) {
			moveFile(aside / pathFromUtf8(file), g_dir / pathFromUtf8(file));
		}
	};

	for(const std::string & file : outgoing) {
		fs::path path = g_dir / pathFromUtf8(file);
		if(!fs::exists(path, ec)) {
			continue;
		}
		if(!moveFile(path, aside / pathFromUtf8(file))) {
			error = "could not move " + file;
			undo();
			return false;
		}
		movedOut.push_back(file);
	}
	for(const std::string & file : newFiles) {
		if(!moveFile(incoming / pathFromUtf8(file), g_dir / pathFromUtf8(file))) {
			error = "could not install " + file;
			undo();
			return false;
		}
		movedIn.push_back(file);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Dialogs

void message(const std::wstring & text, UINT icon = MB_ICONINFORMATION) {
	MessageBoxW(nullptr, text.c_str(), L"Arx Raymix", MB_OK | icon);
}

enum Choice { UpdateNow = 100, NotNow = 101, SkipVersion = 102 };

HRESULT CALLBACK linkCallback(HWND, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) {
	if(notification == TDN_HYPERLINK_CLICKED) {
		ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
	}
	return S_OK;
}

Choice askToUpdate(const updates::Release & release, const std::string & installed) {

	std::wstring title = std::wstring(tr(L"Arx Raymix ", L"Arx Raymix ")) + widen(release.tag)
	                     + tr(L" is available", L" está disponível");
	std::wstring content = std::wstring(tr(L"You have ", L"Você tem a ")) + widen(installed)
	                       + tr(L". Settings, saves and the Arx Fatalis files are not changed, and the "
	                            L"current version is kept in the backup folder.",
	                            L". Configurações, saves e os arquivos do Arx Fatalis não são alterados, e a "
	                            L"versão atual fica guardada na pasta backup.");
	if(!release.pageUrl.empty()) {
		content += L"\n\n<a href=\"" + widen(release.pageUrl) + L"\">"
		           + tr(L"What's new", L"Novidades") + L"</a>";
	}

	TASKDIALOG_BUTTON buttons[] = {
		{ UpdateNow, tr(L"Update now", L"Atualizar agora") },
		{ NotNow, tr(L"Not now", L"Agora não") },
		{ SkipVersion, tr(L"Skip this version", L"Pular esta versão") },
	};
	TASKDIALOGCONFIG config = {};
	config.cbSize = sizeof(config);
	config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
	config.pszWindowTitle = L"Arx Raymix";
	config.pszMainIcon = TD_INFORMATION_ICON;
	config.pszMainInstruction = title.c_str();
	config.pszContent = content.c_str();
	config.pButtons = buttons;
	config.cButtons = ARRAYSIZE(buttons);
	config.nDefaultButton = UpdateNow;
	config.pfCallback = linkCallback;

	int pressed = NotNow;
	if(FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
		return NotNow;
	}
	return (pressed == UpdateNow || pressed == SkipVersion) ? Choice(pressed) : NotNow;
}

// Download, verify and unpack on a worker thread while a progress dialog runs.
struct Job {

	updates::Release release; //!< empty tag for --install: any Arx Raymix package is accepted
	fs::path work;     //!< g_dir\update
	fs::path staging;  //!< g_dir\update\staging
	fs::path zipFile;  //!< set up front for --install, by download() otherwise

	http::Progress progress;
	std::atomic<bool> finished { false };
	bool ok = false;
	std::string error;

	void run() {
		ok = (zipFile.empty() ? download() : checkLocal()) && unpack();
		finished = true;
	}

	bool download() {
		const updates::ReleaseAsset * package = release.package();
		const updates::ReleaseAsset * checksum = release.checksum();
		if(!package || !checksum) {
			error = "the release has no ArxRaymix-" + release.tag + ".zip / .zip.sha256";
			return false;
		}

		std::string userAgent = "ArxRaymix-Launcher";
		http::Request sumRequest;
		sumRequest.url = checksum->url;
		sumRequest.userAgent = userAgent;
		sumRequest.maxBytes = 4096;
		std::string sumText;
		if(!http::get(sumRequest, sumText, error)) {
			error = "checksum download: " + error;
			return false;
		}
		std::string expected = updates::parseSha256(sumText);
		if(expected.empty()) {
			error = "checksum file is malformed";
			return false;
		}

		std::error_code ec;
		fs::create_directories(work, ec);
		zipFile = work / pathFromUtf8(package->name);
		{
			std::ofstream out(zipFile, std::ios::binary | std::ios::trunc);
			if(!out) {
				error = "cannot write " + utf8(zipFile.wstring());
				return false;
			}
			http::Request request;
			request.url = package->url;
			request.userAgent = userAgent;
			request.maxBytes = 2ull * 1024 * 1024 * 1024;
			request.progress = &progress;
			if(package->size) {
				progress.total = package->size;
			}
			bool got = http::get(request, [&out](const char * data, size_t size) {
				out.write(data, std::streamsize(size));
				return bool(out);
			}, error);
			if(!got) {
				error = "download: " + error;
				return false;
			}
		}
		std::string actual = sha256(zipFile);
		if(actual != expected) {
			error = "SHA-256 mismatch (expected " + expected + ", got " + actual + ")";
			return false;
		}
		log("download ok, sha256 " + actual);
		return true;
	}

	//! A zip the player downloaded (from Nexus Mods, say): checked against <zip>.sha256 when present.
	bool checkLocal() {
		std::optional<std::string> sumText = readFile(zipFile.wstring() + L".sha256");
		if(!sumText) {
			log("no " + utf8(zipFile.wstring()) + ".sha256, installing unchecked");
			return true;
		}
		std::string expected = updates::parseSha256(*sumText);
		std::string actual = sha256(zipFile);
		if(expected.empty() || actual != expected) {
			error = "SHA-256 mismatch (expected " + expected + ", got " + actual + ")";
			return false;
		}
		return true;
	}
	
	bool unpack() {
		std::vector<zip::Entry> entries;
		if(!zip::list(zipFile, entries, error)) {
			return false;
		}
		std::error_code ec;
		fs::remove_all(staging, ec);
		for(const zip::Entry & entry : entries) {
			if(progress.cancel) {
				error = "cancelled";
				return false;
			}
			std::string path = updates::packageRelativePath(entry.name);
			if(path.empty()) {
				continue; // folders, and anything that would land outside the install
			}
			if(!zip::extract(zipFile, entry, staging / pathFromUtf8(path), error)) {
				return false;
			}
		}
		std::optional<updates::PackageManifest> manifest = readManifest(staging);
		bool versionOk = manifest && (release.tag.empty() ? updates::parseVersion(manifest->version) != 0
		                                                  : manifest->version == release.tag);
		if(!versionOk || !fs::exists(staging / "arx.exe", ec)) {
			error = "the package does not look like an Arx Raymix release";
			return false;
		}
		release.tag = manifest->version;
		return true;
	}

};

HRESULT CALLBACK progressCallback(HWND window, UINT notification, WPARAM wParam, LPARAM, LONG_PTR data) {
	Job & job = *reinterpret_cast<Job *>(data);
	switch(notification) {
		case TDN_CREATED: {
			SendMessageW(window, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
			break;
		}
		case TDN_TIMER: {
			std::uint64_t total = job.progress.total;
			std::uint64_t done = job.progress.done;
			if(total) {
				SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, WPARAM(std::min<std::uint64_t>(1000, done * 1000 / total)), 0);
			}
			if(job.finished) {
				SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0); // the only button; closes the dialog
			}
			break;
		}
		case TDN_BUTTON_CLICKED: {
			if(wParam == IDCANCEL && !job.finished) {
				job.progress.cancel = true;
				return S_FALSE; // stay open until the worker stops, then the timer closes it
			}
			break;
		}
		default: break;
	}
	return S_OK;
}

bool downloadWithProgress(Job & job) {

	std::thread worker([&job]() { job.run(); });

	std::wstring title = job.zipFile.empty()
	                     ? std::wstring(tr(L"Downloading Arx Raymix ", L"Baixando Arx Raymix ")) + widen(job.release.tag)
	                     : std::wstring(tr(L"Installing ", L"Instalando ")) + job.zipFile.filename().wstring();
	TASKDIALOGCONFIG config = {};
	config.cbSize = sizeof(config);
	config.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
	config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	config.pszWindowTitle = L"Arx Raymix";
	config.pszMainInstruction = title.c_str();
	config.pszContent = tr(L"The game starts when the update is in place.",
	                       L"O jogo abre quando a atualização terminar.");
	config.pfCallback = progressCallback;
	config.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
	TaskDialogIndirect(&config, nullptr, nullptr, nullptr);

	job.progress.cancel = !job.finished.load() || job.progress.cancel.load();
	worker.join();
	return job.ok && !job.progress.cancel;
}

// ---------------------------------------------------------------------------------------------
// Update and rollback

void cleanLeftovers() {
	std::error_code ec;
	fs::remove_all(g_dir / "update", ec);
}

void install(Job & job, const updates::PackageManifest & installed);

void checkForUpdate() {

	std::optional<updates::PackageManifest> installed = readManifest(g_dir);
	if(!installed) {
		log("no package.txt: not an installed package, update check skipped");
		return;
	}
	std::uint64_t installedVersion = updates::parseVersion(installed->version);
	if(installedVersion == 0) {
		log("installed version '" + installed->version + "' is not a release, update check skipped");
		return;
	}

	http::Request request;
	request.url = updates::latestReleaseApi();
	request.userAgent = "ArxRaymix-Launcher/" + installed->version;
	request.accept = "application/vnd.github+json";
	request.maxBytes = 1024 * 1024;
	std::string body;
	std::string error;
	if(!http::get(request, body, error)) {
		log("update check: " + error);
		return;
	}
	std::optional<updates::Release> release = updates::parseRelease(body);
	if(!release) {
		log("update check: no usable release");
		return;
	}
	log("installed " + installed->version + ", latest " + release->tag);
	if(release->version <= installedVersion || release->tag == skippedVersion()) {
		return;
	}
	if(!release->package() || !release->checksum()) {
		log("release " + release->tag + " has no package / checksum asset");
		return;
	}

	Choice choice = askToUpdate(*release, installed->version);
	if(choice == SkipVersion) {
		setSkippedVersion(release->tag);
		return;
	}
	if(choice != UpdateNow) {
		return;
	}

	while(gameIsRunning()) {
		int answer = MessageBoxW(nullptr, tr(L"Arx Raymix is running. Close the game, then press Retry.",
		                                     L"O Arx Raymix está aberto. Feche o jogo e clique em Repetir."),
		                         L"Arx Raymix", MB_RETRYCANCEL | MB_ICONWARNING);
		if(answer != IDRETRY) {
			return;
		}
	}

	Job job;
	job.release = *release;
	install(job, *installed);
}

//! Download (or check, for --install) and unpack \a job, then swap it in. Messages on failure.
void install(Job & job, const updates::PackageManifest & installed) {
	
	job.work = g_dir / "update";
	job.staging = job.work / "staging";
	if(!downloadWithProgress(job)) {
		log("update failed: " + (job.progress.cancel ? std::string("cancelled") : job.error));
		if(!job.progress.cancel) {
			message(std::wstring(tr(L"The update could not be installed:\n", L"Não foi possível atualizar:\n"))
			        + widen(job.error)
			        + tr(L"\n\nThe current version will start.", L"\n\nA versão atual será aberta."),
			        MB_ICONWARNING);
		}
		cleanLeftovers();
		return;
	}

	std::string error;
	if(!swapIn(job.staging, installed.files, g_dir / "backup", error)) {
		log("swap failed: " + error);
		message(std::wstring(tr(L"The update could not be installed:\n", L"Não foi possível atualizar:\n"))
		        + widen(error) + tr(L"\n\nNothing was changed.", L"\n\nNada foi alterado."), MB_ICONWARNING);
	} else {
		log("updated " + installed.version + " -> " + job.release.tag);
	}
	cleanLeftovers();
}

//! --install <zip>: a package downloaded by hand, e.g. from Nexus Mods.
void installLocal(const fs::path & zipFile) {
	std::optional<updates::PackageManifest> installed = readManifest(g_dir);
	if(!installed) {
		message(tr(L"package.txt is missing: --install only works in a folder extracted from an Arx Raymix zip.",
		           L"package.txt não encontrado: --install só funciona numa pasta extraída de um zip do Arx Raymix."),
		        MB_ICONWARNING);
		return;
	}
	if(gameIsRunning()) {
		message(tr(L"Close the game first.", L"Feche o jogo primeiro."), MB_ICONWARNING);
		return;
	}
	Job job;
	job.zipFile = fs::absolute(zipFile);
	install(job, *installed);
}

void rollback() {

	fs::path backup = g_dir / "backup";
	std::optional<updates::PackageManifest> previous = readManifest(backup);
	std::optional<updates::PackageManifest> current = readManifest(g_dir);
	if(!previous || !current) {
		message(tr(L"There is no previous version to go back to.", L"Não há versão anterior para restaurar."));
		return;
	}
	if(gameIsRunning()) {
		message(tr(L"Close the game first.", L"Feche o jogo primeiro."), MB_ICONWARNING);
		return;
	}

	// The same swap in reverse; what comes out goes to update\ and is deleted on the next start.
	std::string error;
	if(!swapIn(backup, current->files, g_dir / "update" / "rolled-back", error)) {
		log("rollback failed: " + error);
		message(std::wstring(tr(L"Could not restore the previous version:\n", L"Não foi possível restaurar a versão anterior:\n"))
		        + widen(error), MB_ICONWARNING);
		return;
	}
	std::error_code ec;
	fs::remove_all(backup, ec);
	setSkippedVersion(current->version);
	log("rolled back " + current->version + " -> " + previous->version);
	message(std::wstring(tr(L"Restored Arx Raymix ", L"Arx Raymix restaurado para a versão ")) + widen(previous->version)
	        + tr(L". The launcher will not offer ", L". O launcher não vai oferecer a ") + widen(current->version)
	        + tr(L" again.", L" de novo."));
}

// ---------------------------------------------------------------------------------------------
// Starting the game

std::wstring quote(const std::wstring & arg) {
	if(!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
		return arg;
	}
	// CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
	std::wstring out = L"\"";
	size_t backslashes = 0;
	for(wchar_t c : arg) {
		if(c == L'\\') {
			backslashes++;
			continue;
		}
		if(c == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
		} else {
			out.append(backslashes, L'\\');
		}
		backslashes = 0;
		out += c;
	}
	out.append(backslashes * 2, L'\\');
	out += L'"';
	return out;
}

int startGame(const std::vector<std::wstring> & args) {

	fs::path exe = g_dir / "arx.exe";
	std::wstring commandLine = quote(exe.wstring());
	for(const std::wstring & arg : args) {
		commandLine += L' ';
		commandLine += quote(arg);
	}

	// The game skips its own check: this launch has already asked GitHub.
	SetEnvironmentVariableW(L"ARX_UPDATE_CHECKED", L"1");

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if(!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
	                   g_dir.c_str(), &startup, &process)) {
		log("CreateProcess arx.exe failed (error " + std::to_string(GetLastError()) + ")");
		message(std::wstring(tr(L"Could not start arx.exe from\n", L"Não foi possível abrir o arx.exe em\n"))
		        + g_dir.wstring(), MB_ICONERROR);
		return 1;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 0;
}

} // anonymous namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {

	g_portuguese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;

	wchar_t self[MAX_PATH * 4] = {};
	GetModuleFileNameW(nullptr, self, DWORD(ARRAYSIZE(self)));
	g_dir = fs::path(self).parent_path();
	g_log.open(g_dir / "launcher.log", std::ios::trunc);

	using SetDpiContext = BOOL(WINAPI *)(HANDLE);
	if(HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
		if(auto setContext = reinterpret_cast<SetDpiContext>(
		       reinterpret_cast<void *>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))) {
			setContext(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
		}
	}

	bool doRollback = false;
	bool noUpdate = false;
	fs::path localZip;
	std::vector<std::wstring> passThrough;
	int argc = 0;
	if(LPWSTR * argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
		for(int i = 1; i < argc; i++) {
			std::wstring arg = argv[i];
			if(arg == L"--rollback") {
				doRollback = true;
			} else if(arg == L"--no-update") {
				noUpdate = true;
			} else if(arg == L"--install" && i + 1 < argc) {
				localZip = argv[++i];
			} else {
				passThrough.push_back(arg);
			}
		}
		LocalFree(argv);
	}

	cleanLeftovers();

	if(doRollback) {
		rollback();
		return 0;
	}

	if(!localZip.empty()) {
		installLocal(localZip);
	} else if(!noUpdate) {
		checkForUpdate();
	}

	return startGame(passThrough);
}
