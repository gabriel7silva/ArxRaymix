# Contribuição

Inglês é o idioma padrão de ficheiros, pastas, comentários e documentação. O português fica em [`docs/pt-BR/`](README.md).

Inglês: [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## Onde está o quê

| Caminho | Função |
|---|---|
| `arx/src/graphics/d3d12/` | Backend raster Direct3D 12 (padrão no Windows) |
| `arx/src/graphics/dxr/` | DXR híbrido (AO, sombras, GI, reflexos) e Streamline (DLSS, FG, RR) |
| `arx/src/graphics/d3d9/` | Backend raster Direct3D 9 (fallback) |
| `arx/src/graphics/GlobalFog.{h,cpp}` | Plano longe do seletor Distância do render (`fog=`) |
| `arx/src/window/SDL2Window.cpp` | Escolhe o backend e cria o dispositivo |
| `arx/src/gui/MainMenu.cpp` | Páginas Vídeo, Render e Ray tracing |
| `arx/src/core/Config.{h,cpp}` | `config.video.renderer` e as chaves `dxr_*` |
| `scripts/run-d3d12.ps1` | Lança DirectX 12 |
| `scripts/run-d3d9.ps1` | Lança DirectX 9 |
| `scripts/fetch-streamline.ps1` | Obtém o SDK NVIDIA Streamline uma vez |
| `scripts/Find-ArxFatalis.ps1` | Procura Steam / GOG, sem caminhos fixos |

`arx/src/graphics/remix/` é leftover e não entra no binário de produto. Não começar path tracing de vários saltos (Phase 6 arquivada).

## Compilar e correr

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo --target arx --parallel
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

`-DataDir '<caminho do Arx Fatalis>'` se a deteção falhar. Log: `runtime/user/arx.log`. Saia pelo menu.

## Privacidade

O repositório é público. Nunca commitar:

- Pastas de utilizador
- Letras de unidade do clone local
- `runtime/user/`
- `data.dirs` gerado
- Scripts pessoais que apontam para kits de GPU ou Downloads

## Estilo

Seguir o código em volta: tabs, chavetas existentes, comentários que explicam *porquê*. Fontes D3D12 compilam fora do unity blob. Sem RTTI (`/GR-`); `static_cast`, não `ComPtr` da WRL.
