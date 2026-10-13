# Third-party notices

Arx Raymix is GPLv3 (see [LICENSE](LICENSE)). The release zip also contains, or is built with, the
components below. Each keeps its own license; the full texts are in the zip's `licenses\` folder,
and this file is copied there as `README.txt`.

| Component | Used for | License | In the zip |
|---|---|---|---|
| [Arx Libertatis](https://arx-libertatis.org/) | The engine Arx Raymix is built on | GPLv3 | `LICENSE`, `AUTHORS` |
| Arx Fatalis source code (Arkane Studios) | Original game code, released by Arkane | GPLv3 | `LICENSE` |
| [SDL2](https://www.libsdl.org/) | Window, input | zlib | `SDL2.dll`, `licenses\SDL2.txt` |
| [OpenAL Soft](https://openal-soft.org/) | Audio | GNU LGPL 2 | `OpenAL32.dll`, `licenses\OpenAL-Soft.txt` |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Compiling the D3D12 / DXR shaders | Windows SDK redistributable terms | `dxcompiler.dll`, `dxil.dll`, `licenses\DirectXShaderCompiler.txt` |
| [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline), DLSS, Reflex | DLSS Super Resolution, Ray Reconstruction, Frame Generation, Reflex | NVIDIA license, **not GPLv3** | `sl.*.dll`, `nvngx_*.dll`, `NvLowLatencyVk.dll`, `licenses\NVIDIA-Streamline\` |
| [FreeType](https://freetype.org/) | Font rendering (linked in) | FreeType License (FTL) | `licenses\FreeType.txt` |
| [zlib](https://zlib.net/) | Compression (linked in; the launcher uses it to unpack updates) | zlib | `licenses\zlib.txt` |
| [Boost](https://www.boost.org/) | Headers (linked in) | Boost Software License 1.0 | `licenses\Boost.txt` |
| [GLM](https://github.com/g-truc/glm) | Math headers (linked in) | MIT / Happy Bunny | `licenses\GLM.txt` |
| [libepoxy](https://github.com/anholt/libepoxy) | OpenGL loader (linked in) | MIT | `licenses\libepoxy.txt` |
| [fast_float](https://github.com/fastfloat/fast_float) | Number parsing (linked in) | Apache 2.0 / MIT | `licenses\fast_float.txt` |
| [stb](https://github.com/nothings/stb) | Image loading (linked in) | MIT / public domain | `licenses\stb.txt` |
| [DejaVu fonts](https://dejavu-fonts.github.io/) | Fallback font | Bitstream Vera / DejaVu license | `licenses\DejaVu-fonts.txt` |

NVIDIA, DLSS, GeForce RTX and Reflex are trademarks and/or registered trademarks of NVIDIA
Corporation. The NVIDIA binaries are redistributed unmodified under NVIDIA's terms.

**Not in the zip:** Arx Fatalis game data (`*.pak`, speech, videos). It stays © Arkane Studios /
ZeniMax Media / Bethesda Softworks; players use their own Steam or GOG copy.

Arx Raymix is not affiliated with or endorsed by Arkane Studios, ZeniMax Media, Bethesda
Softworks or NVIDIA.
