# Configuração em tempo de execução

O que o remaster Windows realmente lê. Ficheiros locais de `cfg.ini`, logs e pastas de utilizador não entram no Git.

Inglês: [`../CONFIGURATION.md`](../CONFIGURATION.md).

## API gráfica

O renderer é escolhido **uma vez**, quando a janela é criada.

| Fonte (a de cima ganha) | Valores |
|---|---|
| Ambiente `ARX_RENDERER` | `d3d12`, `dx12`, `Direct3D 12`, `DirectX 12`, `d3d9`, `dx9`, `Direct3D 9`, `DirectX 9` |
| `cfg.ini` `[video] renderer=` | `auto` (Direct3D 12), `Direct3D 12` / `DirectX 12`, `Direct3D 9` / `DirectX 9` / `D3D9` |
| **Opções → Vídeo** | DirectX 12 ou DirectX 9. Salvo na hora. Vale no próximo arranque |

A página de vídeo também mostra a API da **sessão atual**, por exemplo `Graphics API: DirectX 12`. Essa linha só muda depois de reiniciar.

**Aplicar** grava resolução e ecrã inteiro. Nunca troca Direct3D 9 ↔ Direct3D 12 no processo que já está a correr.

Os scripts de lançamento definem `ARX_RENDERER` só para aquele processo. Não reescrevem o `cfg.ini`.

## Diretórios

| Caminho | Função |
|---|---|
| `--data-dir` | Instalação do Arx Fatalis (`data.pak`) |
| `--user-dir` | Saves, `cfg.ini`, `arx.log`. Os scripts usam `runtime/user/` no repositório |
| `data.dirs` ao lado do `arx.exe` | Gerado no build. Não commitar |

`--list-dirs` lista os diretórios de dados por prioridade.

## O que não commitar

- `runtime/user/cfg.ini`, `runtime/user/arx.log`, saves
- `data.dirs` (caminho absoluto de build)
- Caminhos da máquina, pasta de utilizador, lançadores pessoais de GPU
