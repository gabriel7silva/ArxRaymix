/*
 * Arx Raymix — tests for the software ray tracing BVH.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/RayBvhTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "graphics/RayBvh.h"

CppUnit::AutoRegisterSuite<RayBvhTest> g_registerRayBvhTest;

namespace {

// A level-like soup: many small triangles scattered through a box, some large walls.
std::vector<float> makeTriangles(size_t count, unsigned seed) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> pos(-2000.f, 2000.f);
	std::uniform_real_distribution<float> size(-60.f, 60.f);
	std::vector<float> v;
	v.reserve(count * 12);
	for(size_t t = 0; t < count; t++) {
		float cx = pos(rng);
		float cy = pos(rng) * 0.25f;
		float cz = pos(rng);
		float s = (t % 50 == 0) ? 15.f : 1.f;
		for(int k = 0; k < 3; k++) {
			v.push_back(cx + size(rng) * s);
			v.push_back(cy + size(rng) * s);
			v.push_back(cz + size(rng) * s);
			v.push_back(1.f);
		}
	}
	return v;
}

int depthOf(const std::vector<raybvh::Node> & nodes, std::uint32_t i) {
	if(nodes[i].count > 0) {
		return 1;
	}
	return 1 + std::max(depthOf(nodes, nodes[i].leftOrFirst), depthOf(nodes, nodes[i].leftOrFirst + 1));
}

} // anonymous namespace

void RayBvhTest::emptyTest() {
	std::vector<raybvh::Node> nodes;
	std::vector<std::uint32_t> tris;
	raybvh::build(nullptr, 0, nodes, tris);
	CPPUNIT_ASSERT(nodes.empty());
	CPPUNIT_ASSERT(tris.empty());
	float o[3] = { 0, 0, 0 };
	float d[3] = { 1, 0, 0 };
	float t = 0.f;
	CPPUNIT_ASSERT_EQUAL(-1ll, raybvh::closestHit(nodes, tris, nullptr, o, d, 0.f, 1e6f, t));
}

void RayBvhTest::everyTriangleOnceTest() {
	const size_t n = 5000;
	std::vector<float> v = makeTriangles(n, 1);
	std::vector<raybvh::Node> nodes;
	std::vector<std::uint32_t> tris;
	raybvh::build(v.data(), n, nodes, tris);
	CPPUNIT_ASSERT_EQUAL(n, tris.size());
	std::vector<int> seen(n, 0);
	size_t inLeaves = 0;
	for(const raybvh::Node & node : nodes) {
		if(node.count > 0) {
			inLeaves += node.count;
			for(std::uint32_t i = 0; i < node.count; i++) {
				seen[tris[node.leftOrFirst + i]]++;
			}
		}
	}
	CPPUNIT_ASSERT_EQUAL(n, inLeaves);
	for(size_t t = 0; t < n; t++) {
		CPPUNIT_ASSERT_EQUAL(1, seen[t]);
	}
	CPPUNIT_ASSERT(depthOf(nodes, 0) <= raybvh::kMaxDepth);
	// Each node bounds its children.
	for(const raybvh::Node & node : nodes) {
		if(node.count == 0) {
			for(std::uint32_t c = node.leftOrFirst; c <= node.leftOrFirst + 1; c++) {
				for(int a = 0; a < 3; a++) {
					CPPUNIT_ASSERT(nodes[c].min[a] >= node.min[a] && nodes[c].max[a] <= node.max[a]);
				}
			}
		}
	}
}

void RayBvhTest::matchesBruteForceTest() {
	const size_t n = 3000;
	std::vector<float> v = makeTriangles(n, 7);
	std::vector<raybvh::Node> nodes;
	std::vector<std::uint32_t> tris;
	raybvh::build(v.data(), n, nodes, tris);
	std::mt19937 rng(99);
	std::uniform_real_distribution<float> pos(-2200.f, 2200.f);
	std::uniform_real_distribution<float> dir(-1.f, 1.f);
	int hits = 0;
	for(int r = 0; r < 2000; r++) {
		float o[3] = { pos(rng), pos(rng) * 0.25f, pos(rng) };
		float d[3] = { dir(rng), dir(rng) * 0.3f, dir(rng) };
		float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if(len < 1e-3f) {
			continue;
		}
		for(float & c : d) {
			c /= len;
		}
		long long bruteTri = -1;
		float bruteT = 5000.f;
		for(size_t t = 0; t < n; t++) {
			const float * p = v.data() + t * 12;
			float h = raybvh::intersectTriangle(o, d, p, p + 4, p + 8);
			if(h > 1.f && h < bruteT) {
				bruteT = h;
				bruteTri = (long long)t;
			}
		}
		float t = 0.f;
		long long tri = raybvh::closestHit(nodes, tris, v.data(), o, d, 1.f, 5000.f, t);
		CPPUNIT_ASSERT_EQUAL(bruteTri, tri);
		if(tri >= 0) {
			CPPUNIT_ASSERT_DOUBLES_EQUAL(double(bruteT), double(t), 1e-3);
			hits++;
		}
	}
	CPPUNIT_ASSERT(hits > 50); // the soup is dense enough that the comparison means something
}

void RayBvhTest::degenerateInputTest() {
	// Every triangle at the same place: no split plane exists, the build must still terminate.
	std::vector<float> v;
	for(int t = 0; t < 100; t++) {
		const float tri[12] = { 0, 0, 0, 1, 10, 0, 0, 1, 0, 10, 0, 1 };
		v.insert(v.end(), tri, tri + 12);
	}
	std::vector<raybvh::Node> nodes;
	std::vector<std::uint32_t> tris;
	raybvh::build(v.data(), 100, nodes, tris);
	CPPUNIT_ASSERT_EQUAL(size_t(100), tris.size());
	float o[3] = { 2, 2, -10 };
	float d[3] = { 0, 0, 1 };
	float t = 0.f;
	CPPUNIT_ASSERT(raybvh::closestHit(nodes, tris, v.data(), o, d, 0.f, 100.f, t) >= 0);
	CPPUNIT_ASSERT_DOUBLES_EQUAL(10.0, double(t), 1e-4);
}
