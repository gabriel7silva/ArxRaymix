/*
 * Arx Raymix — tests for the ray tracing preset table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/DxrPresetTest.h"

#include "core/DxrPreset.h"

CppUnit::AutoRegisterSuite<DxrPresetTest> g_registerDxrPresetTest;

namespace {

// The ranges Config::init clamps each key to when it reads cfg.ini.
constexpr DxrPresetValues kSliderMax = { 3, 3, 3, 3, 3, 1, 2, 1, 1, 2, 1 };

template <typename F>
void forEachField(const DxrPresetValues & a, const DxrPresetValues & b, F && f) {
	f("rtao", a.rtao, b.rtao);
	f("dxrShadows", a.dxrShadows, b.dxrShadows);
	f("dxrGi", a.dxrGi, b.dxrGi);
	f("dxrReflections", a.dxrReflections, b.dxrReflections);
	f("dxrTransReflections", a.dxrTransReflections, b.dxrTransReflections);
	f("dxrShadowDenoise", a.dxrShadowDenoise, b.dxrShadowDenoise);
	f("dxrGiDenoise", a.dxrGiDenoise, b.dxrGiDenoise);
	f("dxrContact", a.dxrContact, b.dxrContact);
	f("dxrDebris", a.dxrDebris, b.dxrDebris);
	f("dxrTransparency", a.dxrTransparency, b.dxrTransparency);
	f("dxrRr", a.dxrRr, b.dxrRr);
}

} // anonymous namespace

void DxrPresetTest::offWritesZeroTest() {
	const DxrPresetValues zero {};
	forEachField(kDxrPresets[kDxrPresetOff], zero, [](const char * name, int value, int) {
		CPPUNIT_ASSERT_EQUAL_MESSAGE(name, 0, value);
	});
}

void DxrPresetTest::onlyOffIsDisabledTest() {
	CPPUNIT_ASSERT(!dxrValuesEnabled(kDxrPresets[kDxrPresetOff]));
	for(int preset = kDxrPresetOff + 1; preset <= kDxrPresetHigh; preset++) {
		CPPUNIT_ASSERT(dxrValuesEnabled(kDxrPresets[preset]));
	}
}

void DxrPresetTest::presetsNeverLowerQualityTest() {
	for(int preset = kDxrPresetOff + 1; preset <= kDxrPresetHigh; preset++) {
		forEachField(kDxrPresets[preset - 1], kDxrPresets[preset], [](const char * name, int lower, int higher) {
			CPPUNIT_ASSERT_MESSAGE(name, higher >= lower);
		});
	}
}

void DxrPresetTest::presetsLeaveRayReconstructionOffTest() {
	for(int preset = kDxrPresetOff; preset <= kDxrPresetHigh; preset++) {
		CPPUNIT_ASSERT_EQUAL(0, kDxrPresets[preset].dxrRr);
	}
}

void DxrPresetTest::valuesStayInSliderRangeTest() {
	for(int preset = kDxrPresetOff; preset <= kDxrPresetHigh; preset++) {
		forEachField(kDxrPresets[preset], kSliderMax, [](const char * name, int value, int max) {
			CPPUNIT_ASSERT_MESSAGE(name, value >= 0 && value <= max);
		});
	}
}

void DxrPresetTest::shapingSlidersDoNotEnableTest() {
	DxrPresetValues v {};
	v.dxrShadowDenoise = 1;
	v.dxrGiDenoise = 2;
	v.dxrDebris = 1;
	v.dxrTransparency = 2;
	v.dxrRr = 1;
	CPPUNIT_ASSERT(!dxrValuesEnabled(v));
	v.dxrContact = 1;
	CPPUNIT_ASSERT(dxrValuesEnabled(v));
}
