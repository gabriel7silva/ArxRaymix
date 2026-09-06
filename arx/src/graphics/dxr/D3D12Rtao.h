/*
 * Arx Raymix — Option A RTAO (D3D12 DXR). Raster stays in ../d3d12/.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_DXR_D3D12RTAO_H
#define ARX_GRAPHICS_DXR_D3D12RTAO_H

#include "Configure.h"

#if ARX_HAVE_D3D12

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "graphics/Renderer.h"
#include "graphics/Vertex.h"

struct ID3D12Device;
struct ID3D12Device5;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12RootSignature;
struct ID3D12StateObject;
struct ID3D12PipelineState;
struct ID3D12DescriptorHeap;

class D3D12Rtao {
	
public:
	
	D3D12Rtao();
	~D3D12Rtao();
	
	D3D12Rtao(const D3D12Rtao &) = delete;
	D3D12Rtao & operator=(const D3D12Rtao &) = delete;
	
	bool init(ID3D12Device * device);
	void shutdown();
	void resize(int width, int height);
	
	[[nodiscard]] bool supported() const { return m_supported; }
	[[nodiscard]] bool ready() const { return m_ready; }
	
	void beginWorldFrame();
	void clearRooms();
	void addRoom(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	             const unsigned short * indices, size_t nindices);
	void addWorld(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	              const unsigned short * indices, size_t nindices);
	void addWorld(Renderer::Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
	              const unsigned short * indices, size_t nindices);
	
	[[nodiscard]] size_t triangleCount() const { return (m_positions.size() + m_roomPositions.size()) / 3; }
	
	static constexpr size_t kMaxShadowLights = 8;
	static constexpr float kCasterDistance = 4000.f;
	
	struct GpuLight {
		float x, y, z, intensity;
		float fallstart, fallend, pad0, pad1;
	};
	
	bool apply(ID3D12GraphicsCommandList * list, ID3D12Resource * backbuffer,
	           ID3D12Resource * depth,
	           const glm::mat4x4 & view, const glm::mat4x4 & proj,
	           int width, int height, int quality, bool shadows,
	           const GpuLight * lights, size_t lightCount, std::uint64_t rtvPtr);
	
private:
	
	struct Pos {
		float x, y, z, w;
	};
	
	template <class T>
	class ComPtr {
		T * p = nullptr;
	public:
		ComPtr() = default;
		~ComPtr() { reset(); }
		ComPtr(const ComPtr &) = delete;
		ComPtr & operator=(const ComPtr &) = delete;
		ComPtr(ComPtr && o) noexcept : p(o.p) { o.p = nullptr; }
		ComPtr & operator=(ComPtr && o) noexcept {
			if(this != &o) {
				reset();
				p = o.p;
				o.p = nullptr;
			}
			return *this;
		}
		void reset(T * next = nullptr) {
			if(p) {
				p->Release();
			}
			p = next;
		}
		T * Get() const { return p; }
		T ** put() { reset(); return &p; }
		T * operator->() const { return p; }
		explicit operator bool() const { return p != nullptr; }
	};
	
	bool compileRayLib();
	bool createPipeline();
	bool ensureTargets(int width, int height);
	bool ensureGeometryBuffers(ID3D12GraphicsCommandList * list);
	bool buildAcceleration(ID3D12GraphicsCommandList * list);
	void updateDescriptors();
	void addTris(std::vector<Pos> & dst, size_t cap, Renderer::Primitive primitive,
	             const SMY_VERTEX * vertices, size_t nvertices,
	             const unsigned short * indices, size_t nindices);
	
	ID3D12Device * m_device = nullptr;
	ComPtr<ID3D12Device5> m_device5;
	ComPtr<ID3D12RootSignature> m_rtRoot;
	ComPtr<ID3D12RootSignature> m_compositeRoot;
	ComPtr<ID3D12StateObject> m_rtState;
	ComPtr<ID3D12PipelineState> m_compositePso;
	std::vector<unsigned char> m_dxil;
	ComPtr<ID3D12DescriptorHeap> m_heap;
	ComPtr<ID3D12Resource> m_vertUpload;
	ComPtr<ID3D12Resource> m_vertDefault;
	ComPtr<ID3D12Resource> m_roomUpload;
	ComPtr<ID3D12Resource> m_roomDefault;
	ComPtr<ID3D12Resource> m_blas;
	ComPtr<ID3D12Resource> m_roomBlas;
	ComPtr<ID3D12Resource> m_tlas;
	ComPtr<ID3D12Resource> m_scratch;
	ComPtr<ID3D12Resource> m_instances;
	ComPtr<ID3D12Resource> m_shaderTable;
	ComPtr<ID3D12Resource> m_ao;
	ComPtr<ID3D12Resource> m_shadow;
	ComPtr<ID3D12Resource> m_colorCopy;
	ComPtr<ID3D12Resource> m_lights;
	
	std::vector<Pos> m_positions;
	std::vector<Pos> m_roomPositions;
	unsigned m_descriptorSize = 0;
	int m_width = 0;
	int m_height = 0;
	unsigned m_frameIndex = 0;
	bool m_supported = false;
	bool m_ready = false;
	bool m_loggedCap = false;
	bool m_aoIsUav = true;
	bool m_vertsAreSrv = false;
	bool m_roomVertsAreSrv = false;
	bool m_roomsDirty = true;
	
};

#endif // ARX_HAVE_D3D12

#endif // ARX_GRAPHICS_DXR_D3D12RTAO_H
