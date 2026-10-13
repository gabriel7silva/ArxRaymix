/*
 * Arx Raymix — tests for the updater's release, checksum, manifest and path rules.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/UpdateReleaseTest.h"

#include <string>

#include "core/UpdateRelease.h"

CppUnit::AutoRegisterSuite<UpdateReleaseTest> g_registerUpdateReleaseTest;

using namespace updates;

void UpdateReleaseTest::versionTest() {

	// Same packing as arx_version_number for "Arx Raymix 1.4" in arx/VERSION.
	CPPUNIT_ASSERT_EQUAL(std::uint64_t(1) << 48 | std::uint64_t(4) << 32, parseVersion("v1.4"));
	CPPUNIT_ASSERT_EQUAL(parseVersion("1.4"), parseVersion("v1.4"));
	CPPUNIT_ASSERT_EQUAL(parseVersion("1.4"), parseVersion("1.4.0.0"));
	CPPUNIT_ASSERT(parseVersion("v1.5") > parseVersion("v1.4"));
	CPPUNIT_ASSERT(parseVersion("v1.10") > parseVersion("v1.9"));
	CPPUNIT_ASSERT(parseVersion("v1.4.1") > parseVersion("v1.4"));
	CPPUNIT_ASSERT(parseVersion("v2.0") > parseVersion("v1.99.99"));

	for(const char * bad : { "", "v", "dev", "ci-abc123", "v1.5-beta", "1..4", "1.4.", ".4",
	                         "1.2.3.4.5", "v70000", "Arx Raymix 1.4" }) {
		CPPUNIT_ASSERT_EQUAL_MESSAGE(bad, std::uint64_t(0), parseVersion(bad));
	}
}

namespace {

const char * const kRelease = R"({
  "url": "https://api.github.com/repos/gabriel7silva/ArxRaymix/releases/1",
  "html_url": "https://github.com/gabriel7silva/ArxRaymix/releases/tag/v1.5",
  "tag_name": "v1.5",
  "name": "Arx Raymix v1.5 \u2014 \"teste\"",
  "draft": false,
  "prerelease": false,
  "body": "Notas\nlinha 2 \ud83d\ude00",
  "author": { "login": "x", "id": 1, "site_admin": false },
  "assets": [
    {
      "name": "ArxRaymix-v1.5.zip.sha256",
      "size": 90,
      "browser_download_url": "https://github.com/gabriel7silva/ArxRaymix/releases/download/v1.5/ArxRaymix-v1.5.zip.sha256"
    },
    {
      "name": "ArxRaymix-v1.5.zip",
      "size": 123456789,
      "browser_download_url": "https://github.com/gabriel7silva/ArxRaymix/releases/download/v1.5/ArxRaymix-v1.5.zip"
    },
    {
      "name": "insecure.zip",
      "size": 1,
      "browser_download_url": "http://example.com/insecure.zip"
    }
  ],
  "reactions": null
})";

} // anonymous namespace

void UpdateReleaseTest::releaseTest() {

	std::optional<Release> release = parseRelease(kRelease);
	CPPUNIT_ASSERT(release.has_value());
	CPPUNIT_ASSERT_EQUAL(std::string("v1.5"), release->tag);
	CPPUNIT_ASSERT_EQUAL(parseVersion("1.5"), release->version);
	CPPUNIT_ASSERT_EQUAL(std::string("https://github.com/gabriel7silva/ArxRaymix/releases/tag/v1.5"),
	                     release->pageUrl);
	CPPUNIT_ASSERT_EQUAL(size_t(2), release->assets.size()); // the http:// asset is dropped

	const ReleaseAsset * package = release->package();
	CPPUNIT_ASSERT(package != nullptr);
	CPPUNIT_ASSERT_EQUAL(std::uint64_t(123456789), package->size);
	CPPUNIT_ASSERT(package->url.find("/ArxRaymix-v1.5.zip") != std::string::npos);

	const ReleaseAsset * checksum = release->checksum();
	CPPUNIT_ASSERT(checksum != nullptr);
	CPPUNIT_ASSERT(checksum->url.find(".zip.sha256") != std::string::npos);

	// Escapes, including a surrogate pair, decode to UTF-8.
	json::Parser parser(R"({"a":"\u00e7\ud83d\ude00\n\"\\/"})");
	std::optional<json::Value> value = parser.parse();
	CPPUNIT_ASSERT(value.has_value());
	CPPUNIT_ASSERT_EQUAL(std::string("\xc3\xa7\xf0\x9f\x98\x80\n\"\\/"), value->string("a"));
}

void UpdateReleaseTest::refusedReleaseTest() {

	CPPUNIT_ASSERT(!parseRelease("").has_value());
	CPPUNIT_ASSERT(!parseRelease("not json").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"message":"Not Found"})").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"tag_name":"v1.5-rc1"})").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"tag_name":"v1.5","prerelease":true})").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"tag_name":"v1.5","draft":true})").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"tag_name":"v1.5",})").has_value());
	CPPUNIT_ASSERT(!parseRelease(R"({"tag_name":"v1.5"} trailing)").has_value());
	CPPUNIT_ASSERT(!parseRelease(std::string(200, '[')).has_value());

	// No assets is still a release (the menu notice needs no package), just not installable.
	std::optional<Release> bare = parseRelease(R"({"tag_name":"v1.5"})");
	CPPUNIT_ASSERT(bare.has_value());
	CPPUNIT_ASSERT(bare->package() == nullptr);
}

void UpdateReleaseTest::sha256Test() {

	const std::string hash = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
	CPPUNIT_ASSERT_EQUAL(hash, parseSha256(hash));
	CPPUNIT_ASSERT_EQUAL(hash, parseSha256(hash + "  ArxRaymix-v1.5.zip\n"));
	CPPUNIT_ASSERT_EQUAL(hash, parseSha256(hash + " *ArxRaymix-v1.5.zip\r\n"));
	CPPUNIT_ASSERT_EQUAL(hash, parseSha256("\xef\xbb\xbf" "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08"));

	CPPUNIT_ASSERT_EQUAL(std::string(), parseSha256(""));
	CPPUNIT_ASSERT_EQUAL(std::string(), parseSha256(hash.substr(0, 63)));
	CPPUNIT_ASSERT_EQUAL(std::string(), parseSha256(hash + "0"));
	CPPUNIT_ASSERT_EQUAL(std::string(), parseSha256("<html>" + hash));
}

void UpdateReleaseTest::packagePathTest() {

	CPPUNIT_ASSERT_EQUAL(std::string("arx.exe"), packageRelativePath("ArxRaymix-v1.5/arx.exe"));
	CPPUNIT_ASSERT_EQUAL(std::string("data/localisation/x.ini"),
	                     packageRelativePath("ArxRaymix-v1.5\\data\\localisation\\x.ini"));

	for(const char * bad : { "arx.exe", "ArxRaymix-v1.5/", "ArxRaymix-v1.5/data/", "ArxRaymix-v1.5//x",
	                         "ArxRaymix-v1.5/../x", "ArxRaymix-v1.5/a/../../x", "ArxRaymix-v1.5/./x",
	                         "ArxRaymix-v1.5/C:/Windows/x", "ArxRaymix-v1.5/C:x", "/abs",
	                         "ArxRaymix-v1.5/*.dll", "ArxRaymix-v1.5/a?.ini", "ArxRaymix-v1.5/a|b",
	                         "ArxRaymix-v1.5/a\"b", "ArxRaymix-v1.5/a<b>", "ArxRaymix-v1.5/a	b" }) {
		CPPUNIT_ASSERT_EQUAL_MESSAGE(bad, std::string(), packageRelativePath(bad));
	}
}

void UpdateReleaseTest::manifestTest() {

	std::optional<PackageManifest> manifest = parseManifest("\xef\xbb\xbfv1.5\r\narx.exe\r\n\r\n# c\r\ndata/x.ini\r\n");
	CPPUNIT_ASSERT(manifest.has_value());
	CPPUNIT_ASSERT_EQUAL(std::string("v1.5"), manifest->version);
	CPPUNIT_ASSERT_EQUAL(size_t(2), manifest->files.size());
	CPPUNIT_ASSERT_EQUAL(std::string("arx.exe"), manifest->files[0]);
	CPPUNIT_ASSERT_EQUAL(std::string("data/x.ini"), manifest->files[1]);

	CPPUNIT_ASSERT(!parseManifest("").has_value());
	CPPUNIT_ASSERT(!parseManifest("v1.5\n../outside.dll\n").has_value());
	CPPUNIT_ASSERT(!parseManifest("v1.5\nC:/Windows/system32/x.dll\n").has_value());
}
