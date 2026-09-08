/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Based on:
===========================================================================
ARX FATALIS GPL Source Code
Copyright (C) 1999-2010 Arkane Studios SA, a ZeniMax Media company.

This file is part of the Arx Fatalis GPL Source Code ('Arx Fatalis Source Code'). 

Arx Fatalis Source Code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Arx Fatalis Source Code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Arx Fatalis Source Code.  If not, see 
<http://www.gnu.org/licenses/>.

In addition, the Arx Fatalis Source Code is also subject to certain additional terms. You should have received a copy of these 
additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Arx 
Fatalis Source Code. If not, please request a copy in writing from Arkane Studios at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing Arkane Studios, c/o 
ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/

#include "graphics/GlobalFog.h"

#include <cmath>
#include <cstring>
#include <algorithm>

#include "core/Application.h"
#include "core/Config.h"
#include "core/Core.h"
#include "graphics/BaseGraphicsTypes.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "io/log/Logger.h"
#include "platform/profiler/Profiler.h"

GLOBAL_MODS g_currentFogParameters;
GLOBAL_MODS g_desiredFogParameters;

// Slider 0 = one cell + fog wall. Slider 10 = old max. The previous linear
// map kept Low at ~6000 (plus a 4000 floor), so the next room stayed in view.
static const float DEFAULT_ZCLIP = 28000.f;
static const float DEFAULT_MINZCLIP = 1600.f;

Color g_fogColor;

void ARX_GLOBALMODS_Reset() {
	
	g_desiredFogParameters = GLOBAL_MODS();
	g_currentFogParameters = GLOBAL_MODS();
	g_currentFogParameters.zclip = DEFAULT_ZCLIP;
	g_desiredFogParameters.zclip = DEFAULT_ZCLIP;
	g_currentFogParameters.depthcolor = Color3f::black;
	g_desiredFogParameters.depthcolor = Color3f::black;
	g_fogColor = Color();
	
}

static float Approach(float current, float desired, float increment) {
	
	if(desired > current) {
		current = std::min(current + increment, desired);
	} else if(desired < current) {
		current = std::max(current - increment, desired);
	}
	
	return current;
}

void ARX_GLOBALMODS_Apply() {
	
	ARX_PROFILE_FUNC();
	
	float baseinc = g_framedelay;
	float incdiv1000 = g_framedelay * 0.001f;
	
	GLOBAL_MODS & current = g_currentFogParameters;
	GLOBAL_MODS & desired = g_desiredFogParameters;
	
	if(desired.flags & GMOD_ZCLIP) {
		current.zclip = Approach(current.zclip, desired.zclip, baseinc * 2);
	} else { // return to default...
		desired.zclip = current.zclip = Approach(current.zclip, DEFAULT_ZCLIP, baseinc * 2);
	}
	
	// Now goes for RGB mods
	if(desired.flags & GMOD_DCOLOR) {
		current.depthcolor.r = Approach(current.depthcolor.r, desired.depthcolor.r, incdiv1000);
		current.depthcolor.g = Approach(current.depthcolor.g, desired.depthcolor.g, incdiv1000);
		current.depthcolor.b = Approach(current.depthcolor.b, desired.depthcolor.b, incdiv1000);
	} else {
		current.depthcolor.r = Approach(current.depthcolor.r, 0, incdiv1000);
		current.depthcolor.g = Approach(current.depthcolor.g, 0, incdiv1000);
		current.depthcolor.b = Approach(current.depthcolor.b, 0, incdiv1000);
	}
	
	const float t = std::clamp(config.video.fogDistance, 0.f, 10.f) / 10.f;
	float fZclipp = DEFAULT_MINZCLIP + std::pow(t, 1.65f) * (DEFAULT_ZCLIP - DEFAULT_MINZCLIP);
	fZclipp += (g_camera->focal - 310.f) * 5.f;
	// Zone PATH_FARCLIP (current.zclip) drives the fog colour only; the Render slider owns the far
	// plane. Clamping cdepth to the zone's clip pins the far plane to whatever the level author
	// chose — on level 15 that is 2170 — so the slider stops doing anything above that and the
	// option reads as broken.
	g_camera->cdepth = (std::max)(fZclipp, DEFAULT_MINZCLIP);
	
	if(current.depthcolor.r + current.depthcolor.g + current.depthcolor.b < 0.04f) {
		g_fogColor = Color(Color3f::rgb(0.11f, 0.09f, 0.08f));
	} else {
		g_fogColor = Color(current.depthcolor);
	}
	
	static float s_loggedSlider = -1.f;
	static float s_loggedDepth = -1.f;
	if(std::abs(config.video.fogDistance - s_loggedSlider) > 0.01f
	   || std::abs(g_camera->cdepth - s_loggedDepth) > 40.f) {
		LogInfo << "Render distance slider=" << config.video.fogDistance
		        << " cdepth=" << g_camera->cdepth
		        << " fog=" << (fZFogRampStart * g_camera->cdepth)
		        << "-" << (fZFogRampEnd * g_camera->cdepth);
		s_loggedSlider = config.video.fogDistance;
		s_loggedDepth = g_camera->cdepth;
	}
}
