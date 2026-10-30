# Estado do Projeto — Apresentação na Tela

**Data:** 2026-10-30
**Estado:** Jogo a correr com 846066+ SVCs, comandos GPU a ser processados, camada de display criada, framebuffer em 0x9C800000 (1280x720)

## O que funciona

- Execução ARM64 nativa do guest
- 5 módulos NSO carregados (rtld, main, sdk, subsdk0, subsdk1)
- 24 serviços registrados (sm:, lm:, fsp-srv, nvdrv, vi:m, vi:s, vi:u, etc.)
- Filesystem: fsp-srv comandos, IStorage Read
- NVDRV: Initialize, Open, Ioctl, QueryEvent, SubmitGPFIFO (376 draw commands)
- VI: GetDisplayService, OpenLayer (camada=1, janela=0x9C800000, 3686400 bytes)
- Draw commands: total=376, draw_begin=1414, draw_end=1413

## O que falta

- **Apresentação na tela**: O loop de vsync em vi.cpp desenha um TRIÂNGULO de diagnóstico e escreve para g_janela_vi, sobrepondo o conteúdo real do jogo. Precisa apresentar os pixels reais do jogo via Vulkan/Wayland/HDMI.

## Plano de apresentação

1. Adicionar SDL2 + Vulkan swapchain ao emulador (main.cpp, antes de engine.run())
2. Modificar o loop de vsync em vi.cpp para ler pixels de g_janela_vi e apresentar via swapchain, em vez de desenhar triângulo
3. Verificar Pokemon Shield na tela HDMI

## Arquivos envolvidos

- src/main.cpp — inicialização de SDL2/Vulkan/swapchain
- src/services/vi.cpp — loop de vsync (modificar para apresentar conteúdo do jogo)
- include/g610/gpu/vulkan_backend.hpp — possivelmente adicionar present_pixels
