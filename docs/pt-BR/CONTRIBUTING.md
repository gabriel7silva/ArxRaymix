# Contribuição

Inglês é o idioma padrão de ficheiros, pastas, comentários e documentação. O português fica em [`docs/pt-BR/`](README.md).

Inglês: [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## Onde está o quê

| Caminho | Função |
|---|---|
| `arx/src/graphics/d3d12/` | Backend raster Direct3D 12 (padrão no Windows) |
| `arx/src/graphics/d3d9/` | Backend raster Direct3D 9 (fallback) |
| `arx/src/window/SDL2Window.cpp` | Escolhe o backend e cria o dispositivo |
| `arx/src/gui/MainMenu.cpp` | Opções de vídeo: API atual, seletor, aviso de reinício |
| `arx/src/core/Config.{h,cpp}` | Persistência de `config.video.renderer` |
| `scripts/run-d3d12.ps1` | Lança DirectX 12 |
| `scripts/run-d3d9.ps1` | Lança DirectX 9 |
| `scripts/Find-ArxFatalis.ps1` | Procura Steam / GOG, sem caminhos fixos |

`arx/src/graphics/remix/` é leftover e não entra no binário de produto.

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
