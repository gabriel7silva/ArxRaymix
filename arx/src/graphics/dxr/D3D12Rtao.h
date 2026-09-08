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
#include "math/Vector.h"

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
	
	//! The ray pass and the raster share one CBV_SRV_UAV heap, because only one can be bound at
	//! a time and the hit shader has to reach the game's textures through it. The renderer owns
	//! the heap and lends this module a reserved block of kHeapDescriptors at baseIndex.
	bool init(ID3D12Device * device, ID3D12DescriptorHeap * sharedHeap, unsigned baseIndex);

	//! Descriptors this module needs inside the shared heap.
	static constexpr unsigned kHeapDescriptors = 28;
	void shutdown();
	void resize(int width, int height);
	//! True when resize() would actually release and recreate the targets. Lets the caller pay for
	//! a GPU sync only on the frames that need one, instead of on every frame.
	[[nodiscard]] bool needsResize(int width, int height) const;
	
	[[nodiscard]] bool supported() const { return m_supported; }
	[[nodiscard]] bool ready() const { return m_ready; }
	[[nodiscard]] ID3D12Resource * colorCopyResource() const { return m_colorCopy.Get(); }
	[[nodiscard]] ID3D12Resource * waterMaskResource() const { return m_waterMask.Get(); }
	[[nodiscard]] ID3D12Resource * metalMaskResource() const { return m_metalMask.Get(); }
	[[nodiscard]] ID3D12Resource * waterDepthResource() const { return m_waterDepth.Get(); }
	//! Masks exist and are in PIXEL/NON_PIXEL shader-resource state.
	[[nodiscard]] bool masksReadable() const {
		return m_maskIsSrv && m_waterMask && m_metalMask && m_waterDepth;
	}
	//! rasterizeMasks ran this world frame (cleared in beginWorldFrame).
	[[nodiscard]] bool masksRasterized() const { return m_masksRasterized; }
	
	void beginWorldFrame();
	void markReflectOnlyStart();
	void clearRooms();
	void addRoom(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	             const unsigned short * indices, size_t nindices);
	void addWorld(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	              const unsigned short * indices, size_t nindices);
	void addWorld(Renderer::Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
	              const unsigned short * indices, size_t nindices);
	void addWater(const Vec3f & a, const Vec3f & b, const Vec3f & c);
	void addMetal(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	              const unsigned short * indices, size_t nindices);
	void addRoomMetal(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	                  const unsigned short * indices, size_t nindices);
	
	[[nodiscard]] size_t triangleCount() const {
		return (m_positions.size() + m_roomPositions.size() + m_waterPositions.size()) / 3;
	}
	[[nodiscard]] size_t dynTriangleCount() const { return m_positions.size() / 3; }
	
	static constexpr size_t kMaxShadowLights = 16;
	
	//! 0 = Low, 1 = Medium, 2 = High, 3 = Ultra.
	struct DistancePreset {
		float caster;
		int roomHops;
		size_t maxRooms;
		int lightHops;
		float specTMax;
		float giTMax;
	};
	
	[[nodiscard]] static DistancePreset distancePreset(int level) {
		static constexpr DistancePreset k[] = {
			{ 1800.f,  2, 10, 2, 1200.f, 200.f },
			{ 3200.f,  3, 20, 3, 2200.f, 300.f },
			{ 5500.f,  4, 32, 4, 3500.f, 400.f },
			{ 10000.f, 5, 48, 6, 6000.f, 550.f },
		};
		const int i = (level < 0) ? 0 : ((level > 3) ? 3 : level);
		return k[i];
	}
	
	struct Settings {
		int aoQuality = 0;
		int shadowQuality = 0;
		int giQuality = 0;
		int transRefl = 0;
		int metalRefl = 0;
		int shadowDenoise = 0;
		int giDenoise = 0;
		int contact = 0;
		int distance = 2;
		float range = 0.f;
		bool skipTemporal = false;
		//! NDC Halton offset matching the world-pass VS (pixel * 2 / size).
		float jitterNdcX = 0.f;
		float jitterNdcY = 0.f;
	};
	
	struct GpuLight {
		float x, y, z, intensity;
		float fallstart, fallend, radius, presence; // presence: 0..1 fade as a light enters / leaves the set
		float r, g, b, pad2; // hue only (max component 1); GI bounce takes the light's colour
	};
	
	bool apply(ID3D12GraphicsCommandList * list, ID3D12Resource * backbuffer,
	           ID3D12Resource * depth,
	           const glm::mat4x4 & view, const glm::mat4x4 & proj,
	           int width, int height, const Settings & settings,
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
	bool createMaskPipeline();
	bool ensureTargets(int width, int height);
	bool ensureGeometryBuffers(ID3D12GraphicsCommandList * list);
	bool buildAcceleration(ID3D12GraphicsCommandList * list);
	void updateDescriptors();
	void releaseTargets();
	void rasterizeMasks(ID3D12GraphicsCommandList * list, const glm::mat4x4 & viewProj,
	                    ID3D12Resource * depth, float jitterNdcX, float jitterNdcY);
	template <typename Vertex>
	void addTris(std::vector<Pos> & dst, size_t cap, Renderer::Primitive primitive,
	             const Vertex * vertices, size_t nvertices,
	             const unsigned short * indices, size_t nindices);
	void addPosTri(std::vector<Pos> & dst, size_t cap, const Vec3f & a, const Vec3f & b, const Vec3f & c);
	
	ID3D12Device * m_device = nullptr;
	ComPtr<ID3D12Device5> m_device5;
	ComPtr<ID3D12RootSignature> m_rtRoot;
	ComPtr<ID3D12RootSignature> m_compositeRoot;
	ComPtr<ID3D12RootSignature> m_maskRoot;
	ComPtr<ID3D12StateObject> m_rtState;
	ComPtr<ID3D12PipelineState> m_compositePso;
	ComPtr<ID3D12PipelineState> m_waterMaskPso;
	ComPtr<ID3D12PipelineState> m_waterDepthPso;
	ComPtr<ID3D12PipelineState> m_metalMaskPso;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	std::vector<unsigned char> m_dxil;
	//! Not owned: the renderer's CBV_SRV_UAV heap. Cleared in shutdown.
	ID3D12DescriptorHeap * m_heap = nullptr;
	//! First descriptor of this module's reserved block inside that heap.
	unsigned m_rtaoBase = 0;
	ComPtr<ID3D12Resource> m_vertUpload;
	ComPtr<ID3D12Resource> m_vertDefault;
	ComPtr<ID3D12Resource> m_roomUpload;
	ComPtr<ID3D12Resource> m_roomDefault;
	ComPtr<ID3D12Resource> m_waterUpload;
	ComPtr<ID3D12Resource> m_waterDefault;
	ComPtr<ID3D12Resource> m_metalUpload;
	ComPtr<ID3D12Resource> m_roomMetalUpload;
	// Params (b0). A root CBV rather than root constants: constants would occupy 60 of the
	// signature's 64 DWORDs and leave no room for the texture table the reflections need.
	// Ringed over kParamsSlots because, unlike root constants, an upload buffer is read by the
	// GPU at dispatch time and the renderer keeps more than one frame in flight.
	ComPtr<ID3D12Resource> m_paramsCbuf;
	void * m_paramsMapped = nullptr;
	unsigned m_paramsSlot = 0;
	ComPtr<ID3D12Resource> m_viewCbuf;
	ComPtr<ID3D12Resource> m_blas;
	ComPtr<ID3D12Resource> m_playerBlas;
	ComPtr<ID3D12Resource> m_roomBlas;
	ComPtr<ID3D12Resource> m_waterBlas;
	ComPtr<ID3D12Resource> m_tlas;
	ComPtr<ID3D12Resource> m_scratch;
	ComPtr<ID3D12Resource> m_instances;
	ComPtr<ID3D12Resource> m_shaderTable;
	ComPtr<ID3D12Resource> m_ao;
	ComPtr<ID3D12Resource> m_shadow;
	ComPtr<ID3D12Resource> m_gi;
	ComPtr<ID3D12Resource> m_colorCopy;
	ComPtr<ID3D12Resource> m_lights;
	// Temporal history: last frame's accumulated AO / shadow and the raw depth
	// RayGen saw, reprojected through m_prevViewProj on the next frame.
	ComPtr<ID3D12Resource> m_aoPrev;
	ComPtr<ID3D12Resource> m_shadowPrev;
	ComPtr<ID3D12Resource> m_depthCur;
	ComPtr<ID3D12Resource> m_depthPrev;
	ComPtr<ID3D12Resource> m_giPrev;
	ComPtr<ID3D12Resource> m_spec;
	ComPtr<ID3D12Resource> m_specPrev;
	ComPtr<ID3D12Resource> m_waterMask;
	ComPtr<ID3D12Resource> m_waterDepth;
	ComPtr<ID3D12Resource> m_metalMask;

	std::vector<Pos> m_positions;
	std::vector<Pos> m_roomPositions;
	std::vector<Pos> m_waterPositions;
	std::vector<Pos> m_metalPositions;
	std::vector<Pos> m_roomMetalPositions;
	glm::mat4x4 m_prevViewProj = glm::mat4x4(1.f);
	unsigned m_descriptorSize = 0;
	unsigned m_rtvSize = 0;
	int m_width = 0;
	int m_height = 0;
	bool m_supported = false;
	bool m_ready = false;
	bool m_loggedCap = false;
	bool m_histValid = false;
	bool m_aoIsUav = true;
	bool m_colorIsShader = false;
	bool m_vertsAreSrv = false;
	bool m_roomVertsAreSrv = false;
	bool m_waterVertsAreSrv = false;
	bool m_roomsDirty = true;
	bool m_maskIsSrv = false;
	bool m_masksRasterized = false;
	bool m_targetsFailed = false;
	int m_failedW = 0;
	int m_failedH = 0;
	size_t m_reflectOnlyStart = SIZE_MAX;
	
};

#endif // ARX_HAVE_D3D12

#endif // ARX_GRAPHICS_DXR_D3D12RTAO_H
