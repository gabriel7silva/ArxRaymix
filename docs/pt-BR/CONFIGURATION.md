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
| `data.dirs` ao lado do `arx.exe` | Gerado no build para achar o overlay de texto e `speech/portugues/`. Não commitar |

`--list-dirs` lista os diretórios de dados por prioridade.

## Distância do render

`cfg.ini` `[video] fog=` continua a ser a chave (0–10). Neste fork é o **plano longe**, não um seletor de densidade de neblina. Opções → Render → Distância do render. O valor 0 é cerca de uma célula mais uma parede de neblina (~1600 unidades Arx); 10 é o máximo antigo (~28000). Os pixels do mundo desaparecem na cor de neblina da zona de 40 % a 92 % dessa distância. A neblina segue o estado do `Renderer` (`getFog()`); não pode depender da flag `worldPass` do DLSS. O HUD fica sem neblina.

## Ray tracing e Streamline

Aplicados na hora. Cinzentos no menu, e ignorados no `cfg.ini`, quando o hardware não os corre.

| Chave | Menu | Valores |
|---|---|---|
| `dxr_preset` | Opções → Ray tracing → Preset | 0 Desligado, 1 Baixo, 2 Médio, 3 Alto, 4 Personalizado |
| `rtao` | Oclusão ambiente | 0–3 |
| `dxr_shadows` | Iluminação direta (sombras DXR) | 0–3 |
| `dxr_shadow_denoise` | Denoise direto | 0 Baixo, 1 Alto |
| `dxr_contact` | Sombras de contacto | 0 / 1 |
| `dxr_gi` | Iluminação indireta (um salto) | 0–3 |
| `dxr_gi_denoise` | Denoise indireto | 0–2 |
| `dxr_reflections` | Reflexos de metal | 0–3 |
| `dxr_trans_reflections` | Reflexos de água / transparentes | 0–3 |
| `dxr_transparency` | Casters recortados | 0 / 1 / 2 |
| `dxr_debris` | Adereços pequenos no TLAS | 0 / 1 |
| `dxr_distance` | Distância do ray tracing | 0 Baixo, 1 Médio, 2 Alto, 3 Ultra |
| `dxr_dlss` | Opções → Vídeo → Upscaling / modo DLSS | 0 Desligado, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance |
| `dxr_fg` | Opções → Vídeo → Frame Generation | 0 / 1 (2×). Só este seletor liga ou desliga o FG |
| `dxr_rr` | Opções → Ray tracing → Reconstrução de raios (experimental) | 0 / 1. O denoiser caseiro fica se o NGX falhar ao criar |

Os presets de RT não gravam `dxr_dlss`, `dxr_fg` nem `dxr_distance`. Eles põem `dxr_rr` a 0. `dxr_dlss` usa o esquema `dlss_schema=1`.

Iluminação indireta é um salto analítico. Não há chave de número de saltos nem de path tracing. A Phase 6 está arquivada.

## O que não commitar

- `runtime/user/cfg.ini`, `runtime/user/arx.log`, saves
- `data.dirs` (caminho absoluto de build)
- Caminhos da máquina, pasta de utilizador, lançadores pessoais de GPU
