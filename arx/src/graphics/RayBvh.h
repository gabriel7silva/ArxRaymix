/*
 * Arx Raymix — bounding volume hierarchy for the software ray tracing path.
 *
 * Without DXR the ray pass runs as a compute shader that walks this tree itself. It is built on
 * the CPU from the same triangles the DXR path puts in its acceleration structures, with a
 * binned surface area heuristic, and laid out the way the shader reads it. Header-only and
 * free of graphics API headers so arxtest can check it against brute force.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_RAYBVH_H
#define ARX_GRAPHICS_RAYBVH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace raybvh {

//! 32 bytes, read by the shader as two float4. A leaf has count > 0 and holds the triangles
//! triIndex[first .. first + count); an inner node has count == 0 and its children at
//! leftOrFirst and leftOrFirst + 1.
struct Node {
	float min[3];
	std::uint32_t leftOrFirst;
	float max[3];
	std::uint32_t count;
};
static_assert(sizeof(Node) == 32, "the shader reads a node as two float4");

constexpr std::uint32_t kMaxLeafTriangles = 4;
constexpr int kBins = 12;
//! Deepest path the shader's traversal stack can hold. build() never goes deeper.
constexpr int kMaxDepth = 48;

struct Aabb {

	float lo[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
	                std::numeric_limits<float>::max() };
	float hi[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
	                -std::numeric_limits<float>::max() };

	void grow(const float * p) {
		for(int a = 0; a < 3; a++) {
			lo[a] = (std::min)(lo[a], p[a]);
			hi[a] = (std::max)(hi[a], p[a]);
		}
	}

	void grow(const Aabb & b) {
		for(int a = 0; a < 3; a++) {
			lo[a] = (std::min)(lo[a], b.lo[a]);
			hi[a] = (std::max)(hi[a], b.hi[a]);
		}
	}

	bool empty() const { return lo[0] > hi[0]; }

	float area() const {
		if(empty()) {
			return 0.f;
		}
		float dx = hi[0] - lo[0];
		float dy = hi[1] - lo[1];
		float dz = hi[2] - lo[2];
		return 2.f * (dx * dy + dy * dz + dz * dx);
	}

};

/*!
 * Builds the tree over triCount triangles whose vertices are float4 (x, y, z, unused), three per
 * triangle, the layout of D3D12Rtao::Pos. nodes[0] is the root. With no triangles both outputs
 * are left empty.
 */
inline void build(const float * verts4, size_t triCount, std::vector<Node> & nodes,
                  std::vector<std::uint32_t> & triIndex) {

	nodes.clear();
	triIndex.clear();
	if(triCount == 0) {
		return;
	}

	std::vector<Aabb> triBox(triCount);
	std::vector<float> centroid(triCount * 3);
	for(size_t t = 0; t < triCount; t++) {
		for(int v = 0; v < 3; v++) {
			triBox[t].grow(verts4 + (t * 3 + size_t(v)) * 4);
		}
		for(int a = 0; a < 3; a++) {
			centroid[t * 3 + size_t(a)] = 0.5f * (triBox[t].lo[a] + triBox[t].hi[a]);
		}
	}
	triIndex.resize(triCount);
	for(size_t t = 0; t < triCount; t++) {
		triIndex[t] = std::uint32_t(t);
	}

	nodes.reserve(triCount * 2);
	nodes.push_back(Node {});

	struct Task {
		std::uint32_t node;
		std::uint32_t first;
		std::uint32_t count;
		int depth;
	};
	std::vector<Task> stack;
	stack.push_back(Task { 0, 0, std::uint32_t(triCount), 1 });

	while(!stack.empty()) {

		Task task = stack.back();
		stack.pop_back();

		Aabb box;
		Aabb cbox;
		for(std::uint32_t i = task.first; i < task.first + task.count; i++) {
			box.grow(triBox[triIndex[i]]);
			cbox.grow(&centroid[size_t(triIndex[i]) * 3]);
		}
		Node & node = nodes[task.node];
		for(int a = 0; a < 3; a++) {
			node.min[a] = box.lo[a];
			node.max[a] = box.hi[a];
		}

		auto makeLeaf = [&]() {
			Node & leaf = nodes[task.node];
			leaf.leftOrFirst = task.first;
			leaf.count = task.count;
		};
		if(task.count <= kMaxLeafTriangles || task.depth >= kMaxDepth) {
			makeLeaf();
			continue;
		}

		// Binned SAH over the axis-aligned planes of the centroid bounds.
		int bestAxis = -1;
		int bestSplit = 0;
		float bestCost = std::numeric_limits<float>::max();
		for(int axis = 0; axis < 3; axis++) {
			float extent = cbox.hi[axis] - cbox.lo[axis];
			if(!(extent > 1e-6f)) {
				continue;
			}
			Aabb binBox[kBins];
			std::uint32_t binCount[kBins] = {};
			float scale = float(kBins) / extent;
			for(std::uint32_t i = task.first; i < task.first + task.count; i++) {
				std::uint32_t t = triIndex[i];
				int b = (std::min)(kBins - 1, int((centroid[size_t(t) * 3 + size_t(axis)] - cbox.lo[axis]) * scale));
				binBox[b].grow(triBox[t]);
				binCount[b]++;
			}
			float leftArea[kBins - 1];
			std::uint32_t leftCount[kBins - 1];
			Aabb acc;
			std::uint32_t n = 0;
			for(int b = 0; b < kBins - 1; b++) {
				acc.grow(binBox[b]);
				n += binCount[b];
				leftArea[b] = acc.area();
				leftCount[b] = n;
			}
			acc = Aabb();
			n = 0;
			for(int b = kBins - 1; b > 0; b--) {
				acc.grow(binBox[b]);
				n += binCount[b];
				float cost = float(leftCount[b - 1]) * leftArea[b - 1] + float(n) * acc.area();
				if(leftCount[b - 1] > 0 && n > 0 && cost < bestCost) {
					bestCost = cost;
					bestAxis = axis;
					bestSplit = b;
				}
			}
		}

		// Not splitting costs every triangle tested against every ray reaching the node.
		if(bestAxis < 0 || bestCost >= float(task.count) * box.area()) {
			if(task.count <= 4 * kMaxLeafTriangles || bestAxis < 0) {
				makeLeaf();
				continue;
			}
		}

		std::uint32_t mid;
		if(bestAxis >= 0) {
			float extent = cbox.hi[bestAxis] - cbox.lo[bestAxis];
			float scale = float(kBins) / extent;
			auto begin = triIndex.begin() + task.first;
			auto end = begin + task.count;
			auto split = std::partition(begin, end, [&](std::uint32_t t) {
				int b = (std::min)(kBins - 1, int((centroid[size_t(t) * 3 + size_t(bestAxis)] - cbox.lo[bestAxis]) * scale));
				return b < bestSplit;
			});
			mid = std::uint32_t(split - triIndex.begin());
		} else {
			mid = task.first + task.count / 2;
		}
		if(mid == task.first || mid == task.first + task.count) {
			mid = task.first + task.count / 2;
		}

		std::uint32_t left = std::uint32_t(nodes.size());
		nodes.push_back(Node {});
		nodes.push_back(Node {});
		nodes[task.node].leftOrFirst = left;
		nodes[task.node].count = 0;
		stack.push_back(Task { left + 1, mid, task.first + task.count - mid, task.depth + 1 });
		stack.push_back(Task { left, task.first, mid - task.first, task.depth + 1 });
	}
}

//! Ray / triangle distance (Möller-Trumbore), or a negative value for a miss. Shared by the
//! tests; the shader has its own copy of the same arithmetic.
inline float intersectTriangle(const float * o, const float * d, const float * v0, const float * v1,
                               const float * v2) {
	float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	float e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
	float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
	float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
	if(std::fabs(det) < 1e-12f) {
		return -1.f;
	}
	float inv = 1.f / det;
	float s[3] = { o[0] - v0[0], o[1] - v0[1], o[2] - v0[2] };
	float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
	if(u < 0.f || u > 1.f) {
		return -1.f;
	}
	float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
	float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
	if(v < 0.f || u + v > 1.f) {
		return -1.f;
	}
	return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

//! Nearest hit in (tmin, tmax) through the tree, the way the shader walks it: returns the
//! triangle index, or -1, and the distance in t.
inline long long closestHit(const std::vector<Node> & nodes, const std::vector<std::uint32_t> & triIndex,
                            const float * verts4, const float * o, const float * d, float tmin, float tmax,
                            float & t) {
	if(nodes.empty()) {
		return -1;
	}
	float inv[3];
	for(int a = 0; a < 3; a++) {
		inv[a] = 1.f / (std::fabs(d[a]) > 1e-12f ? d[a] : (d[a] < 0.f ? -1e-12f : 1e-12f));
	}
	auto slab = [&](const Node & n, float limit) {
		float t0 = tmin;
		float t1 = limit;
		for(int a = 0; a < 3; a++) {
			float ta = (n.min[a] - o[a]) * inv[a];
			float tb = (n.max[a] - o[a]) * inv[a];
			t0 = (std::max)(t0, (std::min)(ta, tb));
			t1 = (std::min)(t1, (std::max)(ta, tb));
		}
		return t0 <= t1;
	};
	long long best = -1;
	float bestT = tmax;
	std::uint32_t stack[kMaxDepth * 2];
	int sp = 0;
	stack[sp++] = 0;
	while(sp > 0) {
		const Node & n = nodes[stack[--sp]];
		if(!slab(n, bestT)) {
			continue;
		}
		if(n.count > 0) {
			for(std::uint32_t i = 0; i < n.count; i++) {
				std::uint32_t tri = triIndex[n.leftOrFirst + i];
				const float * v = verts4 + size_t(tri) * 12;
				float h = intersectTriangle(o, d, v, v + 4, v + 8);
				if(h > tmin && h < bestT) {
					bestT = h;
					best = tri;
				}
			}
		} else {
			stack[sp++] = n.leftOrFirst + 1;
			stack[sp++] = n.leftOrFirst;
		}
	}
	t = bestT;
	return best;
}

} // namespace raybvh

#endif // ARX_GRAPHICS_RAYBVH_H
