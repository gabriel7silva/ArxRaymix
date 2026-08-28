/*
 * Arx Remaster — dump PAK textures to <user>/remix-textures for Remix.
 *
 * Remix CreateMaterial only accepts file paths, not in-memory pixels, and its
 * asset loader only accepts DDS:
 *
 *   err: Unsupported image file format, use the RTX-Remix toolkit and ingest
 *        the following asset: ...bmp
 *
 * A material whose albedo fails to load renders black, which is exactly what
 * the preview window showed while the log reported healthy textured surfaces.
 * So we write uncompressed BGRA8 DDS ourselves instead of Image::save()'s BMP.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixTextures.h"

#if ARX_HAVE_RTX_REMIX

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

#include "core/Config.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/image/Image.h"
#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixConvert.h"
#include "graphics/texture/Texture.h"
#include "io/fs/FilePath.h"
#include "io/fs/FileStream.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"
#include "io/resource/ResourcePath.h"
#include "platform/WindowsUtils.h"

namespace remix {
namespace {

struct CachedMaterial {
	remixapi_MaterialHandle handle = nullptr;
	std::wstring albedoPath;
	std::wstring normalPath;
	std::wstring heightPath;
	bool opaqueUi = false;
};

std::map<TextureContainer *, CachedMaterial> g_materials;
std::map<Texture *, CachedMaterial> g_uiMaterials;

//! UI textures whose pixels changed since their material was built.
std::set<Texture *> g_dirtyUiTextures;

/*!
 * Dump revision per UI texture.
 *
 * Remix caches assets by file path, so rewriting the same .dds is not enough to
 * make it pick up new glyphs: the revision goes into both the file name and the
 * material hash so the runtime sees a genuinely new asset.
 */
std::map<Texture *, unsigned> g_uiRevisions;

//! Sobel gain for the derived normal maps. High enough that masonry joints read,
//! low enough that texture noise does not turn into gravel.
constexpr float kNormalStrength = 6.f;

/*!
 * Emissive gain for UI materials.
 *
 * WORLD_UI instances are composited rather than lit, so this mostly matters as a
 * fallback. It is the knob to turn if menus read washed out (lower) or too dim
 * (raise); scene lights for comparison run at radiance 4 to 22.
 */
constexpr float kUiEmissiveIntensity = 1.f;

std::string sanitizeStem(std::string name) {
	for(char & c : name) {
		if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
			c = '_';
		}
	}
	const size_t dot = name.find_last_of('.');
	if(dot != std::string::npos && dot > 0) {
		name.resize(dot);
	}
	if(name.empty()) {
		name = "unnamed";
	}
	return name;
}

uint64_t materialHash(TextureContainer * tex) {
	const uint64_t ptr = uint64_t(reinterpret_cast<uintptr_t>(tex));
	return ptr ? ptr : 1ull;
}

void writeU32LE(fs::ofstream & f, uint32_t x) {
	const char buffer[4] = { char(uint8_t(x)), char(uint8_t(x >> 8)),
	                         char(uint8_t(x >> 16)), char(uint8_t(x >> 24)) };
	f.write(buffer, 4);
}

//! Expand any Image format to straight BGRA8, top-down (DDS row order).
std::vector<unsigned char> toBgra8(const Image & image) {
	
	const size_t width = image.getWidth();
	const size_t height = image.getHeight();
	const size_t channels = image.getNumChannels();
	const unsigned char * src = image.getData();
	
	std::vector<unsigned char> out(width * height * 4);
	
	for(size_t i = 0; i < width * height; i++) {
		const unsigned char * d = src + i * channels;
		unsigned char * o = &out[i * 4];
		switch(image.getFormat()) {
			case Image::Format_L8:       o[0] = o[1] = o[2] = d[0]; o[3] = 0xff; break;
			case Image::Format_A8:       o[0] = o[1] = o[2] = 0xff; o[3] = d[0]; break;
			case Image::Format_L8A8:     o[0] = o[1] = o[2] = d[0]; o[3] = d[1]; break;
			case Image::Format_R8G8B8:   o[0] = d[2]; o[1] = d[1]; o[2] = d[0]; o[3] = 0xff; break;
			case Image::Format_B8G8R8:   o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; o[3] = 0xff; break;
			case Image::Format_R8G8B8A8: o[0] = d[2]; o[1] = d[1]; o[2] = d[0]; o[3] = d[3]; break;
			case Image::Format_B8G8R8A8: o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; o[3] = d[3]; break;
			default:                     o[0] = o[1] = o[2] = o[3] = 0xff; break;
		}
	}
	
	return out;
}

//! Box-filter one BGRA level down to the next. Halves each axis, minimum 1.
std::vector<unsigned char> downsampleBgra8(const std::vector<unsigned char> & src,
                                           size_t width, size_t height,
                                           size_t & outWidth, size_t & outHeight) {
	
	outWidth = std::max<size_t>(width / 2, 1);
	outHeight = std::max<size_t>(height / 2, 1);
	
	std::vector<unsigned char> out(outWidth * outHeight * 4);
	
	for(size_t y = 0; y < outHeight; y++) {
		const size_t y0 = std::min(y * 2, height - 1);
		const size_t y1 = std::min(y * 2 + 1, height - 1);
		for(size_t x = 0; x < outWidth; x++) {
			const size_t x0 = std::min(x * 2, width - 1);
			const size_t x1 = std::min(x * 2 + 1, width - 1);
			const unsigned char * a = &src[(y0 * width + x0) * 4];
			const unsigned char * b = &src[(y0 * width + x1) * 4];
			const unsigned char * c = &src[(y1 * width + x0) * 4];
			const unsigned char * d = &src[(y1 * width + x1) * 4];
			unsigned char * o = &out[(y * outWidth + x) * 4];
			for(int i = 0; i < 4; i++) {
				o[i] = (unsigned int(a[i]) + b[i] + c[i] + d[i] + 2) / 4;
			}
		}
	}
	
	return out;
}

/*!
 * Uncompressed BGRA8 DDS with a full mipmap chain.
 *
 * Without mipmaps the albedo aliases at distance, and that specular sparkle is
 * what reads as a plastic sheen once the path tracer is lighting it.
 *
 * Default is the legacy A8R8G8B8 header, which Remix loads without running the
 * asset through the toolkit. DebugSrgbTextures switches to a DX10 header with
 * B8G8R8A8_UNORM_SRGB, to see whether Remix has been treating the albedo as
 * linear; if it rejects the DX10 header, its own log says so.
 */
bool writeDds(const std::vector<unsigned char> & base, size_t width, size_t height,
              const fs::path & dest) {
	
	if(width == 0 || height == 0 || base.size() != width * height * 4) {
		return false;
	}
	
	std::vector<std::vector<unsigned char>> levels;
	std::vector<std::pair<size_t, size_t>> sizes;
	levels.push_back(base);
	sizes.emplace_back(width, height);
	while(sizes.back().first > 1 || sizes.back().second > 1) {
		size_t w = 0;
		size_t h = 0;
		levels.push_back(downsampleBgra8(levels.back(), sizes.back().first, sizes.back().second, w, h));
		sizes.emplace_back(w, h);
	}
	
	fs::ofstream f(dest, fs::fstream::out | fs::fstream::binary | fs::fstream::trunc);
	if(!f.is_open()) {
		return false;
	}
	
	const uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4;
	const uint32_t DDSD_PITCH = 0x8, DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000;
	const uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40;
	const uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
	const uint32_t DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91;
	const uint32_t DDS_DIMENSION_TEXTURE2D = 3;
	
	const bool dx10 = debugEnabled(DebugSrgbTextures);
	
	f.write("DDS ", 4);
	writeU32LE(f, 124);
	writeU32LE(f, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT
	             | DDSD_MIPMAPCOUNT);
	writeU32LE(f, uint32_t(height));
	writeU32LE(f, uint32_t(width));
	writeU32LE(f, uint32_t(width * 4)); // pitch
	writeU32LE(f, 0);                   // depth
	writeU32LE(f, uint32_t(levels.size()));
	for(int i = 0; i < 11; i++) {
		writeU32LE(f, 0);               // reserved1
	}
	// DDS_PIXELFORMAT
	writeU32LE(f, 32);
	if(dx10) {
		writeU32LE(f, DDPF_FOURCC);
		f.write("DX10", 4);
		writeU32LE(f, 0);               // bits per pixel, unused with DX10
		writeU32LE(f, 0);
		writeU32LE(f, 0);
		writeU32LE(f, 0);
		writeU32LE(f, 0);
	} else {
		writeU32LE(f, DDPF_ALPHAPIXELS | DDPF_RGB);
		writeU32LE(f, 0);               // fourCC
		writeU32LE(f, 32);              // bits per pixel
		writeU32LE(f, 0x00ff0000);      // red mask
		writeU32LE(f, 0x0000ff00);      // green mask
		writeU32LE(f, 0x000000ff);      // blue mask
		writeU32LE(f, 0xff000000);      // alpha mask
	}
	writeU32LE(f, DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP);
	writeU32LE(f, 0);
	writeU32LE(f, 0);
	writeU32LE(f, 0);
	writeU32LE(f, 0);                   // reserved2
	
	if(dx10) {
		// DDS_HEADER_DXT10
		writeU32LE(f, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
		writeU32LE(f, DDS_DIMENSION_TEXTURE2D);
		writeU32LE(f, 0);               // miscFlag
		writeU32LE(f, 1);               // arraySize
		writeU32LE(f, 0);               // miscFlags2
	}
	
	if(!f.good()) {
		return false;
	}
	
	for(const std::vector<unsigned char> & level : levels) {
		f.write(reinterpret_cast<const char *>(level.data()), std::streamsize(level.size()));
	}
	
	return f.good();
}

/*!
 * Height from albedo luminance.
 *
 * Luminance is not height - a dark stain is not a dent. But Arx ships no height
 * or normal maps at all, and on masonry, wood and cloth the dark parts really
 * are the recessed parts (mortar joints, grain, weave), so it buys most of the
 * relief for nothing.
 */
std::vector<float> deriveHeight(const std::vector<unsigned char> & bgra, size_t width, size_t height) {

	std::vector<float> out(width * height);

	for(size_t i = 0; i < width * height; i++) {
		const unsigned char * p = &bgra[i * 4];
		// Rec. 601 luma, and the buffer is BGRA.
		out[i] = (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.f;
	}

	return out;
}

//! Sobel the height field into a tangent-space normal map, BGRA, wrapping at the
//! edges because these textures tile.
std::vector<unsigned char> deriveNormalMap(const std::vector<float> & height,
                                           size_t width, size_t height_px, float strength) {

	std::vector<unsigned char> out(width * height_px * 4);

	auto at = [&](size_t x, size_t y) {
		return height[(y % height_px) * width + (x % width)];
	};

	const bool flipGreen = debugEnabled(DebugFlipNormalGreen);

	for(size_t y = 0; y < height_px; y++) {
		for(size_t x = 0; x < width; x++) {

			const size_t xm = (x + width - 1) % width;
			const size_t xp = (x + 1) % width;
			const size_t ym = (y + height_px - 1) % height_px;
			const size_t yp = (y + 1) % height_px;

			const float dx = (at(xp, ym) + 2.f * at(xp, y) + at(xp, yp))
			                 - (at(xm, ym) + 2.f * at(xm, y) + at(xm, yp));
			const float dy = (at(xm, yp) + 2.f * at(x, yp) + at(xp, yp))
			                 - (at(xm, ym) + 2.f * at(x, ym) + at(xp, ym));

			float nx = -dx * strength;
			float ny = -dy * strength;
			float nz = 1.f;
			const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			nx /= len;
			ny /= len;
			nz /= len;

			if(flipGreen) {
				ny = -ny;
			}

			unsigned char * o = &out[(y * width + x) * 4];
			o[0] = (unsigned char)(std::clamp(nz * 0.5f + 0.5f, 0.f, 1.f) * 255.f); // B
			o[1] = (unsigned char)(std::clamp(ny * 0.5f + 0.5f, 0.f, 1.f) * 255.f); // G
			o[2] = (unsigned char)(std::clamp(nx * 0.5f + 0.5f, 0.f, 1.f) * 255.f); // R
			o[3] = 255;
		}
	}

	return out;
}

std::vector<unsigned char> heightToBgra(const std::vector<float> & height, size_t width, size_t rows) {

	std::vector<unsigned char> out(width * rows * 4);

	for(size_t i = 0; i < width * rows; i++) {
		const unsigned char v = (unsigned char)(std::clamp(height[i], 0.f, 1.f) * 255.f);
		out[i * 4 + 0] = v;
		out[i * 4 + 1] = v;
		out[i * 4 + 2] = v;
		out[i * 4 + 3] = 255;
	}

	return out;
}

struct SurfaceClass {
	float roughness = 0.9f;
	float metallic = 0.f;
	float emissiveIntensity = 0.f;
	remixapi_Float3D emissiveColor = { 0.f, 0.f, 0.f };
	const char * name = "default";
};

/*!
 * Arx encodes the material class in the texture file name - [stone], [sand],
 * [fabric], (wood), [iron], [metal] - and the engine already reads [metal] that
 * way in Object.cpp. This is real game data, not a guess about what a surface
 * looks like.
 *
 * Order matters: names like "l1_prison_(stone)_metal01" and "(stone)_gridl02"
 * are metal grating whose bracket tag says stone, so metal is tested first.
 */
SurfaceClass classify(const TextureContainer & tex) {

	std::string name = tex.m_texName.string();
	std::transform(name.begin(), name.end(), name.begin(),
	               [](unsigned char c) { return char(std::tolower(c)); });

	auto has = [&](const char * needle) { return name.find(needle) != std::string::npos; };

	if((tex.userflags & POLY_GLOW) || has("fire") || has("flame")
	   || has("brazier") || has("lava")) {
		if(has("lava")) {
			return { 0.8f, 0.f, 6.f, { 1.0f, 0.3f, 0.05f }, "lava" };
		}
		return { 0.8f, 0.f, 2.0f, { 1.0f, 0.65f, 0.25f }, "fire/glow" };
	}

	if((tex.userflags & POLY_METAL) || has("[metal]") || has("[iron]") || has("[rusty]")
	   || has("metal") || has("grid") || has("chain")) {
		/*
		 * metallic = 1 turned the cell grating black, and that was right of the
		 * path tracer: a conductor has no diffuse lobe, so in a room lit by two
		 * point lights there is nothing for it to reflect. Arx textures are
		 * hand-painted diffuse, not PBR metal albedo, so a glossy dielectric is
		 * the honest match - the painting still reads and it picks up highlights.
		 */
		return { 0.55f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "metal" };
	}
	if(has("water")) {
		return { 0.05f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "water" };
	}
	if(has("[stone]") || has("(stone)") || has("[sand]") || has("[gravel]") || has("[earth]")) {
		return { 0.95f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "stone" };
	}
	if(has("[wood]") || has("(wood)")) {
		return { 0.8f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "wood" };
	}
	if(has("[fabric]") || has("(fabric)")) {
		return { 1.f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "fabric" };
	}
	if(has("[grass]") || has("[plant]")) {
		return { 0.95f, 0.f, 0.f, { 0.f, 0.f, 0.f }, "plant" };
	}

	return { };
}

/*!
 * Dump the albedo and, unless disabled, the normal and height maps derived from
 * it. All three go through the same mipmapped DDS writer.
 */
bool dumpTextures(TextureContainer * tex, const fs::path & albedo,
                  const fs::path & normal, const fs::path & heightMap) {
	
	const bool wantNormal = !debugEnabled(DebugNoNormalMaps);
	const bool wantHeight = wantNormal && debugEnabled(DebugParallax);
	
	if(fs::is_regular_file(albedo)
	   && (!wantNormal || fs::is_regular_file(normal))
	   && (!wantHeight || fs::is_regular_file(heightMap))) {
		return true;
	}
	
	res::path source;
	if(tex->m_pTexture && !tex->m_pTexture->getFileName().empty()) {
		source = tex->m_pTexture->getFileName();
	} else {
		source = tex->m_texName;
	}
	if(source.empty()) {
		return false;
	}
	
	Image image;
	if(!image.load(source) || !image.isValid()) {
		LogWarning << "Remix textures: failed to load " << source.string();
		return false;
	}

	if(!(tex->m_dwFlags & TextureContainer::NoColorKey) && source.ext() == ".bmp" && !image.hasAlpha()) {
		image.applyColorKeyToAlpha(Color::black, false);
	}
	
	const size_t width = image.getWidth();
	const size_t rows = image.getHeight();
	if(width == 0 || rows == 0 || image.getFormat() >= Image::Format_Unknown) {
		return false;
	}
	
	const std::vector<unsigned char> bgra = toBgra8(image);
	if(!writeDds(bgra, width, rows, albedo)) {
		LogWarning << "Remix textures: failed to write " << albedo.string();
		return false;
	}
	
	if(wantNormal) {
		const std::vector<float> heights = deriveHeight(bgra, width, rows);
		if(!writeDds(deriveNormalMap(heights, width, rows, kNormalStrength), width, rows, normal)) {
			LogWarning << "Remix textures: failed to write " << normal.string();
			return false;
		}
		if(wantHeight && !writeDds(heightToBgra(heights, width, rows), width, rows, heightMap)) {
			LogWarning << "Remix textures: failed to write " << heightMap.string();
			return false;
		}
	}
	
	LogInfo << "Remix textures: cached " << source.string() << " -> " << albedo.string();
	return true;
}

std::string textureNameLower(const TextureContainer & tex) {
	std::string name = tex.m_texName.string();
	std::transform(name.begin(), name.end(), name.begin(),
	               [](unsigned char c) { return char(std::tolower(c)); });
	return name;
}

/*!
 * Pixels for a UI texture.
 *
 * Texture::restore() only keeps m_image long enough to upload it and then drops
 * it for anything that came from a file, so getImage() is empty for every menu,
 * cursor and HUD texture - only the dynamically built font atlases still hold
 * data. Reload from the resource and redo the color key exactly like restore()
 * did, otherwise those materials get no albedo and the menu renders black.
 */
bool uiTexturePixels(Texture & tex, Image & out) {
	
	if(tex.getImage().isValid()) {
		out = tex.getImage();
		return true;
	}
	
	if(tex.getFileName().empty() || !out.load(tex.getFileName()) || !out.isValid()) {
		return false;
	}
	
	// hasColorKey() reports the flag against the *stored* format, which restore()
	// already set from the color-keyed image, so it stays true after the reload.
	if(tex.hasColorKey() && !out.hasAlpha()) {
		out.applyColorKeyToAlpha(Color::black, config.video.colorkeyAntialiasing);
	}
	
	return true;
}

} // namespace

bool isCutoutTexture(const TextureContainer * tex) {
	
	if(!tex) {
		return false;
	}
	
	const std::string name = textureNameLower(*tex);
	auto has = [&](const char * needle) { return name.find(needle) != std::string::npos; };
	
	// Only grating / portcullis. Bare "metal"/"door"/"bar" punched holes in solid geo.
	return has("grid") || has("grate") || has("portcul") || has("porticul")
	       || has("[iron]") || has("[rusty]");
}

remixapi_MaterialHandle materialFor(TextureContainer * tex, RemixApi & api) {
	
	if(!tex || !api.iface().CreateMaterial) {
		return nullptr;
	}
	
	const auto cached = g_materials.find(tex);
	if(cached != g_materials.end()) {
		return cached->second.handle;
	}
	
	const fs::path dir = fs::getUserDir() / "remix-textures";
	if(!fs::is_directory(dir) && !fs::create_directories(dir)) {
		LogWarning << "Remix textures: could not create " << dir.string();
		return nullptr;
	}
	
	// The suffix versions the dump: mipmaps and the sRGB variant change the file
	// contents, and dumpTextures() reuses whatever is already on disk.
	const std::string stem = sanitizeStem(tex->m_texName.string());
	const char * variant = debugEnabled(DebugSrgbTextures) ? ".v3srgb" : ".v3";
	const fs::path albedoDest = dir / (stem + variant + ".dds");
	const fs::path normalDest = dir / (stem + ".v3n.dds");
	const fs::path heightDest = dir / (stem + ".v3h.dds");
	if(!dumpTextures(tex, albedoDest, normalDest, heightDest)) {
		return nullptr;
	}
	
	CachedMaterial entry;
	entry.albedoPath = platform::WinPath(albedoDest).c_str();
	if(!debugEnabled(DebugNoNormalMaps)) {
		entry.normalPath = platform::WinPath(normalDest).c_str();
		if(debugEnabled(DebugParallax)) {
			entry.heightPath = platform::WinPath(heightDest).c_str();
		}
	}
	
	const SurfaceClass surface = classify(*tex);
	
	remixapi_MaterialInfoOpaqueEXT opaque {};
	opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	// Bit 256: drop the texture and force a colour no Arx texture has. Red room
	// means the material binds and albedoTexture is what fails; still white
	// means the material never reaches the surface at all.
	opaque.albedoConstant = debugEnabled(DebugMaterialNoTexture)
	                        ? remixapi_Float3D{ 1.f, 0.f, 0.f }
	                        : remixapi_Float3D{ 1.f, 1.f, 1.f };
	opaque.opacityConstant = 1.f;
	// One roughness for stone, cloth, wood and iron alike was the waxy look.
	// The class comes from the Arx texture name, so grating reflects and cloth
	// does not.
	opaque.roughnessConstant = surface.roughness;
	opaque.metallicConstant = surface.metallic;
	if(!entry.heightPath.empty()) {
		opaque.heightTexture = entry.heightPath.c_str();
		opaque.displaceIn = 0.02f;
	}
	/*
	 * TextureContainer::userflags POLY_TRANS is only water/spider_web (Object.cpp).
	 * Cell bars are POLY_TRANS on the face, with grid/metal in the texture name.
	 */
	const bool isCutout = isCutoutTexture(tex);
	if(isCutout) {
		opaque.alphaTestType = 4; // VK_COMPARE_OP_GREATER
		opaque.alphaReferenceValue = 64;
	} else {
		opaque.alphaTestType = 7; // VK_COMPARE_OP_ALWAYS
		opaque.alphaReferenceValue = 0;
	}
	if(debugEnabled(DebugAlphaTestNever)) {
		opaque.alphaTestType = 0; // bit 512 now restores the broken value for A/B
	}
	if(debugEnabled(DebugDrawCallAlphaState)) {
		opaque.useDrawCallAlphaState = TRUE;
	}
	
	remixapi_MaterialInfo info {};
	info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	info.pNext = &opaque;
	info.hash = materialHash(tex);
	info.albedoTexture = debugEnabled(DebugMaterialNoTexture) ? nullptr : entry.albedoPath.c_str();
	info.normalTexture = entry.normalPath.empty() ? nullptr : entry.normalPath.c_str();
	info.emissiveIntensity = surface.emissiveIntensity;
	info.emissiveColorConstant = surface.emissiveColor;
	info.spriteSheetRow = 1;
	info.spriteSheetCol = 1;
	info.filterMode = 1;
	info.wrapModeU = 1;
	info.wrapModeV = 1;
	
	const remixapi_ErrorCode status = api.iface().CreateMaterial(&info, &entry.handle);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogWarning << "Remix textures: CreateMaterial failed for " << tex->m_texName.string()
		           << " (" << RemixApi::errorString(status) << ')';
		return nullptr;
	}
	if(g_materials.size() < 32) {
		LogInfo << "Remix textures: " << tex->m_texName.string() << " class=" << surface.name
		        << " roughness=" << surface.roughness << " metallic=" << surface.metallic
		        << (surface.emissiveIntensity > 0.f ? " (emissive)" : "")
		        << (isCutout ? " (cutout alpha)" : "")
		        << (entry.normalPath.empty() ? " (no normal map)" : " + normal map");
	}
	
	auto inserted = g_materials.emplace(tex, std::move(entry));
	return inserted.first->second.handle;
}

remixapi_MaterialHandle materialForTexture(Texture * tex, RemixApi & api, bool forceOpaque) {
	
	if(!tex || !api.iface().CreateMaterial) {
		return nullptr;
	}
	
	bool noColorKey = false;
	for(TextureContainer * tc = GetTextureList(); tc; tc = tc->m_pNext) {
		if(tc->m_pTexture == tex) {
			noColorKey = bool(tc->m_dwFlags & TextureContainer::NoColorKey);
			break;
		}
	}
	/*
	 * Fullscreen logos / menu parchment / loading art are RGBA DDS but are
	 * drawn opaque (LoadUI NoColorKey + render2D().noBlend()). Alpha-test
	 * GREATER/128 punched those bitmaps into islands of surviving texels.
	 */
	const bool wantOpaque = forceOpaque || noColorKey || !tex->hasAlpha();
	
	const auto cached = g_uiMaterials.find(tex);
	if(cached != g_uiMaterials.end()) {
		if(cached->second.opaqueUi == wantOpaque) {
			return cached->second.handle;
		}
		if(cached->second.handle && api.iface().DestroyMaterial) {
			api.iface().DestroyMaterial(cached->second.handle);
		}
		g_uiMaterials.erase(cached);
	}
	
	// Failures are not cached: a one-frame dump miss must not paint the
	// texture black for the rest of the process.
	const fs::path dir = fs::getUserDir() / "remix-textures";
	if(!fs::is_directory(dir) && !fs::create_directories(dir)) {
		return nullptr;
	}
	
	// Textures without a file name are the font atlases, whose identity is just a
	// heap address. Never trust a dump from an earlier run for those.
	const bool dynamic = tex->getFileName().empty();
	const unsigned revision = g_uiRevisions[tex];
	std::string name = tex->getFileName().string();
	if(name.empty()) {
		name = "ui_texture_" + std::to_string(reinterpret_cast<uintptr_t>(tex));
	}
	std::string stem = sanitizeStem(name);
	if(revision != 0) {
		stem += ".r" + std::to_string(revision);
	}
	const fs::path albedoDest = dir / (stem + ".v3ui.dds");
	
	if(dynamic || !fs::is_regular_file(albedoDest)) {
		Image img;
		if(!uiTexturePixels(*tex, img)) {
			LogWarning << "Remix textures: no pixels for UI texture " << name;
			return nullptr;
		}
		const size_t width = img.getWidth();
		const size_t rows = img.getHeight();
		if(width == 0 || rows == 0 || !writeDds(toBgra8(img), width, rows, albedoDest)) {
			LogWarning << "Remix textures: failed to write UI dump " << albedoDest.string();
			return nullptr;
		}
	}
	
	CachedMaterial entry;
	entry.albedoPath = platform::WinPath(albedoDest).c_str();
	entry.opaqueUi = wantOpaque;
	
	// Taken from the Texture, not from the Image: the stored format survives even
	// when the pixels were dropped, so this also holds when the dump is reused.
	const bool alphaTested = !wantOpaque && tex->hasAlpha();
	
	remixapi_MaterialInfoOpaqueEXT opaque {};
	opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
	opaque.albedoConstant = { 1.f, 1.f, 1.f };
	opaque.opacityConstant = 1.f;
	opaque.roughnessConstant = 1.f;
	opaque.metallicConstant = 0.f;
	opaque.alphaTestType = alphaTested ? 4 : 7; // VK_COMPARE_OP_GREATER for alpha, ALWAYS for solid
	opaque.alphaReferenceValue = alphaTested ? 32 : 0;
	
	remixapi_MaterialInfo info {};
	info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
	info.pNext = &opaque;
	info.hash = 0xB2000000ull ^ uint64_t(reinterpret_cast<uintptr_t>(tex))
	            ^ (uint64_t(revision) << 40) ^ (wantOpaque ? 0x20ull : 0);
	info.albedoTexture = entry.albedoPath.c_str();
	/*
	 * Menus and loading screens have no map lights, so a purely reflective UI
	 * material renders black. Emission has to come from the texture: a constant
	 * white emissiveColorConstant ignores the albedo and turned every widget
	 * into a uniform glowing blob.
	 */
	info.emissiveTexture = entry.albedoPath.c_str();
	info.emissiveIntensity = kUiEmissiveIntensity;
	info.emissiveColorConstant = { 1.f, 1.f, 1.f };
	info.spriteSheetRow = 1;
	info.spriteSheetCol = 1;
	info.filterMode = 1;
	info.wrapModeU = 1;
	info.wrapModeV = 1;
	
	const remixapi_ErrorCode status = api.iface().CreateMaterial(&info, &entry.handle);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogWarning << "Remix textures: CreateMaterial failed for UI " << name
		           << " (" << RemixApi::errorString(status) << ')';
		return nullptr;
	}
	if(g_uiMaterials.size() < 24) {
		LogInfo << "Remix textures: UI " << name << ' ' << tex->getSize().x << 'x' << tex->getSize().y
		        << (alphaTested ? " (alpha cutout ref 32)" : " (opaque)");
	}
	
	auto inserted = g_uiMaterials.emplace(tex, std::move(entry));
	return inserted.first->second.handle;
}

void invalidateUiMaterial(Texture * tex) {
	
	// Only meaningful for a texture we already built a material for; the lookup
	// keeps startup uploads from queueing work that has nothing to rebuild.
	if(tex && g_uiMaterials.find(tex) != g_uiMaterials.end()) {
		g_dirtyUiTextures.insert(tex);
	}
}

std::vector<Texture *> takeDirtyUiTextures() {
	
	std::vector<Texture *> dirty(g_dirtyUiTextures.begin(), g_dirtyUiTextures.end());
	g_dirtyUiTextures.clear();
	return dirty;
}

void dropUiMaterial(Texture * tex, RemixApi & api) {
	
	const auto stale = g_uiMaterials.find(tex);
	if(stale == g_uiMaterials.end()) {
		return;
	}
	
	if(stale->second.handle && api.iface().DestroyMaterial) {
		api.iface().DestroyMaterial(stale->second.handle);
	}
	g_uiMaterials.erase(stale);
	g_uiRevisions[tex]++;
}

void destroyMaterials(RemixApi & api) {
	
	if(api.iface().DestroyMaterial) {
		for(auto & entry : g_materials) {
			if(entry.second.handle) {
				api.iface().DestroyMaterial(entry.second.handle);
			}
		}
		for(auto & entry : g_uiMaterials) {
			if(entry.second.handle) {
				api.iface().DestroyMaterial(entry.second.handle);
			}
		}
	}
	g_materials.clear();
	g_uiMaterials.clear();
	g_dirtyUiTextures.clear();
	g_uiRevisions.clear();
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
