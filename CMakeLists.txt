# EdenMaliG610 — motor nativo de Switch para RK3588
#
# Compila-se dentro do Orange Pi 5 Pro. Sem dependências de emulador:
# apenas Vulkan (PanVK) e a toolchain AArch64 nativa.

cmake_minimum_required(VERSION 3.20)
# ASM é necessário para a entrada no guest (src/g610_call_guest.S).
project(edenmalig610 LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Flags de otimização. -O3 e um alinhamento agressivo: o guest roda nativamente
# e o custo do host que o cerca é o que aparece no perfil.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter
                    -fno-omit-frame-pointer)

# A telemetria e o despachante de SVC capturam registos em handlers de sinais;
# sem isto o backtrace fica truncado e o diagnóstico perde-se.
add_compile_options(-fno-omit-frame-pointer -fasynchronous-unwind-tables)

# --- Vulkan (PanVK sobre Mali-G610) ----------------------------------------
find_package(Vulkan REQUIRED)
# SDL2: o input do guest passa por ele em vez de evdev directo, porque ha
# um mapeamento por guid ja configurado no sistema e porque o SDL trata do
# ruido e das diferencas entre dispositivos. Ver include/g610/input.hpp.
find_package(SDL2 REQUIRED)

# fmt: o motor de formatacao que o codigo vendorizado do Eden usa
# (`fmt::format_to` no `service.cpp`, nos `LOG_*` do `logging.h`). O Eden
# traz o fmt de sistema; trazemos o mesmo, em vez de vendorizar uma copia
# dele para dentro do projecto.
#
# **Compilado, nao header-only.** Com `FMT_HEADER_ONLY` cada unidade de
# traducao que inclui o `logging.h` geraria a sua propria copia das funcoes do
# fmt — e a ligacao daria `undefined reference to fmt::v11::vformat`, que e'
# o sintoma de duas copias que o linker nao consegue juntar.
find_package(fmt REQUIRED)

# O OpenSSL — **so' para o bloco AES**.
#
# O motor precisa de AES para ler o RomFS do jogo (o NCA esta' cifrado com
# AES-XTS e AES-CTR). O `libcrypto` e' a implementacao; o XTS e o CTR do
# Switch sao escritos por cima, porque **nao sao os de serie** (ver
# `include/g610/crypto/aes.hpp`).
#
# A alternativa seria escrever o AES de raiz — e nao vale a pena: nao e'
# onde esta' o valor do projecto, e o AES e' o alvo de uma historia infinita
# de bugs que ja foi escrita por outros.
find_package(OpenSSL REQUIRED)

# --- núcleo reutilizável -----------------------------------------------------
# Separado do executável para que os testes liguem as mesmas fontes e não uma
# cópia — um teste que passa contra uma cópia não garante nada sobre o
# binário que corre no ecrã.
add_library(g610_core STATIC
  src/telemetry.cpp
  src/nso_loader.cpp
  src/nso_arguments.cpp
  src/guest_memory.cpp
  src/guest_program.cpp
  src/input.cpp
  src/svc_dispatcher.cpp
  src/services/applet_oe.cpp
  src/services/binder.cpp
  src/services/csrng.cpp
  src/services/fsp_srv.cpp
  src/services/hid.cpp
  src/services/pl_u.cpp
  src/services/set.cpp
  src/services/shared_font.cpp
  # A fonte do sistema — os **dados** vem do upstream (embutidos em
  # codigo) e a **partilha** e' a nossa (`src/services/shared_font.cpp`).
  # Sao as 6 fontes do `SHARED_FONTS` do Eden; a `ext` entra duas vezes no
  # blob (os mesmos dados), como no upstream.
  vendor/eden/core/file_sys/system_archive/data/font_standard.cpp
  vendor/eden/core/file_sys/system_archive/data/font_chinese_simplified.cpp
  vendor/eden/core/file_sys/system_archive/data/font_extended_chinese_simplified.cpp
  vendor/eden/core/file_sys/system_archive/data/font_chinese_traditional.cpp
  vendor/eden/core/file_sys/system_archive/data/font_korean.cpp
  vendor/eden/core/file_sys/system_archive/data/font_nintendo_extended.cpp
  src/services/nvdrv.cpp
  src/services/time_u.cpp
  src/services/vi.cpp
  src/ipc_min.cpp
  src/guest_placement.cpp
  src/crypto/aes.cpp
  src/fs/romfs.cpp
  src/gpu/ir.cpp
  src/gpu/maxwell_isa.cpp
  src/gpu/maxwell_program.cpp
  src/gpu/maxwell_frontend.cpp
  src/gpu/glsl_backend.cpp
  src/gpu/maxwell_translator.cpp
  src/gpu/macro_mme.cpp
  src/gpu/pushbuffer.cpp
  src/gpu/rasterizer_g610.cpp
  src/gpu/draw_consumer.cpp
  src/patcher.cpp
  src/native_engine.cpp
  src/g610_call_guest.S
)
target_include_directories(g610_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}                # `vendor/eden/core/...`
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/eden/src # `video_core/...`, `core/...` do upstream
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/Vulkan-Headers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/VulkanMemoryAllocator/include
)

# --- o motor Maxwell do Eden (video_core) ----------------------------------
# O `video_core` do upstream e' o motor Maxwell 3D (que executa os comandos
# do push buffer) e o `renderer_vulkan` (que os traduz para Vulkan). Entra no
# MESMO alvo do `core/`, como o resto do Eden vendorizado: sao as duas metades
# do mesmo motor e separadas dariam ao linker um circulo.
#
# As fontes vem do `CMakeLists.txt` do upstream — extraidas por
# `tools/extrair_fontes_upstream.py` (a fonte da verdade e' o upstream).
#
# **Não entra no build**: o `video_core` do upstream está acoplado ao
# Eden inteiro — o `texture_cache` puxa o `bcdec`, o `GPU` puxa o
# `Core::System` completo, e os renderers puxam shaders gerados
# (glslang). A casa tem o **seu** tradutor ✓ — e agora com a ISA
# **real** ✓ (`g610/gpu/maxwell_comandos.hpp` ✓, extraída do upstream ✓).
#
# O que fica vendorizado ✓ (e é a referência): o `video_core` ✓, o
# `shader_recompiler` ✓ e o `core/` completo ✓.
include(cmake/ia_fontes.cmake)

# --- o shader_recompiler do upstream (NVIDIA GLSL -> SPIR-V) ----------------
#
# O `ShaderCache` da casa é um *placeholder* (copia o blob para dentro de
# um vector `spirv`). O upstream faz a conversão a sério ✓ — e traz o
# *frontend* Maxwell (o descompilador de SASS) ✓, que é a parte difícil.
#
# Depende do SPIRV-Tools ✓ e do SPIRV-Reflect ✓ (vendorizados ✓, com o
# layout irmão que o SPIRV-Tools exige ✓).
set(SPIRV-Headers_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/SPIRV-Headers)
set(SPIRV-Tools_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/SPIRV-Tools)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/third_party/SPIRV-Headers
  ${CMAKE_CURRENT_BINARY_DIR}/spirv-headers EXCLUDE_FROM_ALL)
add_subdirectory(vendor/third_party/SPIRV-Tools
  ${CMAKE_CURRENT_BINARY_DIR}/spirv-tools EXCLUDE_FROM_ALL)

# O `sirit` (o assembler SPIR-V) — o upstream usa-o pelo nome do
# pacote (`sirit::sirit`) e o `shader_recompiler` inclui `<sirit/sirit.h>`.
# O SPIRV-Reflect do Khronos serve para reflectir o SPIR-V (nao para o
# assemblar) — e o que o upstream tambem usa, ao lado.
add_library(sirit STATIC
  vendor/third_party/sirit/src/instructions/annotation.cpp
  vendor/third_party/sirit/src/instructions/arithmetic.cpp
  vendor/third_party/sirit/src/instructions/atomic.cpp
  vendor/third_party/sirit/src/instructions/barrier.cpp
  vendor/third_party/sirit/src/instructions/bit.cpp
  vendor/third_party/sirit/src/instructions/constant.cpp
  vendor/third_party/sirit/src/instructions/conversion.cpp
  vendor/third_party/sirit/src/instructions/debug.cpp
  vendor/third_party/sirit/src/instructions/derivatives.cpp
  vendor/third_party/sirit/src/instructions/extension.cpp
  vendor/third_party/sirit/src/instructions/flow.cpp
  vendor/third_party/sirit/src/instructions/function.cpp
  vendor/third_party/sirit/src/instructions/group.cpp
  vendor/third_party/sirit/src/instructions/image.cpp
  vendor/third_party/sirit/src/instructions/logical.cpp
  vendor/third_party/sirit/src/instructions/memory.cpp
  vendor/third_party/sirit/src/instructions/misc.cpp
  vendor/third_party/sirit/src/instructions/type.cpp
  vendor/third_party/sirit/src/sirit.cpp
  vendor/third_party/sirit/src/stream.cpp
)
target_include_directories(sirit PUBLIC
  vendor/third_party/sirit/include
  vendor/third_party/sirit/src)   # osIncludes de `stream.h` (o `src/` do sirit)
add_library(sirit::sirit ALIAS sirit)

add_library(spirv_reflect STATIC vendor/third_party/SPIRV-Reflect/spirv_reflect.cpp)
target_include_directories(spirv_reflect PUBLIC vendor/third_party/SPIRV-Reflect)

#
# **Não entra no build**: o `sirit` disponível (o `yuzu-mirror/sirit` ✓)
# tem a API antiga — o `PatchDeferredPhi` — e o upstream usa a nova
# (que devolve um par). O `shader_recompiler` fica vendorizado ✓ como
# referência ✓ (e é a via para a conversão NVIDIA GLSL -> SPIR-V ✓),
# mas precisa do `sirit` certo.
#
# target_sources(g610_core PRIVATE ${IA_FONTES_SHADER_RECOMPILER})
# target_link_libraries(g610_core PUBLIC SPIRV-Tools sirit spirv_reflect)

# --- o motor Maxwell do Eden (video_core) -----------------------------------
#
# Entra no MESMO alvo do `core/` ✓ — sao as duas metades do mesmo motor e
# separadas dariam um circulo ao linker ✓.
#
# **Sem os renderers** ✓: o motor fala com o renderizador pela
# `RasterizerInterface` ✓ (uma interface abstracta ✓), e a casa
# implementa-a (o `ValhallBackend` ✓ do desenho). O `renderer_vulkan` do
# upstream traz shaders gerados (glslang) e o PanVK — e nao e'
# preciso para o motor ✓.
#
# **E sem o resto do `video_core`** — medido (`/tmp/build.log`): os
# `.cpp` do upstream do `video_core` herdam o **kernel real**
# (`core/hle/kernel/kernel.h`), que a casa **nao tem** — a casa tem o
# `KernelCore` minimo (a ponte, `k_kernel_core_bridge.h`) porque o
# `kernel.cpp` do Eden precisa de `Core::Timing` e da `CpuManager` — a
# infra de emulacao, que nao e' o que esta casa e'.
#
# O que o upstream pede e' `KernelCore` completo — `GetMemoryManager`,
# `GetCurrentProcess`, o `KDynamicPageManager` — e a casa nao o tem.
#
# **Os CABECALHOS ficam** ✓ — e' o que a `RasterizerInterface` do upstream
# e' (`video_core/rasterizer_interface.h`), que o `RasterizerG610` da
# casa implementa ✓ ✓.
#
# **O upstream e' a REFERENCIA, nao a implementacao**: a leitura do push
# buffer e' a da casa (`src/gpu/pushbuffer.cpp` — 24 verificacoes
# ✓) e o tradutor dos comandos Maxwell tambem
# (`src/gpu/maxwell_translator.cpp`). O `dma_pusher.cpp` do upstream
# serve para conferir a semantica, nao para ser linkado.
target_include_directories(g610_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/eden/src
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/Vulkan-Headers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/SPIRV-Headers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/bcdec
)
target_compile_definitions(g610_core PUBLIC ANKERL_UNORDERED_DENSE_STD_MODULE=1)
target_link_libraries(g610_core PUBLIC Vulkan::Vulkan SDL2::SDL2 fmt::fmt pthread
  OpenSSL::Crypto)

# --- o Eden vendorizado ------------------------------------------------------
# A parte SISTEMÁTICA do OS do Eden: a camada de IPC e os objectos de porta e
# sessão de que ela depende. Ver vendor/eden/README.md para o inventário, para
# o que foi deixado de fora e para cada corte.
#
# Entra no MESMO `g610_core` e não numa biblioteca à parte, porque o `KernelCore`
# da ponte é o nosso `Core::System` e o `EdenSystem` é o nosso motor: as duas
# metades dependem uma da outra, e duas bibliotecas a dependerem uma da outra
# dariam ao linker um círculo para resolver em vez de uma ordem.
#
# A lista é explícita e não um glob, por duas razões: o README diz o que está
# aqui, e a lista tem de poder ser conferida contra ele. Um glob esconderia um
# ficheiro que alguém deitou lá sem querer — que foi exactamente como o
# escalonador entrou na primeira volta. E `tools/podar_vendor.py` serve para
# dizer o que é removível, não para remover o que é preciso.
set(IA_EDEN_SOURCES
  vendor/eden/common/assert.cpp
  vendor/eden/core/hle/kernel/k_auto_object.cpp
  vendor/eden/core/hle/kernel/k_auto_object_container.cpp
  vendor/eden/core/hle/kernel/k_client_port.cpp
  vendor/eden/core/hle/kernel/k_client_session.cpp
  vendor/eden/core/hle/kernel/k_event.cpp
  vendor/eden/core/hle/kernel/k_handle_table.cpp
  vendor/eden/core/hle/kernel/k_light_client_session.cpp
  vendor/eden/core/hle/kernel/k_light_server_session.cpp
  vendor/eden/core/hle/kernel/k_light_session.cpp
  vendor/eden/core/hle/kernel/k_object_name.cpp
  vendor/eden/core/hle/kernel/k_port.cpp
  vendor/eden/core/hle/kernel/k_readable_event.cpp
  vendor/eden/core/hle/kernel/k_resource_limit.cpp
  vendor/eden/core/hle/kernel/k_server_port.cpp
  vendor/eden/core/hle/kernel/k_server_session.cpp
  vendor/eden/core/hle/kernel/k_session.cpp
  vendor/eden/core/hle/kernel/k_session_request.cpp
  vendor/eden/core/hle/kernel/k_spin_lock.cpp
  vendor/eden/core/hle/service/aoc/addon_content_manager.cpp
  vendor/eden/core/hle/service/aoc/purchase_event_manager.cpp
  vendor/eden/core/hle/service/apm/apm.cpp
  vendor/eden/core/hle/service/apm/apm_controller.cpp
  vendor/eden/core/hle/service/apm/apm_interface.cpp
  vendor/eden/core/hle/service/hle_ipc.cpp
  vendor/eden/core/hle/service/kernel_helpers.cpp
  vendor/eden/core/hle/service/lm/lm.cpp
  vendor/eden/core/hle/service/os/event.cpp
  vendor/eden/core/hle/service/os/multi_wait.cpp
  vendor/eden/core/hle/service/os/multi_wait_holder.cpp
  vendor/eden/core/hle/service/os/mutex.cpp
  vendor/eden/core/hle/service/pctl/parental_control_service.cpp
  vendor/eden/core/hle/service/pctl/parental_control_service_factory.cpp
  vendor/eden/core/hle/service/pctl/pctl.cpp
  vendor/eden/core/hle/service/server_manager.cpp
  vendor/eden/core/hle/service/service.cpp
  vendor/eden/core/hle/service/sm/sm.cpp
  vendor/eden/core/hle/service/sm/sm_controller.cpp
)

# NAO vendorizado, e o porque:
#
#   core/hle/kernel/k_synchronization_object.cpp
#   core/hle/kernel/k_server_session.cpp
#
# O `KSynchronizationObject::Wait` -- a funcao que **dorme uma thread** --
# usa `KScopedSchedulerLockAndSleep`, que e' o escalonador. Nao e' um porto
# mecanico: e' a unica coisa do Eden que este motor **nao pode ter**, porque
# nao ha troca de contexto para dormir, e porque nao ha nada a acordar.
#
# Ver PROGRESSO.md, secao "A parede da espera".

# `src/eden_log.cpp` substitui `vendor/eden/common/logging.cpp`: as duas
# funções que o codigo vendorizado pede, a escrever no stderr em vez de num
# ficheiro. Ver a nota no topo desse ficheiro.
target_sources(g610_core PRIVATE
  src/eden_bridge.cpp
  src/eden_log.cpp
  src/eden_settings.cpp
  src/eden_synchronization_object.cpp
  ${IA_EDEN_SOURCES}
)

# --- alvo principal ----------------------------------------------------------
add_executable(g610 src/main.cpp)
target_link_libraries(g610 PRIVATE g610_core)

# --- testes ------------------------------------------------------------------
enable_testing()

# Guest sintetico em assembly. Os tamanhos de cada routine sao calculados
# durante a montagem (diferencas de etiquetas) e publicados como simbolos
# globais, para o teste saber ate onde copiar sem parsear o ELF.
add_library(guest_code OBJECT tests/guest_test.S)

# Os tamanhos das rotinas do guest sao extraidos do objecto: em C++ nao se
# pode subtrair enderecos de funcoes numa expressao constante, e parsear o ELF
# dentro do teste seria exagero para tres numeros.
set(IA_GUEST_OBJ ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/guest_code.dir/tests/guest_test.S.o)
set(IA_GUEST_SIZES ${CMAKE_CURRENT_BINARY_DIR}/generated/guest_sizes.h)
add_custom_command(
  OUTPUT ${IA_GUEST_SIZES}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND /bin/bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_guest_sizes.sh
          ${IA_GUEST_OBJ} ${IA_GUEST_SIZES}
  DEPENDS guest_code ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_guest_sizes.sh
  COMMENT "extrair tamanhos do guest"
  VERBATIM)
add_custom_target(guest_sizes DEPENDS ${IA_GUEST_SIZES})

add_executable(g610_tests tests/test_core.cpp)
target_link_libraries(g610_tests PRIVATE g610_core)

add_executable(g610_exec
  tests/test_exec.cpp
  tests/exec_mem.cpp
  $<TARGET_OBJECTS:guest_code>
)
target_link_libraries(g610_exec PRIVATE g610_core)
target_include_directories(g610_exec PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(g610_exec guest_sizes)
add_test(NAME exec COMMAND g610_exec)
add_test(NAME core COMMAND g610_tests)

# O leitor do push buffer e o tradutor Maxwell, com o formato e os IDs
# reais (do upstream). E' o teste que distingue "o push buffer esta mal
# lido" de "o shader e' esquisito".
add_executable(g610_pushbuffer tests/test_pushbuffer.cpp)
target_link_libraries(g610_pushbuffer PRIVATE g610_core)
add_test(NAME pushbuffer COMMAND g610_pushbuffer)

# O teste do caso REAL capturado (o instrumento `G610_GPFIFO_DUMP`):
# 8864 bytes de um submit de Pokemon Shield, com 29 desenhos por macro
# MME e `draw_begin`/`draw_end` AUSENTES. A Parte B hoje FALHA (o
# tradutor devolve 0) — e' a marca da causa demonstrada; fica verde
# quando as macros forem interpretadas.
add_executable(g610_gpfifo_replay tests/test_gpfifo_replay.cpp)
target_link_libraries(g610_gpfifo_replay PRIVATE g610_core)
target_compile_definitions(g610_gpfifo_replay PRIVATE
  GPFIFO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gpfifo_submit0.bin")
add_test(NAME gpfifo_replay COMMAND g610_gpfifo_replay)

# Teste do programa multi-modulo. Precisa de um ExeFS real, por isso nao e'
# um add_test sem argumentos: corre contra os ROMs do utilizador.
add_executable(g610_program tests/test_program.cpp)
target_link_libraries(g610_program PRIVATE g610_core)

# A medicao do custo do despacho. Nao e' um `add_test`: mede tempo, e
# um teste que mede tempo falha em maquinas carregadas.
add_executable(g610_bench_svc tests/bench_svc.cpp)
target_link_libraries(g610_bench_svc PRIVATE g610_core)

# --- testes da ISA Maxwell ---------------------------------------------------
add_executable(g610_maxwell_test tests/test_maxwell.cpp)
target_link_libraries(g610_maxwell_test PRIVATE g610_core)
add_test(NAME maxwell COMMAND g610_maxwell_test)

# Teste da cadeia: SASS sintetico -> IR -> GLSL -> SPIR-V.
add_executable(g610_translate_test tests/test_translate.cpp)
target_link_libraries(g610_translate_test PRIVATE g610_core)
add_test(NAME translate COMMAND g610_translate_test)

# Executa o SPIR-V na Mali-G610 de verdade e le o pixel de volta.
# Depende do g610_translate_test ter gerado /tmp/ia_min.frag.
find_library(VULKAN_LIB vulkan)
if(VULKAN_LIB)
  add_executable(g610_gpu_exec tests/test_gpu_exec.cpp)
  target_link_libraries(g610_gpu_exec PRIVATE g610_core ${VULKAN_LIB})
  add_test(NAME gpu_exec COMMAND g610_gpu_exec)
endif()

# Teste de input: prova que a pilha SDL2 + evdev + mapeamento funciona.
# Nao e' um teste do motor, e' um teste do INPUT.
add_executable(g610_input tests/probe_input.cpp)
target_link_libraries(g610_input PRIVATE g610_core)

# mostra: a PRIMEIRA IMAGEM NA TELA — SDL2 (sessao Wayland do board) +
# swapchain Vulkan no Mali-G610. Nao e' a cadeia do motor (essa e' o
# gpu_exec); e' o apresentador, e ha' prova de pixel via grim no
# cabecalho do ficheiro.
add_executable(g610_mostra tests/mostra.cpp)
target_link_libraries(g610_mostra PRIVATE g610_core)

# Teste do mapeamento de input. Nao precisa de hardware: a logica do A/B
# e' testada directamente, o que o torna um teste a serio em vez de um
# teste que so' passa com o DS4 ligado.
add_executable(g610_input_test tests/test_input.cpp)
target_link_libraries(g610_input_test PRIVATE g610_core)
add_test(NAME input COMMAND g610_input_test)

# Tracador de arranque: corre o rtld real e reporta os SVC que ele pede.
add_executable(g610_boot tests/boot_trace.cpp)
target_link_libraries(g610_boot PRIVATE g610_core)

# --- instalação --------------------------------------------------------------
install(TARGETS g610 RUNTIME DESTINATION bin)
install(DIRECTORY tools/ DESTINATION share/edenmalig610/tools)
