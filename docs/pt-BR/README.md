<p align="center">
  <a href="../../README.md">English</a> · <a href="README.md">Português (Brasil)</a>
</p>

# Arx Raymix

**Arx Raymix** é um remaster de *Arx Fatalis* para Windows. Mantém o motor Arx Libertatis e adiciona um backend raster **Direct3D 12**. **Direct3D 9** continua como fallback. Uma janela, um dispositivo, sem ray tracing.

Este repositório não distribui os dados do jogo. É preciso ter o próprio *Arx Fatalis* (Steam ou GOG).

A documentação em inglês é a oficial. Esta pasta é a tradução equivalente.

## Base: Arx Libertatis

Fork de [**Arx Libertatis**](https://arx-libertatis.org/), combinado com a árvore [ArxWindows](https://github.com/arx/ArxWindows) para compilar a partir de um único clone.

| | |
|---|---|
| Projeto upstream | [arx/ArxLibertatis](https://github.com/arx/ArxLibertatis) |
| Versão-base | **Arx Libertatis 1.3-dev** |
| Commit-base | `5b95e4c5ca9d583f1b11c085326979772645e0f3` |
| Dependências Windows | [arx/ArxWindows](https://github.com/arx/ArxWindows), em `libs/` |

O histórico upstream não vem neste repositório. [`UPSTREAM.md`](UPSTREAM.md) lista o que divergiu.

## Estado

Veja as barras em [`docs/assets/status.svg`](../assets/status.svg) e o [README em inglês](../../README.md).

- Direct3D 12 é o padrão. Se a criação do dispositivo falhar, a sessão cai para D3D9.
- Direct3D 9 é fallback de primeira classe.
- **Opções → Vídeo** escolhe DirectX 9 ou DirectX 12. A troca é salva e só vale no próximo arranque. A sessão atual mostra a API ativa.
- Scripts separados para cada backend: [`scripts/README.md`](../../scripts/README.md).

Não é um path tracer. Experiências antigas com RTX Remix não são o produto.

## Requisitos

| | |
|---|---|
| Sistema | Windows 10 ou 11, x64 |
| GPU | Direct3D 12 para o backend padrão; Direct3D 9 para o fallback |
| Jogo | Cópia própria de **Arx Fatalis** (Steam ou GOG) |
| Ferramentas | Visual Studio 2022 ou mais recente (C++ desktop), CMake 3.12+ |

## Compilação

```bash
git clone https://github.com/gabriel7silva/ArxRaymix.git
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo --target arx --parallel
```

No Windows os dois backends entram no binário. As dependências estão em `libs/`.

## Execução

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

`run-dx12.ps1` e `run-dx9.ps1` são os mesmos lançadores. Os scripts localizam o *Arx Fatalis* no registro do Steam e do GOG. Use `-DataDir '<caminho>'` para forçar o diretório (a pasta que contém `data.pak`).

| Flag ou variável | Significado |
|---|---|
| `--data-dir <caminho>` | Instalação do Arx Fatalis (`data.pak`) |
| `--user-dir <caminho>` | Saves, configuração e `arx.log` |
| `--loadlevel <n>` | Carrega um nível direto |
| `--list-dirs` | Lista os diretórios de dados em uso |
| `ARX_RENDERER` | `d3d12` / `dx12` / `DirectX 12` ou `d3d9` / `dx9` / `DirectX 9`. Sobrescreve o `cfg.ini` só neste processo |

No jogo: **Opções → Vídeo**. A primeira linha é a API da sessão (`Graphics API: DirectX 12` ou `Graphics API: DirectX 9`). O seletor grava a API da **próxima** inicialização e avisa quando é preciso reiniciar. **Aplicar** salva resolução e ecrã inteiro; não recria o dispositivo para trocar de API.

Saia pelo **menu**.

## Documentação

| Inglês | Português |
|---|---|
| [README](../../README.md) | [README](README.md) |
| [Arquitetura](../ARCHITECTURE.md) | [Arquitetura](ARCHITECTURE.md) |
| [Configuração](../CONFIGURATION.md) | [Configuração](CONFIGURATION.md) |
| [Contribuição](../CONTRIBUTING.md) | [Contribuição](CONTRIBUTING.md) |
| [Upstream](../UPSTREAM.md) | [Upstream](UPSTREAM.md) |
| [Scripts](../../scripts/README.md) | (os mesmos scripts) |

## Créditos e licença

Arkane Studios — *Arx Fatalis*, 2002. [Arx Libertatis](https://arx-libertatis.org/) — o motor. GPLv3, ver [LICENSE](../../LICENSE). Os dados do jogo não estão incluídos.
