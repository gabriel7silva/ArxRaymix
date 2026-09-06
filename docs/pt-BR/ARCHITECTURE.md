# Arquitetura

Como os backends raster do Windows estão ligados. Se um comentário no código e este texto discordarem, o código vence.

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

AO por raios (opção A) e sombras DXR posteriores estão em [`arx/src/graphics/dxr/`](../../arx/src/graphics/dxr/README.md). O RTAO compila com o D3D12 e liga-se em **Opções → Ray tracing**.

## Fontes Remix antigas

`arx/src/graphics/remix/` e `arx/third_party/rtx-remix/` restam de um experimento. `ARX_HAVE_RTX_REMIX` não está ligado. Não é o caminho de lançamento.
