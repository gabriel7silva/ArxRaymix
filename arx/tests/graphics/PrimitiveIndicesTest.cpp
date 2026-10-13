/*
 * Arx Raymix — tests for triangle decomposition and the DXR jitter matrix.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/PrimitiveIndicesTest.h"

#include <array>
#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "graphics/PrimitiveIndices.h"
#include "graphics/dxr/DxrJitter.h"

CppUnit::AutoRegisterSuite<PrimitiveIndicesTest> g_registerPrimitiveIndicesTest;

namespace {

typedef std::array<size_t, 3> Tri;

std::vector<Tri> decompose(Renderer::Primitive primitive, size_t n) {
	std::vector<Tri> tris;
	forEachTriangleIndices(primitive, n, [&](size_t a, size_t b, size_t c) {
		tris.push_back(Tri{ a, b, c });
	});
	return tris;
}

} // anonymous namespace

void PrimitiveIndicesTest::triangleListTest() {
	const std::vector<Tri> expected = { Tri{ 0, 1, 2 }, Tri{ 3, 4, 5 } };
	// The trailing two vertices do not make a triangle.
	CPPUNIT_ASSERT(decompose(Renderer::TriangleList, 8) == expected);
}

void PrimitiveIndicesTest::triangleStripWindingTest() {
	const std::vector<Tri> expected = { Tri{ 0, 1, 2 }, Tri{ 2, 1, 3 }, Tri{ 2, 3, 4 }, Tri{ 4, 3, 5 } };
	CPPUNIT_ASSERT(decompose(Renderer::TriangleStrip, 6) == expected);
}

void PrimitiveIndicesTest::triangleFanTest() {
	const std::vector<Tri> expected = { Tri{ 0, 1, 2 }, Tri{ 0, 2, 3 }, Tri{ 0, 3, 4 } };
	CPPUNIT_ASSERT(decompose(Renderer::TriangleFan, 5) == expected);
}

void PrimitiveIndicesTest::linesEmitNothingTest() {
	CPPUNIT_ASSERT(decompose(Renderer::LineList, 6).empty());
	CPPUNIT_ASSERT(decompose(Renderer::LineStrip, 6).empty());
}

void PrimitiveIndicesTest::tooFewVerticesTest() {
	for(size_t n = 0; n < 3; n++) {
		CPPUNIT_ASSERT(decompose(Renderer::TriangleList, n).empty());
		CPPUNIT_ASSERT(decompose(Renderer::TriangleStrip, n).empty());
		CPPUNIT_ASSERT(decompose(Renderer::TriangleFan, n).empty());
	}
}

void PrimitiveIndicesTest::resolveIndexTest() {
	const unsigned short indices[] = { 2, 0, 7 };
	size_t out = 99;
	CPPUNIT_ASSERT(resolveIndex(indices, 3, 4, 0, out));
	CPPUNIT_ASSERT_EQUAL(size_t(2), out);
	// Index past the vertex array.
	CPPUNIT_ASSERT(!resolveIndex(indices, 3, 4, 2, out));
	// Position past the index array.
	CPPUNIT_ASSERT(!resolveIndex(indices, 3, 4, 3, out));
	// Without indices the position is the vertex.
	CPPUNIT_ASSERT(resolveIndex(nullptr, 0, 4, 3, out));
	CPPUNIT_ASSERT_EQUAL(size_t(3), out);
	CPPUNIT_ASSERT(!resolveIndex(nullptr, 0, 4, 4, out));
}

void PrimitiveIndicesTest::jitterShiftsNdcTest() {
	const glm::mat4x4 proj = glm::perspective(1.2f, 16.f / 9.f, 1.f, 6400.f);
	const float jx = 0.0013f;
	const float jy = -0.0021f;
	const glm::mat4x4 jp = jitteredProjection(proj, jx, jy);
	const glm::vec4 points[] = {
		glm::vec4(0.f, 0.f, -10.f, 1.f),
		glm::vec4(120.f, -40.f, -800.f, 1.f),
		glm::vec4(-3.f, 7.f, -5000.f, 1.f),
	};
	for(const glm::vec4 & p : points) {
		const glm::vec4 a = proj * p;
		const glm::vec4 b = jp * p;
		CPPUNIT_ASSERT(std::abs(b.x / b.w - (a.x / a.w + jx)) < 1e-5f);
		CPPUNIT_ASSERT(std::abs(b.y / b.w - (a.y / a.w + jy)) < 1e-5f);
		// Depth is untouched.
		CPPUNIT_ASSERT(std::abs(b.z / b.w - a.z / a.w) < 1e-6f);
	}
}

void PrimitiveIndicesTest::zeroJitterIsIdentityTest() {
	const glm::mat4x4 proj = glm::perspective(1.2f, 16.f / 9.f, 1.f, 6400.f);
	CPPUNIT_ASSERT(jitteredProjection(proj, 0.f, 0.f) == proj);
}
