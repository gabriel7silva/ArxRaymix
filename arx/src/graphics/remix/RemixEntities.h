/*
 * Export nearby in-scene entities (props, doors, NPCs) to Remix.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXENTITIES_H
#define ARX_GRAPHICS_REMIX_REMIXENTITIES_H

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include "math/Vector.h"

class RemixApi;

namespace remix {

void drawEntities(RemixApi & api, const Vec3f & playerPos, unsigned frame);
void destroyEntityMeshes(RemixApi & api);

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXENTITIES_H
