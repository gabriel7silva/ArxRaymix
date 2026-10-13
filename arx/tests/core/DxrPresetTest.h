/*
 * Arx Raymix — tests for the ray tracing preset table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_TESTS_CORE_DXRPRESETTEST_H
#define ARX_TESTS_CORE_DXRPRESETTEST_H

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

class DxrPresetTest final : public CppUnit::TestFixture {
	
	CPPUNIT_TEST_SUITE(DxrPresetTest);
	CPPUNIT_TEST(offWritesZeroTest);
	CPPUNIT_TEST(onlyOffIsDisabledTest);
	CPPUNIT_TEST(presetsNeverLowerQualityTest);
	CPPUNIT_TEST(presetsLeaveRayReconstructionOffTest);
	CPPUNIT_TEST(valuesStayInSliderRangeTest);
	CPPUNIT_TEST(shapingSlidersDoNotEnableTest);
	CPPUNIT_TEST(firstRunPresetFollowsVramTest);
	CPPUNIT_TEST_SUITE_END();
	
public:
	
	void offWritesZeroTest();
	void onlyOffIsDisabledTest();
	void presetsNeverLowerQualityTest();
	void presetsLeaveRayReconstructionOffTest();
	void valuesStayInSliderRangeTest();
	void shapingSlidersDoNotEnableTest();
	void firstRunPresetFollowsVramTest();
	
};

#endif // ARX_TESTS_CORE_DXRPRESETTEST_H
