# EdenMaliIA — motor nativo de Switch para RK3588
#
# Compila-se dentro do Orange Pi 5 Pro. Sem dependências de emulador:
# apenas Vulkan (PanVK) e a toolchain AArch64 nativa.

cmake_minimum_required(VERSION 3.20)
# ASM é necessário para a entrada no guest (src/ia_call_guest.S).
project(edenmaliia LANGUAGES CXX ASM)

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
# ruido e das diferencas entre dispositivos. Ver include/ia/input.hpp.
find_package(SDL2 REQUIRED)

# --- núcleo reutilizável -----------------------------------------------------
# Separado do executável para que os testes liguem as mesmas fontes e não uma
# cópia — um teste que passa contra uma cópia não garante nada sobre o
# binário que corre no ecrã.
add_library(ia_core STATIC
  src/telemetry.cpp
  src/nso_loader.cpp
  src/nso_arguments.cpp
  src/guest_memory.cpp
  src/guest_program.cpp
  src/input.cpp
  src/svc_dispatcher.cpp
  src/gpu/maxwell_translator.cpp
  src/patcher.cpp
  src/native_engine.cpp
  src/ia_call_guest.S
)
target_include_directories(ia_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ia_core PUBLIC Vulkan::Vulkan SDL2::SDL2 pthread)

# --- alvo principal ----------------------------------------------------------
add_executable(ia src/main.cpp)
target_link_libraries(ia PRIVATE ia_core)

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

add_executable(ia_tests tests/test_core.cpp)
target_link_libraries(ia_tests PRIVATE ia_core)

add_executable(ia_exec
  tests/test_exec.cpp
  tests/exec_mem.cpp
  $<TARGET_OBJECTS:guest_code>
)
target_link_libraries(ia_exec PRIVATE ia_core)
target_include_directories(ia_exec PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(ia_exec guest_sizes)
add_test(NAME exec COMMAND ia_exec)
add_test(NAME core COMMAND ia_tests)

# Teste do programa multi-modulo. Precisa de um ExeFS real, por isso nao e'
# um add_test sem argumentos: corre contra os ROMs do utilizador.
add_executable(ia_program tests/test_program.cpp)
target_link_libraries(ia_program PRIVATE ia_core)

# Teste de input: prova que a pilha SDL2 + evdev + mapeamento funciona.
# Nao e' um teste do motor, e' um teste do INPUT.
add_executable(ia_input tests/probe_input.cpp)
target_link_libraries(ia_input PRIVATE ia_core)

# Teste do mapeamento de input. Nao precisa de hardware: a logica do A/B
# e' testada directamente, o que o torna um teste a serio em vez de um
# teste que so' passa com o DS4 ligado.
add_executable(ia_input_test tests/test_input.cpp)
target_link_libraries(ia_input_test PRIVATE ia_core)
add_test(NAME input COMMAND ia_input_test)

# Tracador de arranque: corre o rtld real e reporta os SVC que ele pede.
add_executable(ia_boot tests/boot_trace.cpp)
target_link_libraries(ia_boot PRIVATE ia_core)

# --- instalação --------------------------------------------------------------
install(TARGETS ia RUNTIME DESTINATION bin)
install(DIRECTORY tools/ DESTINATION share/edenmaliia/tools)
