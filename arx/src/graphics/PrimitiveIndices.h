/*
 * Arx Raymix — triangle decomposition shared by the D3D9, D3D12 and DXR paths.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_PRIMITIVEINDICES_H
#define ARX_GRAPHICS_PRIMITIVEINDICES_H

#include <cstddef>

#include "graphics/Renderer.h"

//! Calls \a emit(i0, i1, i2) once per triangle of \a n vertices drawn as \a primitive.
//! Odd strip triangles are emitted with the first two indices swapped so every triangle keeps
//! the strip's winding. Lines produce nothing.
template <typename Emit>
void forEachTriangleIndices(Renderer::Primitive primitive, size_t n, Emit && emit) {
	switch(primitive) {
		case Renderer::TriangleList:
			for(size_t i = 0; i + 2 < n; i += 3) {
				emit(i, i + 1, i + 2);
			}
			break;
		case Renderer::TriangleStrip:
			for(size_t i = 0; i + 2 < n; ++i) {
				if(i & 1) {
					emit(i + 1, i, i + 2);
				} else {
					emit(i, i + 1, i + 2);
				}
			}
			break;
		case Renderer::TriangleFan:
			for(size_t i = 1; i + 1 < n; ++i) {
				emit(size_t(0), i, i + 1);
			}
			break;
		default:
			break;
	}
}

//! Maps position \a i of a draw to a vertex index, through \a indices when there are any.
//! Returns false instead of reading past either array.
inline bool resolveIndex(const unsigned short * indices, size_t nindices, size_t nverts, size_t i,
                         size_t & out) {
	if(indices) {
		if(i >= nindices || indices[i] >= nverts) {
			return false;
		}
		out = indices[i];
		return true;
	}
	if(i >= nverts) {
		return false;
	}
	out = i;
	return true;
}

#endif // ARX_GRAPHICS_PRIMITIVEINDICES_H
