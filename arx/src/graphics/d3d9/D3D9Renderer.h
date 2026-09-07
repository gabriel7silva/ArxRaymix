/*
 * Arx Raymix — D3D9 renderer (one HWND, no OpenGL).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_D3D9_D3D9RENDERER_H
#define ARX_GRAPHICS_D3D9_D3D9RENDERER_H

#include "Configure.h"

#if ARX_HAVE_D3D9

#include <memory>
#include <vector>

#include "graphics/Renderer.h"
#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/VertexBuffer.h"
#include "math/Rectangle.h"

#include <glm/glm.hpp>

struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;

class D3D9Renderer;

class D3D9Texture final : public Texture {
public:
	explicit D3D9Texture(D3D9Renderer * renderer);
	~D3D9Texture() override;
	
	void upload() override;
	void destroy() override;
	
	[[nodiscard]] IDirect3DTexture9 * handle() const { return m_texture; }
	[[nodiscard]] bool isNPOT() const { return m_npot; }
	[[nodiscard]] bool hasGpuMips() const { return m_hasMips; }
	void detachRenderer() { m_renderer = nullptr; }
	
protected:
	bool create() override;
	
private:
	bool createGpuTexture();
	
	D3D9Renderer * m_renderer = nullptr;
	IDirect3DTexture9 * m_texture = nullptr;
	Vec2i m_gpuSize;
	bool m_npot = false;
	bool m_hasMips = false;
};

class D3D9TextureStage final : public TextureStage {
public:
	explicit D3D9TextureStage(D3D9Renderer * renderer, unsigned stage);
	~D3D9TextureStage() override = default;
	
	Texture * getTexture() const override { return m_texture; }
	void setTexture(Texture * pTexture) override { m_texture = pTexture; }
	void resetTexture() override { m_texture = nullptr; }
	
	void setColorOp(TextureOp textureOp) override { m_colorOp = textureOp; }
	void setAlphaOp(TextureOp textureOp) override { m_alphaOp = textureOp; }
	void setMipMapLODBias(float bias) override { m_lodBias = bias; }
	
	[[nodiscard]] TextureOp getColorOp() const { return m_colorOp; }
	
	void apply(IDirect3DDevice9 * device);
	void invalidateApplied();
	
private:
	D3D9Renderer * m_renderer = nullptr;
	Texture * m_texture = nullptr;
	TextureOp m_colorOp = OpModulate;
	TextureOp m_alphaOp = OpSelectArg1;
	float m_lodBias = 0.f;
	
	struct Applied {
		IDirect3DTexture9 * handle = nullptr;
		TextureOp colorOp = OpDisable;
		TextureOp alphaOp = OpDisable;
		WrapMode wrap = WrapRepeat;
		FilterMode minFilter = FilterLinear;
		FilterMode magFilter = FilterLinear;
		float lodBias = 0.f;
		float anisotropy = 0.f;
		bool valid = false;
	};
	Applied m_applied;
};

class D3D9Renderer final : public Renderer {
public:
	D3D9Renderer();
	~D3D9Renderer() override;
	
	//! Create the D3D9 device on the game HWND. Call before initialize().
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
	
	[[nodiscard]] float getMaxSupportedAnisotropy() const override { return m_maxAnisotropyCap; }
	void setMaxAnisotropy(float value) override;
	[[nodiscard]] float anisotropy() const { return m_anisotropy; }
	
	void setPresentVSync(int vsync) override;
	
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
	
	[[nodiscard]] IDirect3DDevice9 * device() const { return m_device; }
	[[nodiscard]] bool requirePow2Textures() const { return m_requirePow2Textures; }
	[[nodiscard]] unsigned maxTextureSize() const { return m_maxTextureSize; }
	
	void flushDeviceState();
	void drawTextured(Primitive primitive, const TexturedVertex * vertices, size_t count);
	void drawWorldVertices(Primitive primitive, const SMY_VERTEX * vertices, size_t count);
	void drawWorldVertices(Primitive primitive, const SMY_VERTEX3 * vertices, size_t count);
	void drawWorldIndexed(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
	                      const unsigned short * indices, size_t nindices);
	void drawWorldIndexed(Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
	                      const unsigned short * indices, size_t nindices);
	
private:
	void releaseDevice();
	void registerTexture(D3D9Texture * texture);
	void unregisterTexture(D3D9Texture * texture);
	void invalidateTextureStages();
	bool resetDevice(int width, int height);
	bool recoverDeviceIfNeeded();
	
	friend class D3D9Texture;
	[[nodiscard]] bool beginSceneIfNeeded();
	void applyDefaultStates();
	void applyTransforms();
	void applyFog();
	[[nodiscard]] bool beginWorldDraw();
	void resetSwapchain();
	[[nodiscard]] const char * getGraphicsApiName() const override { return "DirectX 9"; }
	
	IDirect3D9 * m_d3d = nullptr;
	IDirect3DDevice9 * m_device = nullptr;
	void * m_hwnd = nullptr;
	int m_width = 0;
	int m_height = 0;
	
	Rect m_viewport;
	Rect m_scissor;
	glm::mat4x4 m_view = glm::mat4x4(1.f);
	glm::mat4x4 m_proj = glm::mat4x4(1.f);
	Color m_fogColor;
	float m_fogStart = 0.f;
	float m_fogEnd = 1.f;
	float m_anisotropy = 1.f;
	float m_maxAnisotropyCap = 16.f;
	int m_vsync = 1;
	bool m_antialiasing = true;
	FillMode m_fillMode = FillSolid;
	bool m_inScene = false;
	bool m_deviceLost = false;
	bool m_requirePow2Textures = false;
	unsigned m_maxTextureSize = 4096;
	std::vector<D3D9Texture *> m_liveTextures;
	RenderState m_appliedState;
	bool m_appliedStateValid = false;
};

#endif // ARX_HAVE_D3D9

#endif // ARX_GRAPHICS_D3D9_D3D9RENDERER_H
