/*
 * Arx Raymix — D3D12 raster renderer (no ray tracing).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_D3D12_D3D12RENDERER_H
#define ARX_GRAPHICS_D3D12_D3D12RENDERER_H

#include "Configure.h"

#if ARX_HAVE_D3D12

#include <memory>
#include <vector>

#include "graphics/Renderer.h"
#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/VertexBuffer.h"
#include "math/Rectangle.h"

#include <glm/glm.hpp>

struct ID3D12Device;
struct ID3D12Resource;

class D3D12Renderer;

class D3D12Texture final : public Texture {
public:
	explicit D3D12Texture(D3D12Renderer * renderer);
	~D3D12Texture() override;
	
	void upload() override;
	void destroy() override;
	
	[[nodiscard]] unsigned srvIndex() const { return m_srvIndex; }
	[[nodiscard]] bool onGpu() const { return m_onGpu; }
	void setOnGpu(bool ready) { m_onGpu = ready; }
	
protected:
	bool create() override;
	
private:
	bool createGpuTexture();
	
	D3D12Renderer * m_renderer = nullptr;
	ID3D12Resource * m_resource = nullptr;
	unsigned m_srvIndex = 0;
	Vec2i m_gpuSize;
	bool m_onGpu = false;
};

class D3D12TextureStage final : public TextureStage {
public:
	explicit D3D12TextureStage(unsigned stage);
	~D3D12TextureStage() override = default;
	
	Texture * getTexture() const override { return m_texture; }
	void setTexture(Texture * pTexture) override { m_texture = pTexture; }
	void resetTexture() override { m_texture = nullptr; }
	
	void setColorOp(TextureOp textureOp) override { m_colorOp = textureOp; }
	void setAlphaOp(TextureOp textureOp) override { m_alphaOp = textureOp; }
	void setMipMapLODBias(float bias) override { m_lodBias = bias; }
	
	[[nodiscard]] TextureOp getColorOp() const { return m_colorOp; }
	[[nodiscard]] TextureOp getAlphaOp() const { return m_alphaOp; }
	
private:
	Texture * m_texture = nullptr;
	TextureOp m_colorOp = OpModulate;
	TextureOp m_alphaOp = OpSelectArg1;
	float m_lodBias = 0.f;
};

class D3D12Renderer final : public Renderer {
public:
	D3D12Renderer();
	~D3D12Renderer() override;
	
	[[nodiscard]] bool createDevice(void * hwnd, int width, int height);
	
	void initialize() override;
	void beforeResize(bool wasOrIsFullscreen) override;
	void afterResize() override;
	
	void SetViewMatrix(const glm::mat4x4 & matView) override;
	void SetProjectionMatrix(const glm::mat4x4 & matProj) override;
	
	void ReleaseAllTextures() override;
	void RestoreAllTextures() override;
	void reloadColorKeyTextures() override;
	
	[[nodiscard]] Texture * createTexture() override;
	
	void SetViewport(const Rect & viewport) override;
	void SetScissor(const Rect & rect) override;
	
	void Clear(BufferFlags bufferFlags, Color clearColor = Color(), float clearDepth = 1.f,
	           size_t nrects = 0, Rect * rect = 0) override;
	
	void SetFogColor(Color color) override;
	void SetFogParams(float fogStart, float fogEnd) override;
	
	void SetAntialiasing(bool enable) override;
	void SetFillMode(FillMode mode) override;
	
	[[nodiscard]] bool needsHalfPixelOffset() const override { return false; }
	[[nodiscard]] const char * getGraphicsApiName() const override { return "DirectX 12"; }
	
	[[nodiscard]] float getMaxSupportedAnisotropy() const override { return 16.f; }
	void setMaxAnisotropy(float value) override { m_anisotropy = value; }
	
	[[nodiscard]] AlphaCutoutAntialising getMaxSupportedAlphaCutoutAntialiasing() const override {
		return NoAlphaCutoutAA;
	}
	
	[[nodiscard]] std::unique_ptr<VertexBuffer<TexturedVertex>> createVertexBufferTL(size_t capacity, BufferUsage usage) override;
	[[nodiscard]] std::unique_ptr<VertexBuffer<SMY_VERTEX>> createVertexBuffer(size_t capacity, BufferUsage usage) override;
	[[nodiscard]] std::unique_ptr<VertexBuffer<SMY_VERTEX3>> createVertexBuffer3(size_t capacity, BufferUsage usage) override;
	
	void drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
	                 unsigned short * indices, size_t nindices) override;
	
	bool getSnapshot(Image & image) override;
	bool getSnapshot(Image & image, size_t width, size_t height) override;
	
	void showFrame() override;
	void applyWorldRayEffects() override;
	[[nodiscard]] bool supportsRayTracing() const override;
	
	void drawTextured(Primitive primitive, const TexturedVertex * vertices, size_t count);
	void drawWorldVertices(Primitive primitive, const SMY_VERTEX * vertices, size_t count);
	void drawWorldVertices(Primitive primitive, const SMY_VERTEX3 * vertices, size_t count);
	void drawWorldIndexed(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	                      const unsigned short * indices, size_t nindices);
	void drawWorldIndexed(Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
	                      const unsigned short * indices, size_t nindices);
	
	[[nodiscard]] ID3D12Device * device() const;
	[[nodiscard]] unsigned allocateSrv();
	void createTextureSrv(ID3D12Resource * resource, unsigned index);
	bool uploadTextureData(ID3D12Resource * dest, const void * bgra, unsigned width, unsigned height,
	                       bool alreadyOnGpu);
	void freeTexture(ID3D12Resource * resource);
	
private:
	void releaseDevice();
	bool createPipeline();
	bool createFrameResources();
	void resizeSwapchain(int width, int height);
	void waitGpu();
	bool ensureCommandList();
	bool beginRecording();
	void bindDrawState(Primitive primitive);
	void drawGpuVerts(Primitive primitive, const void * verts, size_t stride, size_t count);
	void restoreRasterBind();
	void collectWorld(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	                  const unsigned short * indices, size_t nindices);
	void collectWorld(Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
	                  const unsigned short * indices, size_t nindices);
	
	struct Impl;
	Impl * m = nullptr;
	class D3D12Rtao * m_rtao = nullptr;
	bool m_loggedRtaoOff = false;
	bool m_loggedRtaoOn = false;
	bool m_loggedShadowsOff = false;
	bool m_loggedShadowsOn = false;
	bool m_loggedGiOff = false;
	bool m_loggedGiOn = false;
	
	Rect m_viewport;
	Rect m_scissor;
	glm::mat4x4 m_view = glm::mat4x4(1.f);
	glm::mat4x4 m_proj = glm::mat4x4(1.f);
	Color m_fogColor;
	float m_fogStart = 0.f;
	float m_fogEnd = 1.f;
	float m_anisotropy = 1.f;
	FillMode m_fillMode = FillSolid;
	int m_width = 0;
	int m_height = 0;
};

#endif // ARX_HAVE_D3D12

#endif // ARX_GRAPHICS_D3D12_D3D12RENDERER_H
