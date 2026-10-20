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
  src/ipc_min.cpp
  src/guest_placement.cpp
  src/gpu/ir.cpp
  src/gpu/maxwell_isa.cpp
  src/gpu/maxwell_program.cpp
  src/gpu/maxwell_frontend.cpp
  src/gpu/glsl_backend.cpp
  src/gpu/maxwell_translator.cpp
  src/patcher.cpp
  src/native_engine.cpp
  src/g610_call_guest.S
)
target_include_directories(g610_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}                # `vendor/eden/core/...`
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party
)
target_compile_definitions(g610_core PUBLIC ANKERL_UNORDERED_DENSE_STD_MODULE=1)
target_link_libraries(g610_core PUBLIC Vulkan::Vulkan SDL2::SDL2 fmt::fmt pthread)

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
  vendor/eden/core/hle/service/apm/apm.cpp
  vendor/eden/core/hle/service/apm/apm_controller.cpp
  vendor/eden/core/hle/service/apm/apm_interface.cpp
  vendor/eden/core/hle/service/hle_ipc.cpp
  vendor/eden/core/hle/service/lm/lm.cpp
  vendor/eden/core/hle/service/os/multi_wait.cpp
  vendor/eden/core/hle/service/os/multi_wait_holder.cpp
  vendor/eden/core/hle/service/os/mutex.cpp
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

# Teste do programa multi-modulo. Precisa de um ExeFS real, por isso nao e'
# um add_test sem argumentos: corre contra os ROMs do utilizador.
add_executable(g610_program tests/test_program.cpp)
target_link_libraries(g610_program PRIVATE g610_core)

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
