/*
 * Arx Raymix — tests for the software ray tracing BVH.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TESTS_GRAPHICS_RAYBVHTEST_H
#define ARX_TESTS_GRAPHICS_RAYBVHTEST_H

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

class RayBvhTest final : public CppUnit::TestFixture {

	CPPUNIT_TEST_SUITE(RayBvhTest);
	CPPUNIT_TEST(emptyTest);
	CPPUNIT_TEST(everyTriangleOnceTest);
	CPPUNIT_TEST(matchesBruteForceTest);
	CPPUNIT_TEST(degenerateInputTest);
	CPPUNIT_TEST_SUITE_END();

public:

	void emptyTest();
	void everyTriangleOnceTest();
	void matchesBruteForceTest();
	void degenerateInputTest();

};

#endif // ARX_TESTS_GRAPHICS_RAYBVHTEST_H
