/*
 * Arx Raymix — what each Options → Ray tracing preset writes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_CORE_DXRPRESET_H
#define ARX_CORE_DXRPRESET_H

//! Values a preset writes into the DXR sliders. Kept apart from Config so the table can be
//! tested without loading a config file.
struct DxrPresetValues {
	int rtao;
	int dxrShadows;
	int dxrGi;
	int dxrReflections;
	int dxrTransReflections;
	int dxrShadowDenoise;
	int dxrGiDenoise;
	int dxrContact;
	int dxrDebris;
	int dxrTransparency;
	int dxrRr;
	int dxrFog; //!< volumetric haze, Off / Low / Medium / High
};

//! Preset slider positions: Off, Low, Medium, High, then Custom, which writes nothing.
constexpr int kDxrPresetOff = 0;
constexpr int kDxrPresetLow = 1;
constexpr int kDxrPresetMedium = 2;
constexpr int kDxrPresetHigh = 3;
constexpr int kDxrPresetCustom = 4;

//! Every preset turns Ray Reconstruction off: it is an experimental opt-in, not a quality step.
constexpr DxrPresetValues kDxrPresets[] = {
	//  ao sh gi metal trans shDn giDn contact debris transp rr fog
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Off
	{ 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 }, // Low
	{ 2, 2, 2, 1, 2, 1, 1, 1, 0, 0, 0, 1 }, // Medium
	{ 3, 3, 3, 2, 3, 1, 2, 1, 1, 0, 0, 2 }, // High
};
static_assert(sizeof(kDxrPresets) / sizeof(kDxrPresets[0]) == kDxrPresetHigh + 1);

//! True when any slider in \a v would make the DXR pass do work. Denoise, debris and
//! transparency only shape effects that are already on, so they do not count.
constexpr bool dxrValuesEnabled(const DxrPresetValues & v) {
	return v.rtao > 0 || v.dxrShadows > 0 || v.dxrGi > 0 || v.dxrReflections > 0
	       || v.dxrTransReflections > 0 || v.dxrContact > 0 || v.dxrFog > 0;
}

//! Preset the first run picks from the GPU's dedicated memory in MiB (0 when unknown).
//! DXGI reports a little under the label (a 6 GB card shows about 5.9 GiB), so the cut-offs
//! sit below 6 GB and 10 GB: under that Low, 10 GB cards and up High, Medium in between
//! and when the size is unknown.
constexpr int dxrFirstRunPreset(unsigned long long vramMiB) {
	if(vramMiB == 0) {
		return kDxrPresetMedium;
	}
	if(vramMiB < 5 * 1024) {
		return kDxrPresetLow;
	}
	if(vramMiB >= 9 * 1024) {
		return kDxrPresetHigh;
	}
	return kDxrPresetMedium;
}

#endif // ARX_CORE_DXRPRESET_H
