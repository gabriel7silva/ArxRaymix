/*
 * Arx Raymix — tests for triangle decomposition and the DXR jitter matrix.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TESTS_GRAPHICS_PRIMITIVEINDICESTEST_H
#define ARX_TESTS_GRAPHICS_PRIMITIVEINDICESTEST_H

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

class PrimitiveIndicesTest final : public CppUnit::TestFixture {
	
	CPPUNIT_TEST_SUITE(PrimitiveIndicesTest);
	CPPUNIT_TEST(triangleListTest);
	CPPUNIT_TEST(triangleStripWindingTest);
	CPPUNIT_TEST(triangleFanTest);
	CPPUNIT_TEST(linesEmitNothingTest);
	CPPUNIT_TEST(tooFewVerticesTest);
	CPPUNIT_TEST(resolveIndexTest);
	CPPUNIT_TEST(jitterShiftsNdcTest);
	CPPUNIT_TEST(zeroJitterIsIdentityTest);
	CPPUNIT_TEST_SUITE_END();
	
public:
	
	void triangleListTest();
	void triangleStripWindingTest();
	void triangleFanTest();
	void linesEmitNothingTest();
	void tooFewVerticesTest();
	void resolveIndexTest();
	void jitterShiftsNdcTest();
	void zeroJitterIsIdentityTest();
	
};

#endif // ARX_TESTS_GRAPHICS_PRIMITIVEINDICESTEST_H
