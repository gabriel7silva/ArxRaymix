/*
 * Arx Raymix — tests for the updater's release, checksum, manifest and path rules.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TESTS_CORE_UPDATERELEASETEST_H
#define ARX_TESTS_CORE_UPDATERELEASETEST_H

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

class UpdateReleaseTest final : public CppUnit::TestFixture {

	CPPUNIT_TEST_SUITE(UpdateReleaseTest);
	CPPUNIT_TEST(versionTest);
	CPPUNIT_TEST(releaseTest);
	CPPUNIT_TEST(refusedReleaseTest);
	CPPUNIT_TEST(sha256Test);
	CPPUNIT_TEST(packagePathTest);
	CPPUNIT_TEST(manifestTest);
	CPPUNIT_TEST_SUITE_END();

public:

	void versionTest();
	void releaseTest();
	void refusedReleaseTest();
	void sha256Test();
	void packagePathTest();
	void manifestTest();

};

#endif // ARX_TESTS_CORE_UPDATERELEASETEST_H
