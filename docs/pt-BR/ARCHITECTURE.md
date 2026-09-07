# Arquitetura

Como os backends raster do Windows estão ligados, e como a camada de ray tracing se apoia em um deles. Se um comentário no código e este texto discordarem, o código vence.

Esta é a visão geral. O [`docs/maintenance/`](../maintenance/README.md) é a referência de trabalho para quem altera o código, e está somente em inglês.

Versão em inglês: [`../ARCHITECTURE.md`](../ARCHITECTURE.md).

## Uma janela, dois backends

No Windows, `SDL2Window` tem um único HWND e constrói **um** renderer por processo:

| Backend | Fontes | Quando é usado |
|---|---|---|
| Direct3D 12 / DirectX 12 | `arx/src/graphics/d3d12/` | Padrão (`auto`, `Direct3D 12`, `DirectX 12`, `d3d12`, `dx12`) |
| Direct3D 9 / DirectX 9 | `arx/src/graphics/d3d9/` | Pedido explícito, ou falha do `createDevice` em D3D12 |

```text
ARX_RENDERER  →  vale só neste processo
cfg.ini [video] renderer=  →  caso contrário
"auto"  →  Direct3D 12
```

O dispositivo nasce no arranque. Trocar a API em **Opções → Vídeo** grava `config.video.renderer` e salva o `cfg.ini`. **Não** destrói nem recria o dispositivo. A próxima inicialização lê o valor salvo.

`Renderer::getGraphicsApiName()` é o nome da sessão no menu (`DirectX 12` ou `DirectX 9`).

## Construção

1. Escolher `D3D12Renderer` ou `D3D9Renderer` pelas regras acima.
2. Criar a janela SDL e obter o HWND.
3. Chamar `createDevice` no backend escolhido.
4. Se o D3D12 falhar e o D3D9 estiver compilado, transferir os listeners para um `D3D9Renderer` novo.

OpenGL é o caminho fora do Windows. No Windows não é escolha em tempo de execução.

## Persistência

| Onde | Papel |
|---|---|
| `cfg.ini` → `[video] renderer=` | Escolha salva |
| `ARX_RENDERER` | Sobrescrita por processo (scripts de lançamento) |
| Seletor em Vídeo | Grava e salva; avisa para reiniciar se diferir da API ao vivo |

`runtime/user/` é local. Não faz parte da árvore pública.

## A camada de ray tracing

`arx/src/graphics/dxr/` acrescenta oclusão ambiente traçada, sombras, um salto de luz indireta e reflexos de água / metal sobre o raster D3D12 já pronto. O NVIDIA Streamline (`D3D12Streamline`) acrescenta DLSS, Frame Generation e Reconstrução de raios experimental. É uma camada, não uma substituição: o mundo é rasterizado primeiro, e desligar todos os efeitos deixa o raster intacto. Path tracing de vários saltos está arquivado.

Compila junto com o backend D3D12 e é alcançada por um único método opcional na interface do renderizador, cujo padrão é não fazer nada — então os outros backends não precisam saber dela. A chamada fica depois do mundo desenhado e antes de partículas, clarões e HUD, para que efeitos aditivos não sejam multiplicados pelo sombreamento do mundo.

| | |
|---|---|
| Receptores | O buffer de profundidade, um por pixel de tela |
| Bloqueadores | Salas próximas, em cache e reconstruídas na troca de sala, mais as entidades em cena a cada quadro |
| Luzes | Um conjunto limitado escolhido por quadro entre as luzes acesas do nível, com entrada e saída suavizadas para que a troca nunca apareça como um salto |
| Estabilidade | Cada resultado passa por *denoise* e é misturado com o quadro anterior, reprojetado pela câmera anterior |
| Ajustes | `rtao`, `dxr_shadows`, `dxr_gi`, reflexos, denoise, `dxr_distance` no `cfg.ini`, aplicados na hora. DLSS / FG / RR são `dxr_dlss`, `dxr_fg`, `dxr_rr` |

Cada etapa degrada para o raster puro, nunca para um quadro quebrado: hardware sem suporte, compilador de shader ausente, shader que falha ao compilar ou cena sem nada a traçar interrompem o passe e deixam a imagem rasterizada como está. O log sempre diz qual etapa parou.

Phases 1–4 estão feitas. Phase 5 é DLSS + Frame Generation; Reconstrução de raios é experimental (o denoiser caseiro fica se o NGX falhar ao criar). Phase 6 (path tracing de vários saltos) está **arquivada**.

Para saber *por que* um valor é o que é, leia [`arx/src/graphics/dxr/README.md`](../../arx/src/graphics/dxr/README.md). Para onde as coisas ficam e o que quebra ao mudá-las, leia o [`docs/maintenance/`](../maintenance/README.md). As chaves estão em [`CONFIGURATION.md`](CONFIGURATION.md).

## Fontes Remix antigas

`arx/src/graphics/remix/` e `arx/third_party/rtx-remix/` restam de um experimento. `ARX_HAVE_RTX_REMIX` não está ligado. Não é o caminho de lançamento.
