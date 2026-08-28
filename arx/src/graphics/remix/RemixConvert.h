/*
 * Shared Arx → Remix coordinate helpers (Y-up, scaled world) and the
 * --remix-debug test ladder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXCONVERT_H
#define ARX_GRAPHICS_REMIX_REMIXCONVERT_H

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include <remix/remix_c.h>

#include "math/Vector.h"

namespace remix {

constexpr float kWorldScale = 0.01f;
constexpr short kTileRadius = 16;
constexpr float kTileWorldSize = 100.f;

//! Arx units per world-origin step. Coarser than a tile, so rebasing never
//! forces a mesh rebuild that the tile change would not have forced anyway.
constexpr float kOriginGrid = 1000.f;

/*!
 * --remix-debug bits. Each one isolates a single hypothesis for the black
 * preview window so the whole ladder fits in one build.
 *
 * The decisive pair is Probe (is anything visible at all?) and RebaseOrigin
 * (does a camera far from the origin break the parameterized camera?).
 */
enum DebugBit {
	//! Draw the RemixProbe triangle 10 units in front of the camera, unlit
	//! material, with the probe's own sphere light just below the camera.
	DebugProbeTriangle = 1 << 0,
	//! Submit every surface with material = nullptr (tests the albedo BMP path).
	DebugNoMaterials = 1 << 1,
	//! Express the whole scene relative to a snapped origin near the player,
	//! so the camera sits close to (0, 0, 0) like the working probe does.
	DebugRebaseOrigin = 1 << 2,
	//! Send explicit view/projection matrices instead of CameraInfoParameterizedEXT.
	DebugExplicitMatrices = 1 << 3,
	//! Submit instances exactly like the probe: no blend EXT, no category flags.
	DebugProbeInstance = 1 << 4,
	//! Disable autoexposure and pin the tonemap exposure.
	DebugFixedExposure = 1 << 5,
	//! Transpose the matrices from DebugExplicitMatrices (row/column convention).
	DebugTransposeMatrices = 1 << 6,
	//! Start Remix with editorModeEnabled so Alt+X opens the developer menu
	//! and its Debug View (albedo, normals, geometry) inside the preview window.
	DebugEditorMode = 1 << 7,
	//! Keep the material but drop albedoTexture: separates a broken material
	//! object from a broken texture.
	DebugMaterialNoTexture = 1 << 8,
	//! Restore the old MaterialInfoOpaqueEXT::alphaTestType = 0, which is
	//! VK_COMPARE_OP_NEVER and was what made every textured surface render
	//! black. Kept so the regression can be reproduced on demand.
	DebugAlphaTestNever = 1 << 9,
	//! MaterialInfoOpaqueEXT::useDrawCallAlphaState = TRUE, so the alpha state
	//! comes from InstanceInfoBlendEXT instead of the material defaults.
	DebugDrawCallAlphaState = 1 << 10,
	//! Re-attach InstanceInfoBlendEXT, which suppressed the material albedo and
	//! left every surface flat white. Kept to reproduce that regression.
	DebugLegacyBlend = 1 << 11,
	//! Export entities as world-space vertex soup again instead of a skinned or
	//! transformed mesh. That is what produced the ghosting, so it is the A/B.
	DebugNoSkinning = 1 << 12,
	//! Write albedo as a DX10 DDS tagged B8G8R8A8_UNORM_SRGB instead of the
	//! legacy header, to see whether Remix has been reading it as linear.
	DebugSrgbTextures = 1 << 13,
	//! Albedo only, no derived normal map. The baseline for judging the relief.
	DebugNoNormalMaps = 1 << 14,
	/*!
	 * Invert the green channel of the derived normal maps.
	 *
	 * remixapi_HardcodedVertex carries no tangent, so Remix derives the tangent
	 * basis from the UVs and the normal - and we reverse winding for the Y flip,
	 * which can flip its handedness. Relief that reads inverted (mortar joints
	 * bulging out instead of sunk in) is the symptom; this is the A/B.
	 */
	DebugFlipNormalGreen = 1 << 15,
	//! Feed the derived height map to parallax occlusion as well. Off by default:
	//! height guessed from luminance overshoots on high-contrast textures.
	DebugParallax = 1 << 16,
	/*!
	 * Build the camera-facing quads from EERIECreateSprite() - fire, magic,
	 * sparks, light flares - in world space instead of screen space, so the path
	 * tracer sees them and they can light what is around them.
	 *
	 * Off by default. An earlier unconditional version of this left the game on
	 * a black screen with no level geometry and no crash in either log, and the
	 * cause was never identified. Behind a bit it can be toggled between two
	 * runs of the same build, which is the only way to tell a rendering problem
	 * from a loading one.
	 */
	DebugWorldBillboards = 1 << 17,
	/*!
	 * Stop pinning the look-related Remix options, so the developer menu (Alt+X)
	 * and the user.conf it writes are what decide.
	 *
	 * Normally the game re-applies its own values whenever a device registers or
	 * the render mode flips, which protects a run from a stale user.conf but also
	 * means a slider dragged in the developer menu is silently overwritten and
	 * never survives a restart. With this bit the game keeps only what would
	 * otherwise break the image - handedness, scene scale, the Y-flip - and
	 * leaves materials, exposure and lighting alone.
	 *
	 * This is how to find a value worth committing: tune it live, save it, and
	 * read it back out of user.conf.
	 */
	DebugFreeConfig = 1 << 18
};

//! Raw --remix-debug mask. Zero unless the option was passed.
extern unsigned g_debugFlags;

//! World origin in *Arx* space subtracted by toRemix(). Zero unless DebugRebaseOrigin.
extern Vec3f g_worldOrigin;

[[nodiscard]] inline bool debugEnabled(DebugBit bit) {
	return (g_debugFlags & unsigned(bit)) != 0;
}

[[nodiscard]] inline float entityExportRadius() {
	return float(kTileRadius) * kTileWorldSize;
}

//! Snap an Arx position to the origin grid. Used to pick g_worldOrigin.
[[nodiscard]] Vec3f snapToOriginGrid(const Vec3f & pos);

/*!
 * Arx object-space position → Remix object space.
 *
 * Deliberately does not subtract g_worldOrigin: object space is relative to the
 * model, and the level origin only belongs in the instance/bone translation.
 */
[[nodiscard]] inline Vec3f toRemixObject(const Vec3f & p) {
	return Vec3f(p.x * kWorldScale, -p.y * kWorldScale, p.z * kWorldScale);
}

//! Arx position → Remix position (Y-down → Y-up, scaled, origin-relative).
[[nodiscard]] inline Vec3f toRemix(const Vec3f & p) {
	const Vec3f rel = p - g_worldOrigin;
	return Vec3f(rel.x * kWorldScale, -rel.y * kWorldScale, rel.z * kWorldScale);
}

/*!
 * Arx direction → Remix direction. Directions must not pick up the origin
 * translation, so this is deliberately not toRemix(): normals and the camera
 * forward vector go through here.
 */
[[nodiscard]] inline Vec3f toRemixDir(const Vec3f & d) {
	return Vec3f(d.x, -d.y, d.z);
}

// D3DTA_TEXTURE=2, D3DTOP_SELECTARG1=2: albedo only. Vertex colors in Arx are
// baked lighting; if Remix treats them as baked, the path tracer never lights.
inline void fillOpaqueBlend(remixapi_InstanceInfoBlendEXT & blend) {
	blend = {};
	blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
	blend.alphaTestCompareOp = 7;
	blend.srcColorBlendFactor = 1;
	blend.textureColorArg1Source = 2;
	blend.textureColorArg2Source = 0;
	blend.textureColorOperation = 2;
	blend.textureAlphaArg1Source = 1;
	blend.textureAlphaOperation = 1;
	blend.tFactor = 0xFFFFFFFFu;
	blend.isVertexColorBakedLighting = FALSE;
}

[[nodiscard]] inline remixapi_InstanceCategoryFlags pathTracedOpaqueFlags() {
	return REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
}

[[nodiscard]] inline remixapi_Transform identityTransform() {
	return { {
		{ 1.f, 0.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f, 0.f },
		{ 0.f, 0.f, 1.f, 0.f },
	} };
}

/*!
 * Fill an InstanceInfo the same way for the room and for entities.
 *
 * No InstanceInfoBlendEXT: that fixed-function state (D3DTA_TEXTURE +
 * SELECTARG1) overrides the material, so albedo - texture and constant alike -
 * was thrown away and every surface rendered flat white. Our vertices are
 * already plain white, so there is no D3D blend state worth reproducing.
 */
inline void fillOpaqueInstance(remixapi_InstanceInfo & instance, remixapi_InstanceInfoBlendEXT & blend,
                               remixapi_MeshHandle mesh) {
	instance = {};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.mesh = mesh;
	instance.doubleSided = TRUE;
	instance.transform = identityTransform();
	if(!debugEnabled(DebugProbeInstance)) {
		instance.categoryFlags = pathTracedOpaqueFlags();
	}
	if(debugEnabled(DebugLegacyBlend)) {
		fillOpaqueBlend(blend);
		instance.pNext = &blend;
	}
}

/*!
 * Screen-space UI.
 *
 * Same rule as fillOpaqueInstance: attaching InstanceInfoBlendEXT overrides the
 * material and throws the albedo away, which paints the whole menu flat white.
 * Transparency comes from the material alpha test instead, and IGNORE_LIGHTS
 * keeps room torches from tinting the HUD.
 *
 * WORLD_UI takes these quads out of the path traced integration and composites
 * them over the frame, which is what makes the menu come out crisp rather than
 * as dim emissive planes. IGNORE_LIGHTS keeps room torches off the HUD.
 */
inline void fillUIInstance(remixapi_InstanceInfo & instance, remixapi_MeshHandle mesh) {
	instance = {};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.mesh = mesh;
	instance.doubleSided = TRUE;
	instance.transform = identityTransform();
	instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
}

/*!
 * Screen-space sprites that were perspective-divided (particles, magic, flares).
 * PARTICLE + ALPHA_BLEND_TO_CUTOUT is the Remix stand-in for additive D3D blend;
 * WORLD_UI keeps them composited instead of path-traced as dim emissive planes.
 */
inline void fillParticleInstance(remixapi_InstanceInfo & instance, remixapi_MeshHandle mesh) {
	instance = {};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.mesh = mesh;
	instance.doubleSided = TRUE;
	instance.transform = identityTransform();
	instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS
	                         | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXCONVERT_H
