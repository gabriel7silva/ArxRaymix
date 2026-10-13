/*
 * Arx Raymix — tests for the localisation encoding check.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/LocalisationEncodingTest.h"

#include <string_view>

#include "core/LocalisationEncoding.h"

CppUnit::AutoRegisterSuite<LocalisationEncodingTest> g_registerLocalisationEncodingTest;

using namespace std::string_view_literals;

void LocalisationEncodingTest::bomTest() {
	CPPUNIT_ASSERT(looksLikeUtf16Le("\xff\xfe[\0a\0"sv));
}

void LocalisationEncodingTest::bomlessUtf16Test() {
	// How the original loc.pak utext files start.
	CPPUNIT_ASSERT(looksLikeUtf16Le("[\0s\0y\0"sv));
}

void LocalisationEncodingTest::utf8Test() {
	CPPUNIT_ASSERT(!looksLikeUtf16Le("[system_menus]\nsystem_yes=Sim\n"sv));
	CPPUNIT_ASSERT(!looksLikeUtf16Le("[a]\nkey=Configura\xc3\xa7\xc3\xb5\x65s\n"sv));
}

void LocalisationEncodingTest::utf8BomTest() {
	CPPUNIT_ASSERT(!looksLikeUtf16Le("\xef\xbb\xbf[system]\n"sv));
}

void LocalisationEncodingTest::shortInputTest() {
	CPPUNIT_ASSERT(!looksLikeUtf16Le(""sv));
	CPPUNIT_ASSERT(!looksLikeUtf16Le("\xff"sv));
	CPPUNIT_ASSERT(!looksLikeUtf16Le("["sv));
}
