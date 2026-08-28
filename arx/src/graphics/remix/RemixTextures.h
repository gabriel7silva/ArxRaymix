/*
 * Arx Remaster — dump Arx textures to disk for RTX Remix CreateMaterial.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXTEXTURES_H
#define ARX_GRAPHICS_REMIX_REMIXTEXTURES_H

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include <vector>

#include <remix/remix_c.h>

class RemixApi;
class TextureContainer;
class Texture;

namespace remix {

//! True for grating / portcullis textures that should be alpha-tested cutouts.
[[nodiscard]] bool isCutoutTexture(const TextureContainer * tex);

[[nodiscard]] remixapi_MaterialHandle materialFor(TextureContainer * tex, RemixApi & api);
[[nodiscard]] remixapi_MaterialHandle materialForTexture(Texture * tex, RemixApi & api,
                                                         bool forceOpaque = false);

/*!
 * Mark a UI texture's pixels as changed, so its material and dump are rebuilt.
 *
 * Font atlases are filled in lazily as new glyphs are needed, so a material
 * created for the main menu is already out of date by the time a submenu draws
 * text the atlas had never seen.
 */
void invalidateUiMaterial(Texture * tex);

//! Hand over the textures marked since the last call, clearing the list.
[[nodiscard]] std::vector<Texture *> takeDirtyUiTextures();

/*!
 * Destroy a UI material and bump its dump revision.
 *
 * The caller must first release any mesh still referencing it: Remix keeps the
 * material handle on the surface, so freeing the material while a cached mesh
 * points at it leaves a dangling reference.
 */
void dropUiMaterial(Texture * tex, RemixApi & api);

void destroyMaterials(RemixApi & api);

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXTEXTURES_H
