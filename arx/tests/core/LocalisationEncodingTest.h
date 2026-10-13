/*
 * Arx Raymix — tests for the localisation encoding check.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TESTS_CORE_LOCALISATIONENCODINGTEST_H
#define ARX_TESTS_CORE_LOCALISATIONENCODINGTEST_H

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

class LocalisationEncodingTest final : public CppUnit::TestFixture {
	
	CPPUNIT_TEST_SUITE(LocalisationEncodingTest);
	CPPUNIT_TEST(bomTest);
	CPPUNIT_TEST(bomlessUtf16Test);
	CPPUNIT_TEST(utf8Test);
	CPPUNIT_TEST(utf8BomTest);
	CPPUNIT_TEST(shortInputTest);
	CPPUNIT_TEST_SUITE_END();
	
public:
	
	void bomTest();
	void bomlessUtf16Test();
	void utf8Test();
	void utf8BomTest();
	void shortInputTest();
	
};

#endif // ARX_TESTS_CORE_LOCALISATIONENCODINGTEST_H
