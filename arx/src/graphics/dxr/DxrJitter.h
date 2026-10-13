/*
 * Arx Raymix — subpixel jitter applied to the DXR reconstruction matrices.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_DXR_DXRJITTER_H
#define ARX_GRAPHICS_DXR_DXRJITTER_H

#include <glm/glm.hpp>

//! \a proj with the DLSS jitter added, so a point projects \a jitterNdcX / \a jitterNdcY NDC
//! units away from where \a proj puts it. The raster depth under DLSS is jittered; rebuilding
//! world positions from it with the plain projection is half a pixel off at every edge.
inline glm::mat4x4 jitteredProjection(const glm::mat4x4 & proj, float jitterNdcX, float jitterNdcY) {
	glm::mat4x4 jp = proj;
	for(int i = 0; i < 4; ++i) {
		jp[i][0] += jitterNdcX * jp[i][3];
		jp[i][1] += jitterNdcY * jp[i][3];
	}
	return jp;
}

#endif // ARX_GRAPHICS_DXR_DXRJITTER_H
