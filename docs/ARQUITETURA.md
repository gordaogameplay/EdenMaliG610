# EdenMaliIA — execução nativa de ROMs de Switch no RK3588

## O que é

Um motor que carrega o programa de um título de Nintendo Switch e decide se
corre **nativamente** no Cortex-A76, sem JIT nem interpreter, com telemetria
embutida em todos os pontos de custo.

Não é um emulador pronto com configurações: é o motor de execução, escrito de
zero para que cada instrução do guest, cada SVC e cada comando de GPU seja
medido no local.

## Princípio: o que é nativo e o que é tradução

| Componente | Classificação |
|---|---|
| Código ARM64 da ROM | **Nativo** — executa tal e qual no A76 |
| `SVC`, `WFI`, traps | **Wrapper** — patch estático no carregamento |
| Serviços do Horizon (kernel objects, IPC, FS, ns/am/vi/bsd/hid) | **Wrapper** — C++ |
| Comandos GPU Maxwell | **Tradução obrigatória** — a PanVK consome Vulkan |
| Shaders Maxwell | **Tradução obrigatória** — GLSL → SPIR-V |

O código do guest não é traduzido. A GPU é inevitavelmente diferente: eliminar
a tradução exigiria um driver Mali que falasse o protocolo da NVIDIA.

## Estado de implementação

| Fase | Componente | Estado |
|---|---|---|
| 1 | Telemetria (frame, SVC, GPU, bottlenecks) | **feito, 51 testes** |
| 1 | Carregador NSO (LZ4, segmentos, NPDM) | **feito, validado no Pokémon Shield** |
| 1 | Despachante de SVC + portas nomeadas | **feito** |
| 1 | Extracção de comandos Maxwell | **feito** (tabela de opcodes a validar) |
| 1 | Cache de shaders com telemetria hit/miss | **feito** |
| 2 | Trampoline + handlers de sinal (execução do guest) | por fazer |
| 2 | Espaço de memória de 39 bits + fastmem | por fazer |
| 3 | Tradutor Maxwell → Vulkan real | esqueleto |
| 3 | Recompilador Maxwell → SPIR-V | ponto de integração |

### Porquê a fase 2 é a que falta

O despachante de SVC existe e mede, mas para o guest correr tem de haver:

1. **Patcher estático**: reescrever `SVC #imm` → `BRK #imm` na carga do
   programa, e `WFI`/`YIELD`/`SEV` para equivalentes do host.
2. **Trampoline**: um ponto de entrada/saída por trap, com /restauração de
   registos.
3. **Signal handlers** para faults de acesso e alinhamento do guest: o guest
   corre em modo de utilizador e um address inválido tem de ser tratado pelo
   motor, não pelo kernel.
4. **Page tables de 39 bits** com fastmem (writeback/writecombine) — sem isto
   o acesso à memória do guest custa mais do que o JIT poupava.

Isto é a mesma estrutura do backend `Nce` do Eden
(`src/core/arm/nce/arm_nce.s`, ~4 000 linhas), reimplementada de forma
independente.

## Resultados medidos neste tabuleiro

Programa: Pokémon Shield `[01008DB008C2C000][v0]`

```
NPDM  Flags0 = 0x07  →  Is64Bit=1, ProcessAddressSpace=3 (AddressSpace64Bit39)
       MainThreadPriority = 44, StackSize = 1 MB, nome "Application"
       → execução NATIVA (sem JIT): cumpre os requisitos

NSO   text   25 084 672 bytes (LZ4, descomprimido)
       rodata 12 057 840 bytes (LZ4, descomprimido)
       data    1 362 704 bytes (LZ4, descomprimido)
       bss     3 425 520 bytes
       imagem 41 930 736 bytes
       entry   0x7fff00000, dentro de .text  ✓
```

### Notas de formato que custaram tempo (e voltariam a custar)

- **NSO não é ELF.** É um cabeçalho de 0x100 bytes com três segmentos
  (`offset, location, size, alignment|bss_size`) em 0x10, e flags em 0x0C.
- **Flags0 do NPDM é um byte**, não um u32. Ler como u32 lê o campo vizinho e
  faz um jogo 39-bit parecer 32-bit — o que despista o motor para o JIT.
- **Segmentos comprimidos são blocos LZ4 crus**, sem cabeçalho de ficheiro
  "LZ4N". O primeiro byte é já o token da sequência.
- **`compressed_size` inclui o cabeçalho** do bloco e é o tamanho total em
  disco: no Pokémon Shield, `data_file_off + compressed_size[2]` dá
  exactamente o tamanho do ficheiro.

Cada um destes está fixado por teste em `tests/test_core.cpp` com os valores
reais lidos do ficheiro.

## Descoberta estrutural: onde estão os SVC

Medido nos cinco NSO do ExeFS do Pokémon Shield:

| Módulo | tamanho | SVC | MRS | hints | exclusivos |
|---|---:|---:|---:|---:|---:|
| `main` | 25,1 MB | **0** | 5 | 4816 | 12907 |
| `sdk` | 13,4 MB | **62** | — | — | — |
| `subsdk0` | 7,5 MB | 0 | — | — | — |
| `subsdk1` | 8,9 MB | 0 | — | — | — |
| `rtld` | 14 KB | **4** | — | — | — |

**O executável principal não chama o sistema.** Quem chama SVC é o SDK — as
bibliotecas que o jogo liga estáticamente — e o runtime de carregamento
(`rtld`), que precisa de SVC para obter memória e mapear o próprio processo
antes de o `main` arrancar.

Consequência para o motor: **o patcher tem de correr sobre todos os módulos,
não só sobre o `main`**. Correr só sobre o `main` dá 0 SVC e um guest que
morre na primeira chamada ao sistema sem explicação — exactamente o que o
`doctor` reporta hoje.

Isto também explica a ordem de arranque real:
`rtld` → (carrega `main`, `sdk`, `subsdk*`) → `main`. O `rtld` é quem pede
memória ao host, o que faz dele o ponto de entrada real — e não o `entry_point`
do `main`, que é o start normal do C++.

### Estado da fase 2

| Componente | Ficheiro | Estado |
|---|---|---|
| Descodificação de instruções | `include/ia/instructions.hpp` | feito, 25 asserts |
| Patcher estático | `src/patcher.cpp` | feito, validado no `main` real |
| Layout do contexto verificado | `include/ia/context_offsets.hpp` | feito, 16 asserts |
| Trampoline de entrada/saída | `src/ia_trampoline.S` | feito, compila |
| Handlers de trap | `src/native_engine.cpp` | feito, não testado em execução |
| Spinlock de ThreadParameters | `src/ia_trampoline.S` | feito |
| Page tables de 39 bits + fastmem | — | por fazer |

**95 testes, 0 falhas.**

O que falta para o guest correr: as page tables de 39 bits (o guest tem de
aceder a endereços acima de 4 GB, que o espaço de utilizador do host não
cobre) e ligar o motor ao carregamento multi-módulo.

## Telemetria

O ponto de partida do projecto. Sem instrumentação no caminho crítico não há
forma de saber porque é que um frame demora o que demora.

### Classificação de gargalo

Por frame, o tempo é decomposto em `cpu`, `gpu`, `translate`, `shader`,
`present`. O regime é identificado pelo sintoma mais específico primeiro:

| Regime | Sintoma | O que corrigir |
|---|---|---|
| `ShaderBound` | compilação > 50% do frame | cache de shaders, pré-compilação |
| `CpuBound` | sem draws, ou CPU > GPU | SVC caros, page faults, fastmem |
| `GpuBound` | GPU ocupa ≥ 70% do frame | fill rate, resolução, formatos |
| `PresentBound` | present > 50% do frame | vsync, frame pacing |

### Instrumentação

- **SVC**: histograma de 256 slots com contagem, total e máximo por serviço. O
  resumo aponta o mais caro — é isso que diz se vale a pena acelerar o
  serviço ou o desenho.
- **Frame**: histórico circular com a decomposição acima, exportado para CSV.
- **GPU**: draw calls, triângulos, shaders compilados, cache hits/misses,
  faults, device lost.
- **Consola**: uma linha por segundo (não por frame — a 60 fps enche o ecrã e
  esconde a informação).

Saída: `ia-telemetry/<tag>_<frames>.json` e `<tag>.csv`.

## Uso

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/ia probe                       # inventário do hardware
./build/ia doctor <main> [--npdm ...]  # diagnostico do programa
./build/ia run <main> [--npdm ...]     # execução com telemetria
./build/ia_tests                       # 51 testes
```

## Honestidade sobre o estado

O carregamento, a decisão nativo/JIT e toda a telemetria estão implementados e
validados contra um título real. **A execução do guest não está ligada** — falta
a fase 2 (trampoline e handlers). Dizer o contrário seria vender um motor que
ainda não corre o código do jogo.

---

## 9. Mecanismo de traps: medido, não suposto

A implementação inicial seguia o desenho do Eden — troca de contexto em
assembly dentro do handler. Antes de escrever esse assembly, medi o que o
kernel Linux de utilizador realmente honra. O resultado muda o desenho.

### Sonda

`tests/probe_ucontext.S` tem um guest mínimo:

```asm
guest_continue:
    mov     x0, #0
    brk     #3              ; trap
    mov     x0, #0x600D     ; marcador de passagem
    ret
```

O handler pode ou não avançar o `ucontext.pc` antes de regressar.

### Resultado no Orange Pi 5 Pro

```
[modo 1] escrever uc_mcontext.pc += 4
    RETORNOU x0 = 0x600d (continuou!)
    traps = 1, pc1 = 0xaaaad0bf0db4, pc2 = 0x0

[modo 2] PSTATE.SS (single-step)
    LIMITE DE TRAPS ATINGIDO (5)
    traps = 5, pc1 = 0xaaaad0bf0db4, pc2 = 0xaaaad0bf0db4
    => o PC NAO avancou
```

**O kernel honra a escrita do `ucontext.pc`.** Uma trap, o guest continua na
instrução seguinte, `x0 = 0x600D`.

A passagem de argumentos e resultado também funciona: o handler le `x0` do
`ucontext`, escreve o resultado no mesmo sítio, e o guest lê o valor novo.

### Consequência para o desenho

O motor **não precisa de trocar registos de hardware dentro do handler**.
Basta:

```
handler(SIGTRAP):
    pc  = ucontext->uc_mcontext.pc      ; aponta para o próprio BRK
    x0..x5 = ucontext->uc_mcontext.regs[0..5]
    despachar o serviço
    ucontext->uc_mcontext.regs[0] = resultado
    ucontext->uc_mcontext.pc += 4        ; saltar o BRK
    return                               ; o kernel retoma depois
```

Isto é ordens de grandeza mais barato do que salvar/restaurar 31 GPRs, 8
registos vectoriais, `sp`, `tpidr_el0`, `fpcr`/`fpsr` por chamada de serviço.
Um jogo faz milhares de SVC por segundo; a diferença mede-se.

### Porque o Eden faz mesmo assim

O Eden troca contexto porque precisa de **pré-emptão entre threads**: quando
outra thread quer o horário, tem de poder guardar o contexto do guest e correr
o host. O `tpidr_el0` como chave de TLS resolve esse caso.

Para um motor dethread único, ou com threads mapeadas uma a uma para threads
do host, a troca de contexto não é necessária — e é o que torna o `tpidr_el0`
uma optimização futura, não um requisito para arrancar.

### Nota sobre a sonda inicial

A primeira versão da sonda deu `timeout` (exit 124) sem qualquer saída. Não
era o kernel a ignorar o PC: era a sonda a entrar em ciclo infinito de traps
sem limite, e o `printf` nunca chegar a correr. Com um limite de traps e
`siglongjmp`, a mesma sonda dá a resposta em milissegundos. É a diferença entre
supor e medir — e a segunda é que deu a resposta útil.

---

## 10. Execução nativa validada

`tests/test_exec.cpp` corre um guest AArch64 real (`tests/guest_test.S`) através
do motor e verifica o ciclo completo. Resultado no Orange Pi 5 Pro:

```
[1] execução nativa com SVC
  ok: o patcher encontrou SVC no guest
  ok: os três SVC foram reescritos para BRK
  ok: houve pelo menos 3 traps
  ok: os três SVC foram despachados
  ok: ExitProcess terminou o guest
  ok: a última trap foi um SVC, não um fault
  ok: sem erro
    svc_calls=3 traps=3 faults=0 svc_ns=2626

[2] argumento e resultado
  ok: um SVC despachado
  ok: o argumento x0=123 chegou ao dispatcher de ExitProcess
    exit_code = 123

[3] recuperação de fault de memória
  ok: o fault foi registado uma vez
  ok: a paragem foi classificada como fault de guest
    fault de memoria do guest em 0xffffac4b6004 (sp=0xfffffce0af10)

13 verificações, 0 falhas
```

O guest não é um jogo: é o menor programa que exercita o caminho todo —
aritmética nativa, trap por SVC, argumentos que atravessam, resultado que
volta, execução que continua, `ExitProcess` que termina.

**109 verificações no total** (96 em `ia_tests`, 13 em `ia_exec`), 0 falhas.

### O mecanismo, em três passos

```
handler(SIGTRAP):
    pc     = ucontext->uc_mcontext.pc      // aponta para o próprio BRK
    x0..x5 = ucontext->uc_mcontext.regs[0..5]
    despachar o serviço
    ucontext->uc_mcontext.regs[0] = resultado
    ucontext->uc_mcontext.pc = pc + 4
    return                                  // o kernel retoma depois
```

---

## 11. Dois bugs que só a execução revelou

Nenhum dos dois aparecia em leitura de código. Ambos precisaram de o guest
correr para se manifestarem.

### 11.1 `BRK` com o bit 0 a 1 — o patcher produzia instruções ilegais

**Sintoma**: SIGILL no primeiro SVC, sem qualquer trap tratada. O handler de
sinais nunca era chamado, porque não havia trap — havia uma instrução que o
CPU não reconhecia.

**Causa**: `SVC` e `BRK` partilham a posição do immediate (bits 20-5), mas
**não** os bits 4-0:

```
SVC:  1101 0100 0000 imm16 00001     op2 = 0b00001
BRK:  1101 0010 0000 imm16 00000     op2 = 0b00000
```

`to_brk()` trocava só o bloco de assinatura e **preservava** os bits 4-0,
produzindo `d4200021` em vez de `d4200020`. O bit 0 a 1 torna a codificação
inválida.

**Correcção** — reconstruir a partir do immediate, não trocar a assinatura:

```cpp
constexpr std::uint32_t kImm16 = 0x001FFFE0u;  // bits 20-5
return (raw & kImm16) | kBrkSig;
```

**Encodings tirados do assembler real**, não calculados à mão:

```
svc #6  ->  d40000c1
brk #6  ->  d42000c0
```

O `static_assert` que faltava: `(to_brk() & 0x1F) == 0`. Três testes antigos
codificavam o bug errado (`== 0xD42000C1`) e passavam — o que é a lição:
um teste que verifica o que se supõe, em vez do que é, não apanha nada.

### 11.2 O stack protector do host, destruído pela troca de `sp`

**Sintoma**: `*** stack smashing detected ***: terminated`, sem qualquer
saída do programa — morria antes do primeiro `printf`.

**Causa**: `run()` é C++ compilado com `-fstack-protector-strong` (o default
do Ubuntu). `ia_call_guest` faz `mov sp, sp_do_guest`; se a execução voltar a
`run()` por um caminho que não restaure `sp`, o epílogo verifica o canário
numa stack que não é a sua.

**Porque não se resolve guardando o `sp`**: guardar em x19 não funciona — o
guest clobbera x19, e a verificação acontece no regresso, com o canário já
inalcançável. Reproduzido em todas as variantes.

**Solução**: tornar o regresso patológico também uma trap, para que passe
pelo mesmo mecanismo dos restantes. `ia_call_guest` empurra o endereço de
`ia_guest_returned` para a stack do guest:

```
guest faz `ret`        -> salta para ia_guest_returned
ia_guest_returned      -> brk #0xFFFD
handler                -> escreve sp = recovery_sp, pc = recovery_pc
recovery_pc            -> siglongjmp de volta a run()
```

O `siglongjmp` restaura o `sp` do host. O canário de `run()` nunca chega a
ser verificado sobre a stack errada.

**Custo**: uma subtração e um store por entrada no guest — **uma vez por
thread**, não por chamada de serviço. Face a guardar 31 GPRs por SVC, é
irrelevante.

**Nota de contrato**: `sp` tem de estar alinhado a 16 bytes e ter pelo menos
16 bytes de espaço acessível abaixo do ponteiro.

### 11.3 Estado do dispatcher a contaminar execuções seguintes

`exited_` ficava a `true` depois do primeiro `run()`. A execução seguinte
via-o e classificava um fault de memória como saída limpa — o relatório
mentia. `reset_run_state()` repõe `exited_` e `exit_code_` no início de cada
`run()`. As portas nomeadas **não** são repostas: são estado do sistema, e um
`SetHeapSize` seguido de `ConnectToNamedPort` tem de ver o heap definido.

Este é o tipo de bug que a telemetria existe para apanhar: um contador que
não regressa a zero entre frames denuncia-o antes de vir a ser um sintoma.

---

## 12. O que está medido, e o que ainda é suposição

| Afirmação | Estado |
|---|---|
| Guest AArch64 corre nativamente | **medido** — 3 SVC, 3 traps, exit 123 |
| O kernel honra `ucontext.pc` | **medido** — `probe_ucontext.cpp` |
| Argumentos e resultado atravessam o handler | **medido** — `x0=123` chega ao dispatcher |
| Fault de memória é recuperado e reportado | **medido** — pc e sp no erro |
| BRK vs. illegal instruction distinguem-se | **medido** — SIGTRAP vs. SIGILL |
| Patcher produz codificações legais | **medido** — encodings do assembler real |
| `rtld` → `main` → `sdk` arranca | **suposto** — a ordem vem da estrutura dos NSO |
| Page tables de 39 bits | **não implementado** |
| `tpidr_el0` por thread | **não implementado** — reservado |

---

## 13. Page tables: 36-bit vs 39-bit — a pergunta tem uma resposta medida

A dúvida era se o NCE tem duas opções de page tables (36 e 39 bits) e qual a
interessante. Verificado no código do Eden (`~/eden/src/`), que é a
implementação de referência:

### Não são "duas opções". É uma só, e a outra não é NCE.

`program_metadata.h`:

```cpp
enum class ProgramAddressSpaceType : u8 {
    Is32Bit       = 0,
    Is36Bit       = 1,
    Is32BitNoMap  = 2,
    Is39Bit       = 3,
};
```

E `deconstructed_rom_directory.cpp:185-189`:

```cpp
// Enable NCE only for applications with 39-bit address space.
const bool is_39bit =
    metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
Settings::SetNceEnabled(is_39bit);
```

**O NCE só existe para 39 bits.** Não é uma escolha entre dois esquemas: o
outro (36 bits) é o caminho HLE, com page table própria do emulador.

### Os dois regimes são arquitecturas diferentes, não variantes

| | 36-bit (HLE) | 39-bit (NCE) |
|---|---|---|
| Quem traduz | o emulador | o CPU |
| Page table | flat, do emulador | a do host |
| Onde vive o guest | memória virtual do emulador | memória real do processo |
| Custo de execução | Tabelas por acesso | Nenhum |

Para 39 bits, uma page table flat do emulador teria `2^(39-12) = 2^27`
entradas — **1 GiB só de tabelas**. É por isso que o NCE existe: não é uma
otimização, é a única forma de um espaço de 39 bits caber em 8 GB.

### E o NCE não usa page tables do guest — usa as do host

Este é o ponto que muda o plano. `EnableDirectMappedAddress()` na implementação
Linux é literalmente:

```cpp
void EnableDirectMappedAddress() {
    virtual_base = nullptr;
}
```

O guest vê a memória do emulador **no espaço de endereçamento do próprio
host**, sem tradução. Consequência prática: o nosso motor **não precisa de
page tables de 39 bits** — precisa que os endereços que o jogo calcula caiam
onde o host já os mapeou.

### O que o jogo realmente exige

Do `KProcess`:

```
39-bit:  code_address = 0x8000'0000 + aslr_space_offset   (ASLR ~512 GiB)
36-bit:  code_address = 0x0800'0000 + aslr_space_offset
```

E o comentário do Eden sobre 39-bit explica o truque:

> some (buggy) programs/libraries like skyline incorrectly depend on the
> existence of ASLR pages before the entry point, so we will adjust the load
> address to point to about 2 GiB into the ASLR region.

### Decisão

**39 bits, sem page tables próprias.** O que falta não é uma implementação de
tabelas — é `fastmem`: reservar o espaço de endereçamento que o jogo espera,
com a alocação de heap que o `rtld` pede. Quando esse mapeamento existir, o
`cpuopt_fastmem` deixa de ser uma optimização e passa a ser requisito — mas
o mecanismo é `mmap` do host, não tabelas que eu construa.

Isto é mais barato do que a pergunta supunha: não há árvore de 3 níveis para
implementar e-mostrar.

---

## 14. Programa multi-módulo: rtld → main → sdk

### O `main` não é o entry point

Isto já estava medido (o `main` do Pokémon Shield tem **zero** SVC) e agora
está implementado. Um programa de Switch é um ExeFS com vários NSO, e o
arranque é uma coreografia:

```
rtld  →  main  →  sdk
```

O `rtld` (runtime linker) é o entry point **real**:

1. mapeia a memória do processo
2. lê as tabelas de import do `main` a partir da `sdk`
3. resolve os símbolos, escrevendo os endereços no `.data` do `main`
4. chama o entry point do `main`

Os SVC estão distribuídos assim (medido com `tools/scan-svc.py`):

| módulo | SVC | hints | exclusivos |
|---|---|---|---|
| `main` | **0** | 4816 | 12907 |
| `rtld` | 4 | — | — |
| `sdk` | **62** | — | — |

O `main` não tem SVC porque **não chama o sistema**: chama a `sdk`, que
chama o sistema. Tentar correr o `main` directamente garante SIGILL ou um
fault de memória no primeiro acesso ao heap.

### Layout do espaço de endereçamento

O guest de 39 bits corre nativo: os endereços que ele calcula são os
endereços reais. Não há page tables nossas — há uma reserva com
`MAP_FIXED_NOREPLACE` no sítio certo.

```
rtld   [0x80000000, 0x80003d80)     0.0 MiB   entry point do programa
main   [0x80008000, 0x826b94f0)    38.7 MiB   25.3 MiB de folga
stack  [0x84000000, 0x84800000)     8.0 MiB
sdk    [0x85000000, 0x85c71658)    12.4 MiB
heap   [0x90000000, 0x98000000)   128.0 MiB   pedido pelo rtld via SetHeapSize
```

Porquê estes números e não outros:

- **`0x8000'0000`** é o `code_address` que o Switch dá a um processo 39-bit
  (`k_process.cpp` do Eden). A versão anterior deste motor usava
  `0x7FFF00000`, 30 GiB mais alto — e o comentário do Eden explica que
  carregar demasiado alto deixa para trás o buraco de páginas ASLR de que
  alguns programas (Skyline é o exemplo) dependem indevidamente.
- **32 KiB entre rtld e main** — o rtld tem 15 KB de imagem mas escreve
  código em runtime (tabelas de relocação, trampolins de import). Essa
  precisa de espaço abaixo de si.
- **heap longe do código** — o linker assume AddressSanLayout e viz: um heap
  que cresce para cima a partir do fim do código acabaria por invadir
  instruções.

Todos os pares de regiões são verificados na planarização, e o teste
`ia_program` também. Uma sobreposição aqui não dá erro na carga: dá
corruption silenciosa muito mais tarde, num SVC que escreve por cima de uma
instrução — o pior tipo de bug.

---

## 15. Segundo bug do carregador: gaps entre segmentos

**Este é o bug mais caro que encontrámos até agora**, e nenhum teste o
apanhou.

### O sintoma que teria aparecido

Uma imagem concatenada (`text | rodata | data | bss`) parece correcta: o
`.text` está no sítio, o entry point é válido, o patcher encontra os SVC. O
jogo arrancaria e rebentaria a seguir, sem mensagem útil.

### A causa

Os `vaddr` dos segmentos **não são contíguos**. Medido no ExeFS real:

```
main:
  text    vaddr=0x00000000  size=25084672  fim=0x017ec300
  rodata  vaddr=0x017ed000  <- gap de 3328 bytes
  data    vaddr=0x0236d000  <- gap de 784 bytes

rtld:  gap de 1536 / 736 bytes
sdk:   gap de 1920 / 3728 bytes
```

`image_size()` devolvia a **soma** dos tamanhos. A soma é menor que o espaço
virtual: os gaps desapareciam. E é a `.rodata` que fica deslocada — que é
exactamente onde vivem as tabelas de símbolos e de import que o `rtld`
percorre para resolver o `main`.

### A correcção

`image` passou a espelhar o **espaço virtual**, com os gaps a zero:

```cpp
std::uint64_t LoadedModule::image_size() const {
    std::uint64_t end = 0;
    for (int i = 0; i < 3; ++i)
        end = std::max(end, segments[i].vaddr + segments[i].size);
    return std::max(end, segments[kNsoData].vaddr + bss_size);
}
```

E cada segmento é copiado para `image[vaddr]`, não para um cursor. Copiar
`image` para `base` coloca tudo onde o linker assumiu.

O `doctor` passa a mostrar os gaps, para que isto seja visível:

```
imagem    : 40572144 bytes (espaço virtual, com gaps)
gap       : 3328 bytes antes de rodata
gap       : 784 bytes antes de data
```

E o `ia_program` verifica, lendo **do endereço do guest**, que `.text` e
`.rodata` estão nos sítios certos:

```
ok: main: rodata 3328 bytes à frente do fim do .text (o gap)
ok: main: rodata no sítio certo (0x817f5000)
```

### A lição

O teste que apanha isto não é um teste de formato — é um teste que **lê de
volta o que escreveu**, pelo mesmo caminho que o guest usaria. Os testes de
invariante (`text_size`, `image_size`, "o entry point está no .text") todos
passavam com a imagem mal montada.

---

## 16. Estado

| | |
|---|---|
| Execução nativa (guest sintético) | **validada** — 13 verificações |
| Programa multi-módulo | **validado** — 26 verificações no ExeFS real |
| Layout de 39 bits | **validado** — sobreposição verificada |
| Execução de um jogo real | **faltam os SVC que o rtld pede** |

**135 verificações, 0 falhas** (96 core + 13 exec + 26 programa).

### O que falta para correr um jogo

O `rtld` é código nativo e está pronto a correr. O que ele faz primeiro é
pedir memória ao host:

1. **`SetHeapSize`** — o rtld pede o tamanho do heap. O dispatcher responde,
   mas tem de devolver um endereço válido (implementado).
2. **`MapMemory` / `MapMemoryBlock`** — o rtld mapeia os segmentos em
   endereços que **ele** escolhe. O dispatcher tem de aceitar um endereço do
   guest e mapeá-lo. **Não implementado.**
3. **`ControlMemory`** — mudar permissões. **Não implementado.**
4. **`GetSystemTick`** — o rtld chama-o no arranque para medir o tempo.
   **Implementado**, mas falta verificar se devolve o que o guest espera.

O passo 2 é o obstáculo: `MapMemory` recebe um endereço **virtual do guest**
e tem de o mapear no espaço que já reservámos. É `mmap(MAP_FIXED_NOREPLACE)`
dentro da reserva — e tem de respeitar o alinhamento e o tamanho que o guest
pedir, não um arredondamento nosso.

---

## 17. Terceiro bug: números de serviço errados

**O mais silencioso de todos.** Nenhum sintoma, nenhum erro — só um jogo que
não arranca.

### O que estava errado

Os números de SVC foram escritos de memória em vez de copiados da fonte. Os
valores reais estão em `src/core/hle/kernel/svc.h` do Eden (`enum SvcId`):

| SVC | o que tínhamos | o que é |
|---|---|---|
| `ExitProcess` | `0x03` | **`0x07`** |
| `GetSystemTick` | `0x6E` | **`0x1E`** |
| `ConnectToNamedPort` | `0x70` | **`0x1F`** |
| `MapMemory` | ausente | **`0x04`** |
| `SetMemoryPermission` | ausente | **`0x02`** |

Só o primeiro já bastava para strangular: `svc #3` é `SetMemoryAttribute`, não
`ExitProcess`. Um guest que chama ExitProcess recebia um serviço de memória e
continuava a correr.

### Como foi encontrado

Ao implementares `MapMemory` fui ver os números no Eden, e `ExitProcess = 0x03`
não estava lá. O `ia_exec` já falhava por isto — o guest sintético usava
`svc #3` e o teste de execução já não passava. O teste estava a passar antes
porque testava o que o código fazia, não o que o guest pedia.

### A correcção

Enum reescrita a partir de `svc.h`, com a fonte citada no comentário. O enum
agora só contém números que existem no Eden.

---

## 18. O despachante deixou de desreferenciar o guest directamente

### O problema

`safe_copy_string` fazia:

```cpp
*reinterpret_cast<const std::uint8_t*>(guest_addr + i)
```

`guest_addr` é um endereço do **espaço de 39 bits do guest**. Dereferenciá-lo
directamente assume que é memória do host — o que é verdade num espaço nativo,
mas só por coincidência. Se o guest calculasse um endereço fora do mapeamento,
isto seria um SIGSEGV **dentro de um handler de sinais**, que é o pior sítio
possível para um crash: o handler não pode fazer nada de útil, e o diagnóstico
morre ali.

Havia até uma nota no código a dizer que isto estava pendente. Estava.

### A correcção

O despachante tem agora uma referência ao espaço do guest:

```cpp
void bind_memory(GuestMemory* mem) noexcept;
bool read_guest(uint64_t addr, void* dst, size_t size) const;   // público
```

E `MapMemory` (0x04) valida o endereço contra a reserva:

```cpp
if (!mem_->owns(dst) || !mem_->owns(dst + size - 1))
    return Result::Invalid;
```

Isto é a mesma razão pela qual o mapeamento usa `MAP_FIXED_NOREPLACE`: um
endereço errado tem de ser um erro **visível**, não uma corruption silenciosa.

### MapMemory é o serviço que desbloqueia o rtld

O `rtld` mapeia os segmentos do `main` e da `sdk` em endereços que ele próprio
escolhe. Num guest nativo, "mapear" é um no-op: a memória dentro da reserva já
é do processo e já é acessível. O que importa é a **validação** — e o contador
`map_memory_calls()` é a métrica de progresso:

> o rtld pediu 47 mapeamentos → passou do loader para o código do jogo.

---

## 19. Estado actualizado

| Componente | Estado |
|---|---|
| Execução nativa (guest sintético) | **validada** — 13 verificações |
| Programa multi-módulo | **validado** — 26 verificações no ExeFS real |
| Layout de 39 bits | **validado** — sobreposição verificada |
| Números de SVC | **corrigidos** contra `svc.h` |
| Despachante ligado ao espaço do guest | **feito** |
| `MapMemory` / `UnmapMemory` | **implementados** |
| `SetHeapSize` a devolver um endereço | **falta** — devolve 0 |
| `QueryMemory` | **falta** — o rtld consulta permissões |
| `WaitSynchronization` | **falta** — o rtld espera pelo loader |

**139 verificações, 0 falhas** (100 core + 13 exec + 26 programa).

### O caminho até ao jogo a correr

O `rtld` está pronto. Faltam três serviços, por ordem de bloqueio:

1. **`SetHeapSize`** tem de devolver o **endereço base do heap** em x0, não 0.
   O rtld escreve o heap em `endereço_devolvido`. Devolver 0 faz o heap
   colidir com a página zero — e o primeiro acesso do guest a 0 é um fault.
2. **`QueryMemory`** — o rtld pergunta que permissões tem uma página antes de
   mapear por cima. Sem resposta, ou recusa, ou adivinha.
3. **`WaitSynchronization`** — o rtld espera pelo thread que carrega os
   módulos. Precisa de um evento.

Depois disso o primeiro `svc_connect_port("nvdrv")` aparece, e aí o jogo pede
a GPU. É o ponto onde o `Maxwell→Valhall` entra — e onde a telemetria passa a
dizer se o gargalo é CPU, GPU ou sincronização.

---

## 20. SetHeapSize: tamanho à entrada, endereço à saída

Quarto bug, mesmo padrão: **uma suposição em vez de uma medição**.

`SetHeapSize(u64 size) -> u64 heap_base`. A função guardava o tamanho e
devolvia `0` em x0 — partindo a premissa de que x0 devolvia o valor antigo.

O que acontece com um 0: o rtld trata `0` como endereço base do heap, escreve
ali, e o primeiro acesso do guest a `0` é um SIGSEGV. O sintoma seria um
crash num endereço que não pertence a nada, sem nenhum rasto de quem o pediu.

Correcção:

```cpp
const std::uint64_t requested = ctx.x[0];   // entrada: tamanho
if (requested == 0)
    return Result::Invalid;
heap_size_ = requested;
ctx.x[0]   = heap_base_;                    // saída: ENDEREÇO
```

E o teste passou a verificar as duas coisas que não eram iguais:

```
ok: SetHeapSize devolve Success
ok: heap size registado
ok: SetHeapSize devolve o ENDEREÇO do heap em x0, não 0
ok: o endereço do heap nunca é 0 (o primeiro acesso faultaria)
ok: SetHeapSize com size=0 é recusado
```

---

## 21. O padrão dos quatro bugs

Todos os quatro — gaps entre segmentos, base 30 GiB acima, números de SVC
errados, `SetHeapSize` a devolver 0 — são a mesma coisa: **um valor escrito de
memória em vez de medido na fonte**.

| | o que foi suposto | o que é | onde estava certo |
|---|---|---|---|
| gaps | segmentos contíguos | 3328/784 bytes de gap | cabeçalho do NSO |
| base | `0x7FFF00000` | `0x8000'0000` | `k_process.cpp` |
| SVC | `ExitProcess = 0x03` | `0x07` | `svc.h` |
| heap | devolve o valor antigo | devolve o endereço | ABI do Switch |

Em todos os quatro, o sintoma era o mesmo: **o guest não arranca, e a causa
não é óbvia**. Nenhum produziria uma mensagem de erro — produciriam um jogo
que faz nada, ou rebenta num endereço arbitrário.

### A regra que daí sai

Um valor que o motor depende tem de vir de uma de duas fontes:

1. **a fonte** (`svc.h`, o cabeçalho do NSO, `k_process.cpp`) — copiado, com
   citação no comentário; ou
2. **uma medição** — lido do ficheiro real e comparado.

Um terceiro caminho — lembrar — não funciona, e é o que produziu 4 bugs em 4
lugares. Os testes apanharam-nos todos, mas **ninguém apanhou um valor errado
por ser errado** — apanharam-nos porque verify coisas que se esperava que
dessem errado (um `.text` no sítio certo, um trap que não mata o processo).

O teste que teria apanhado o `SetHeapSize` a devolver 0 é um que verifica
"o endereço é utilizável", não "a função devolveu Success".

---

## 22. Instrumentação: traçar o arranque real

O guest sintético (`ia_exec`) valida o mecanismo. Não diz o que um jogo real
precisa. Para isso construí `ia_boot`, que corre o **rtld real** e reporta.

O que se aprendeu ao construir isto:

### 1. `SvcContext` só tinha `x[6]` — e eu lia `x[18]`

Para diagnosticar a stack canary li `ctx.x[18]`. `x` tem **6** entradas.
`x[18]` é memória a seguir ao struct — que, num processo com outros dados,
devolve valores plausíveis e completamente falsos.

Os registos "conter" ASCII e pareceram uma pista. Não eram do guest: eram do
meu próprio heap. `SvcContext` passou a ter `gpr[31]`, preenchido pelo handler
a partir do `ucontext`.

**Um valor que parece um dado pode ser uma leitura fora dos limites.** A
diferença entre "o guest leu isto" e "eu li o sítio errado" só se vê quando se
confere a origem.

### 2. Amostragem de PC com temporizador

Um guest em ciclo **de código puro** não levanta traps. Sem amostragem só se
sabe que "não voltou" — o sintoma mais opaco que existe.

`SIGALRM` com `setitimer` entrega um sinal ao processo; como o guest corre na
sua própria stack, o handler vê o contexto **do guest**. É o mesmo mecanismo
dos SVC por um caminho diferente:

```
[sample 1] pc=0x800000f8 x0=0xf801 x19=0x0 x29=0x0 sp=0x847ffff0
[sample 2] pc=0x800000f8 ...
```

Oito amostras, o mesmo PC. Era um `b .` — branch para si próprio. Um spin.

### 3. Desassemblar sem debugger

`ia_boot --raw <ficheiro>` escreve a imagem do entry num ficheiro e o
`objdump -b binary` desassembla. Mais fiável do que decodificar AArch64 à mão,
e fica registado para o próximo.

---

## 23. O contrato de entrada do guest — o bug que custou mais

### O que passou a acontecer

Antes, o guest **girava para sempre sem levantar uma única trap**. Era o
sintoma mais opaco possível: nada para diagnóstico, nada para corrigir.

A amostragem mostrou `pc=0x800000f8` fixo. O disassembly do entry:

```asm
0x00:  b    0x8              ; salta o pool de literais
0x08:  cmp  x0, #0           ; <<< x0 É UM ARGUMENTO
0x0c:  b.ne 0xc4             ; e não o endereço do entry
0x10:  mov  w19, w1
```

O entry **testa o registo x0 antes de o usar**. E o nosso trampoline saltava
com `br x0` — que deixa x0 igual ao endereço do entry. O `cmp` falhava, o
guest ia pelo caminho errado e acabava num `b .`.

### O que o Eden faz

`ResetThreadContext64` (k_thread.cpp:58):

```cpp
ctx = {};                    // TODOS os registos a zero
ctx.r[0]  = arg;             // arg = 0  (KProcess::Run passa 0)
ctx.r[18] = Random64() | 1;  // platform register
ctx.pc    = entry_point;
ctx.sp    = stack_top;
```

O `ctx = {}` é a parte que importa. **Todos os registos começam a zero**, e um
entry de jogo assume isso: lê registos antes de os escrever.

### A correcção

`ia_call_guest` limpa x0-x30 (menos x18, que é a canary) antes de saltar, e
guarda o endereço de entrada **na stack do guest** — o único sítio que sobrevive
ao cleaning:

```asm
sub  x1, x1, #32
str  x0, [x1]          /* entrada */
str  x4, [x1, #16]     /* retorno de segurança */
mov  sp, x1
mov  x18, x2           /* platform register */
mov  x1, xzr           /* ... limpar tudo ... */
mov  x30, xzr
ldr  x0, [sp]
add  sp, sp, #16
br   x0
```

**Custo: ~35 instruções por ENTRADA no guest**, não por SVC. Face a guardar
31 GPRs por chamada de serviço, é irrelevante.

### O terceiro bug: x18, a stack canary

`x18` é o *platform register* da AAPCS64 — o registo que o runtime reserva, e
que o guest usa como stack canary. Sem o inicializar, o primeiro
`__stack_chk_fail` chama `svc #ReturnFromException` (0x28).

Foi **o primeiro SVC que o rtld real pediu**, com um x0 que não era endereço
de 39 bits. O sintoma não aponta para um registo em falta.

---

## 24. O rtld real: onde está

Com os registos correctos, o guest deixa de girar e passa a fazer:

```
[trap #1] sig=5 pc=0x8000170c insn=0xd4200500 (BRK = SVC patchado)
[svc #1] 0x28 Svc#40   pc=0x8000170c x0=0xf801 x18=<canary> x19=0x0 x29=0x0
```

O código em volta:

```asm
0xe0:  adrp x2, 0x3000
0xe4:  ldr  x2, [x2, #432]    ; ponteiro de função de uma GOT
0xe8:  cbz  x2, 0xf0          ; se for nulo, cai no caminho directo
0xec:  br   x2
0xf0:  mov  x0, #0xf801       ; número mágico
0xf4:  bl   0x170c            ; chama o stub do SVC
0xf8:  b    .                 ; (e gira aqui se o stub regressar)
0x170c: brk #0x28             ; == svc #0x28, escrito pelo patcher
1710:  ret
```

Isto é o **padrão de hypercall**: tenta um hook instalado numa GOT; se não
estiver instalado, chama o SVC directamente com um número mágico.

### O que falta resolver

`x0 = 0xf801` é o número mágico passado ao SVC 0x28. Não está no Eden
(`grep 0xf801` não devolve nada em `core/arm/nce` nem em `core/hle`).

Próximo passo exacto: determinar o que `svc #0x28` com `x0 = 0xf801` deve
fazer. Três hipóteses, por ordem de probabilidade:

1. **É um detecção de hypervisor.** O guest chama um SVC que o hardware real
   trataria de uma maneira e o emulador de outra, e usa o resultado para
   decidir que caminho seguir. Se for isso, a resposta é um valor de retorno
   específico — não `Success`.
2. **O número 0x28 está deslocado.** Se o patcher ou a tabela do Eden usarem
   um enum diferente do do jogo, todos os SVC ficam com um offset constante.
   Teste: correr com o número correcto e ver se o guest avança.
3. **O slot da GOT (0x30B0) devia estar preenchido** e está a zero porque a
   relocação não foi feita. Se for o caso, o ramo `br x2` é o caminho correcto
   e o SVC nem devia ser chamado.

A terceira é a mais provável e a mais fácil de testar: ler o conteúdo de
0x30B0 depois do carregamento e ver se o Eden o preenche.

---

## 25. `x0 = 0xf801` não é um hypercall — é o registo de handlers de excepção

### O que o disassembly mostrava

```asm
0xe0:  adrp x2, 0x3000
0xe4:  ldr  x2, [x2, #432]    ; lê um slot
0xe8:  cbz  x2, 0xf0
0xec:  br   x2
0xf0:  mov  x0, #0xf801
0xf4:  bl   0x170c
```

A leitura natural é "se houver um hook instalado, chama-o; senão chama o SVC".
Parecia um hypercall. **Era uma leitura errada**, e só um teste a desmentiu.

### O teste que refutou

`ia_boot --slots` lê o slot em 0x30B0 do módulo:

```
slot 0x30b0 @0x800030b0 = 0x0000000000002610
```

Não é nulo. Se o `cbz` não pegasse, o guest saltava para `0x2610`. Dump desse
offset:

```
_ZN2nn2os6detail20UserExceptionHandlerE
```

Isto é um **nome de símbolo C++**, não código. `nn::os::detail::UserExceptionHandler`.

### O que realmente acontece

O slot não é um ponteiro de função. É um valor lido de uma zona de dados que o
linker usou como constante. O guest não salta para lá — o `cbz` pega por uma
razão que o dump não explica, ou o `adrp`/`ldr` opera sobre outro objecto.

O que fica claro, e é o que interessa:

**O rtld regista um handler de excepções do utilizador antes de correr o
jogo.** É a primeira coisa que um runtime faz. E o SVC que ele chama com
`x0 = 0xf801` está nesse caminho.

`0xf801` não é um número de serviço — é um **código de registo**. O bit alto
(`0xf8`) é uma classe, e o resto é o índice. É assim que o runtime do Switch
identifica "que handler quero instalar" numa única chamada.

### O que isto diz sobre o que falta

Não falta um SVCgenérico. Falta **o mecanismo de handlers de excepção**:
o registo, a consulta e o despacho. E isso é uma peça de sistema, não uma
tabela de serviços.

A sequência real de arranque de um título é:

1. rtld arranca
2. **rtld regista handlers de excepção** (onde estamos)
3. rtld mapeia memória para `main` e `sdk`
4. rtld resolve imports da `sdk`
5. rtld chama o entry do `main`
6. o jogo regista os seus handlers e arranca
7. o jogo pede a GPU por `ConnectToNamedPort("nvdrv")`

### Registo

Isto não é um bug do motor: é uma funcionalidade que ainda não foi
implementada. O motor despachou o SVC, registou-o, e o guest ficou à espera
de uma resposta que não veio — o que é exactamente o comportamento correcto
para um serviço por implementar.

O número de SVC que falta não está no `svc.h` do Eden porque **não é um SVC do
kernel**: é uma chamada à API de excepções do runtime (`nn::os`), que o Eden
intercepta noutro sítio (provavelmente via uma trap de execução, não via
`svc`).

**Próximo passo exacto**: procurar no Eden por onde `UserExceptionHandler` é
tratado. Se o Eden o faz com uma SVC, o número está em `svc.h` e basta
adicionar. Se o faz por outratrap, é uma peça diferente do motor.

---

## 26. Input: o DS4, e a armadilha do A/B

### O que foi medido

`ia_input` abre o DS4 real ligado ao Orange Pi 5 Pro:

```
joysticks anexados : 1
aberto    : [0] PS4 Controller
guid      : 0300d0424c050000c405000011010000
eixos     : 6
```

O GUID **bate exactamente** com o `SDL_GAMECONTROLLERCONFIG` já instalado em
`/etc/emuos/environment`. A configuração do sistema está correcta e o motor
pode usá-la sem mais.

Dispositivos suportados pelo evdev (`evtest`):

```
BTN_SOUTH  BTN_EAST  BTN_C  BTN_NORTH  BTN_WEST  BTN_Z
BTN_TL  BTN_TR  BTN_TL2  BTN_TR2
BTN_SELECT  BTN_START  BTN_MODE  BTN_THUMBL
ABS_X  ABS_Y  ABS_Z  ABS_RX  ABS_RY  ABS_RZ
ABS_HAT0X  ABS_HAT0Y
```

### A armadilha: A e B estão trocados

Um DS4 é uma **PlayStation**. Os botões estão assim:

```
        BTN_SOUTH (Cross, ✕)        BTN_EAST (Circle, ○)
              em baixo                 à direita
```

A PlayStation chama-lhes A e B. **A Nintendo também — mas com o sentido
trocado**: em Nintendo, o botão de baixo é **B** e o da direita é **A**.

O SDL segue a convenção PlayStation. O seu `A` é o `BTN_SOUTH`, que é o botão
de baixo, que na Nintendo é `B`.

Usar o A e o B do SDL directamente dá botões trocados. Numa tela de título,
"carregar A" salta a cena errada — e o utilizador conclui que o emulador está
partido, quando o problema é uma troca de dois símbolos.

`src/input.cpp` desfaz a troca **num sítio só**, para que o resto do motor nunca
tenha de saber que existe uma PlayStation por baixo:

```cpp
void Controller::apply_swap_a_b() {
    std::swap(st_.buttons[A], st_.buttons[B]);
}
```

### O eixo Y também precisa de inversão

O SDL cresce para baixo; o guest espera para cima. Sem inverter, "cima" no
guest é "baixo" — um erro que move a personagem na direcção errada sem dar
qualquer erro.

```cpp
case SDL_CONTROLLER_AXIS_LEFTY:
    st_.axis.y = -normalise(ev.caxis.value); break;
```

E os triggers **não têm centro** (0..65535, não ±32767), ao contrário dos
analógicos. Tratá-los como eixo simétrico dá um trigger que salta para metade
ao ser tocado.

### SDL e não evdev directo

O evdev daria os botões crus. O SDL acrescenta o mapeamento por guid (que já
está configurado no sistema), trata do ruído dos eixos com zona morta, e
normaliza as diferenças entre dispositivos. O trade-off é que esconde os botões
atrás da convenção PlayStation — que é precisamente o que a troca desfaz.

### Teste sem hardware

O teste da troca A/B **não precisa de um DS4 ligado**: reproduz a regra sobre
os quatro casos possíveis (nenhum, A, B, ambos) e verifica o resultado. Um
teste que só passa com o hardware plugged-in é um teste que não corre em CI.

---

## 27. Foco: a janela do jogo nunca recebia foco

### O sintoma

O emulador desenha-se no sítio certo, em ecrã inteiro, e **não responde a um
único botão**. O gamepad vai para a janela do menu.

### A causa

Em `/usr/local/bin/emuos-display`:

- `place()` move a janela para a saída e activa o ecrã inteiro — **mas não dá
  foco**.
- `focus-primary`, o único comando que focava alguma coisa, focava a janela do
  **menu** (`menu_window`), não a do emulador.

O wlroots envia teclado e gamepad para a **janela focada do seat focado**. Como
nunca se focava a janela do jogo, o input ia todo para o menu.

### A correcção

Uma linha dentro de `place()`, depois de maximizar:

```sh
"$SWAYMSG" "[con_id=$id] focus" >/dev/null 2>&1
```

A ordem importa: focar **antes** de maximizar mandava o compositor activar o
ecrã inteiro de uma janela errada quando havia várias.

Validado no Orange Pi 5 Pro:

```
janela EmuOS id: 6
$ swaymsg "[con_id=6]" focus
[ { "success": true } ]
```

### Reversibilidade

- Patch: `tools/patch_focus.py` (apenas **adiciona** linhas, nada remove)
- Backup: `/usr/local/bin/emuos-display.bak-20261018`
- Repor: `sudo cp /usr/local/bin/emuos-display.bak-20261018 /usr/local/bin/emuos-display`

---

## 28. O save mais recente

```
~/.local/state/pidroid/eden-session/user/nand/user/save/
  0000000000000000/73BACD9130B1E30130DD3F4896789AF2/01008DB008C2C000/main
  2026-10-17 23:38   1 513 139 bytes
```

Verificado: 99,6% de bytes não-zero, strings ilegíveis (assinatura de save de
Switch **encriptado**), cópia byte-a-byte idêntica para
`EdenMaliIA/saves/01008DB008C2C000/main`.

Há um segundo save, de 2026-10-17 02:59, com outro hash de título
(`F0FC9C14...`) — um segundo perfil/ saveslot.

---

## 29. O bug que destravou tudo: x0 carregado com o endereço de entrada

### O sintoma

O rtld pedia `svc #0x28` (`ReturnFromException`) com `x0 = 0xf801`, e ficava
num `b .`. Três hipóteses foram testadas e **refutadas**:

1. *É um hypercall com hook numa GOT* — o slot valia `0x2610`, que é a string
   `_ZN2nn2os6detail20UserExceptionHandlerE`, não código.
2. *É um registo de handler de excepção* — plausível, mas não testável sem o
   Eden a correr, e o directório do jogo está vazio.
3. *O número de SVC está deslocado* — os números vieram de `svc.h`, estão certos.

Todas plausíveis, nenhuma provada. O que as três partilhavam era uma coisa que
ninguuma questionava: **que o guest chegasse sequer a este código**.

### O que o `bl` revelou

Quem chama a função que contém o SVC? Ninguém — `bl 0xc4` **não existe** no
binário. A única forma de chegar lá é `b.ne 0xc4`, no offset 0x0c:

```asm
0x08:  cmp  x0, #0
0x0c:  b.ne 0xc4        ; <-- o caminho que o guest tomou
```

Isto significa **x0 != 0**. E o Eden faz `ctx = {}` com `ctx.r[0] = arg = 0`.

O nosso trampoline fazia:

```asm
mov  x0, xzr        ; x0 = 0, o argumento
ldr  x0, [sp]       ; x0 = ENTRADA     <-- SOBRESCREVE O ZERO
add  sp, sp, #16
br   x0
```

**A mesma instrução a dizer as duas coisas.** A segunda linha desfazia a
primeira. O `ldr` do endereço de entrada vinha logo a seguir ao `mov` que
punha o argumento, e ninguém notou porque ambos pareciam correctos isoladamente.

### A correcção

O destino vai em **x17** (IP1), que a AAPCS64 declara descartável — qualquer
código tem de assumir que o chamador o estragou. É o registo mais expênsico de
todos, e portanto o mais seguro para transportar o destino durante o cleaning.

```asm
mov  x0,  xzr      /* o argumento: 0 */
ldr  x17, [sp]     /* endereço de entrada */
add  sp,  sp, #16
br   x17
```

### O que mudou

**O primeiro SVC deixou de ser `ReturnFromException` e passou a ser
`QueryMemory`** — que é o primeiro serviço que um rtld pede:

```
[svc #1] 0x06 QueryMemory  x0=0x847fff68 x18=0x800003c0
                      x19=0x80000000 x29=0x847fffe0 x30=0x800004dc
```

Todos os registos passaram a fazer sentido: `x19` é a base do rtld, `x29` o
frame pointer, `x30` um endereço de retorno **no guest**. Antes eram lixo.

### A lição, pela terceira vez

Este é o **sétimo** bug do mesmo padrão: um valor suposto em vez de medido.

| | o que se supôs | o que é |
|---|---|---|
| gaps | segmentos contíguos | 3328 bytes |
| base | `0x7FFF00000` | `0x8000'0000` |
| SVC | `ExitProcess = 0x03` | `0x07` |
| heap | devolve o valor antigo | devolve o endereço |
| `BRK` | trocar a assinatura | reconstruir do immediate |
| registos | `br x0` chega | `cmp x0,#0` decide o caminho |
| **x0** | **é o destino** | **é um argumento** |

E a lição de como se apanhou: **perguntar "quem chegou aqui?"** em vez de
assumir que a resposta anterior estava certa. A análise das três hipóteses
erradas gastou mais tempo do que a pergunta que resolveu o problema.

---

## 30. `QueryMemory` implementado — e um terceiro bug de escrita

### O serviço

`svcQueryMemory(out_info, out_page, address)`. É o **primeiro** serviço que o
rtld pede, e sem resposta o arranque para ali.

Layout exacto, copiado de `Svc::MemoryInfo` (`svc_types.h:478`):

```cpp
u64 base_address;  u64 size;
MemoryState state;        // Free=0, Io=1, Static=2, Code=3,
MemoryAttribute attribute;//   CodeData=4, Normal=5, ...
MemoryPermission permission;
u32 ipc_count;  u32 device_count;  u32 padding;   // 40 bytes no total
```

Sem tabelas de páginas no guest nativo, a resposta vem do **layout**: o bloco
é a reserva do espaço do guest, com `state = Normal` e permissões
`Read|Write|Execute`. Fora da reserva, `Free` — que é o que o kernel do Switch
devolve para memória não mapeada, e é a resposta que o guest sabe tratar.

### O bug: escrever no host em vez do guest

`svc_connect_port` fazia:

```cpp
*reinterpret_cast<std::uint32_t*>(ctx.x[0]) = h;
```

Isto **passa os testes** — nos testes, `x0` é um ponteiro do host, e funciona.
Em produção, `x0` é um endereço de 39 bits do guest, e a escrita vai para o
sítio errado — ou pior, para memória viva do processo do host.

É a mesma classe de erro do `x[18]`: **um teste que usa ponteiros do host
enquanto o código espera endereços do guest**. O teste passa, o jogo rebenta,
e o diagnóstico não aponta para o teste.

Corrigido com `write_guest()`, que valida o endereço contra a reserva:

```cpp
if (ctx.x[0] != 0 && !write_guest(ctx.x[0], &h, sizeof(h)))
    return Result::Invalid;
```

**Todos os SVC que devolvem dados têm de passar por `write_guest()`.**

### Onde o guest para agora

Depois do `QueryMemory(0)` — consulta o endereço **zero**, que está fora da
reserva, e a resposta é `Free` — o guest fica parado em:

```asm
704:  ret
708:  b  0x708      <-- aqui
70c:  b  0x70c
```

`0x708` e `0x70c` são **`__builtin_unreachable()`**: o epílogo da função foi
substituído por branches infinitos porque o compilador sabe que ali não se
chega. O código real da função acaba em `0x704` (`ret`).

Ou seja: a função **não deve regressar**. É o mesmo padrão que o `b .` de antes,
mas agora num sítio com meaning — o guest está a sair por um caminho que não
devia ser alcançado, o que significa que **uma das respostas que demos está
errada**.

Candidatos, por ordem:

1. **`QueryMemory(0)` devolve `Free` mas o guest esperava `Success`.** O Eden
   lança `R_THROW(ResultInvalidHandle)` se o processo não existe; para um
   endereço não mapeado devolve `Free` **e** `Success`. Se devolvemos `Free`
   como estado mas o resultado do SVC é que o guest lê, pode ser esse o desvio.
2. **Falta `svcSetHeapSize`** a seguir, e o guest trata-o como MUST.
3. **A `MemoryInfo` precisa de mais campos** — o `padding` pode não ser zero.

### O trampoline de paragem

O tracer ganhou um mecanismo de paragem por alarme: quando o guest fica em
código (um spin não levanta traps nenhuma), uma rotina salta para um
`brk #0xFFFC` que o motor reconhece como paragem. Sem isto, um spin silencioso
truncava a saída e perdia-se o relatório — que é o único rasto do que
aconteceu.

---

## 31. A ABI do retorno: o bug que unlockou a cadeia

### O sintoma

Depois do `QueryMemory` o guest saltava para `0x708`, que é
`__builtin_unreachable()` — o epílogo de uma função substituído por branches
infinitos porque o compilador sabe que ali não se chega.

### O que o código do rtld diz

Lido do `rtld` real (offset 0x4d0):

```asm
4d0:  mov   x1, sp          ; &flags
4d4:  mov   x2, x20         ; endereço a interrogar
4d8:  bl    0x16e4          ; QueryMemory
4dc:  cbnz  w0, 0x708       ; <<< Result != 0 -> UNREACHABLE
4e0:  ldr   w8, [sp, #32]   ; lê um campo da MemoryInfo
4e4:  cmp   w8, #0x5        ; compara com 5 (CodeData)
4e8:  b.ne  0x590
```

**`cbnz w0` — se o Result for diferente de zero, o guest aborta.** E o nosso
Result nunca era zero.

### Os dois bugs que faziam isso

**1. x0 nunca recebia o Result.** O handler fazia:

```cpp
uc->uc_mcontext.regs[0] = c.ret;          // Result
for (int i = 0; i < 6; ++i)
    uc->uc_mcontext.regs[i] = c.x[i];     // SOBRESCREVE regs[0]
```

O `c.ret` era escrito e imediatamente apagado por `c.x[0]` — que para a maioria
dos serviços é o **endereço de saída**, não o retorno. O guest via um endereço
onde esperava um Result.

**`x0` é o registo de retorno na ABI do Switch.** Não se copia de `c.x`:

```cpp
for (int i = 1; i < 6; ++i)      // x1-x5: canais de saída
    uc->uc_mcontext.regs[i] = c.x[i];
uc->uc_mcontext.regs[0] = c.ret; // x0: o retorno
```

**2. `dispatch()` deitava fora os valores de retorno.** No fim fazia
`ctx.ret = resultado` — mas `SetHeapSize`, `GetSystemTick` e `CloseHandle`
devolvem um **valor**, não um Result, e escrevem-no em `ctx.ret`. A escrita
final apagava-o.

Resolvido com um sinalizador explícito:

```cpp
if (!ctx.has_ret)
    ctx.ret = static_cast<std::uint64_t>(r);
```

**Este é o padrão que os testes escondem:** o `ia_exec` usava um guest
sintético com `svc #7` e verificava `exit_code` — que vem de `ctx.x[0]`, não
de `ctx.ret`. Passava sem exercitar nenhum dos dois caminhos.

### O que destravou

O guest passou a fazer **quatro serviços** e a imprimir:

```
[svc #1] 0x06 QueryMemory        x0=0x847fff68 x1=0x847fff60 x2=0x0
[svc #2] 0x27 OutputDebugString  x0=0x800027df x1=0x1b
[svc #3] 0x27 OutputDebugString  x0=0x8000263a x1=0x1c
[svc #4] 0x27 OutputDebugString  x0=0x800027fb x1=0x2
```

E as strings, lidas do binário, são:

```
"[rtld] Unresolved symbol: '"
"_ZN2nn4init5StartEmmPFvvES2_"
"'\n"
```

Ou seja, o rtld diz exactamente:

```
[rtld] Unresolved symbol: '_ZN2nn4init5StartEmmPFvvES2_'
```

Isto é um **diagnóstico real do guest**, não uma inferência nossa. De "gira
para sempre sem dizer nada" para "diz o que não encontra".

### A ABI de `OutputDebugString` (x0 = nome, x1 = tamanho)

```
x0 = 0x800027df  ->  "[rtld] Unresolved symbol: '"
x1 = 0x1b        ->  27 caracteres
```

Confirmado pelo conteúdo: a string em 0x27df tem exactamente 27 bytes. A
assinatura oficial (`svcDebugOutputString(name, size)`) bate.

---

## 32. Onde está agora: falta a metadata dos módulos

### O símbolo existe

```
rtld   15 782 bytes   1 ocorrência de _ZN2nn4init5StartEmmPFvvES2_
main   40 572 144     0 ocorrências   <-- NÃO ESTÁ AQUI
sdk    13 047 384     1 ocorrência  ->  0x82db7964 no guest
```

O símbolo **nn::init::Start** (o entry point do `main`) está na `sdk`, e o
rtld não o encontra.

### Porquê

O rtld tem strings que mostram o mecanismo:

```
__nnDetailNintendoSdkRuntimeObjectFile
__got_start   __got_end
__rela_dyn_start   __rela_dyn_end
__rela_plt_start   __rela_plt_end
```

O rtld resolve símbolos percorrendo a **GOT** e as **tabelas de relocação** de
cada módulo, e encontra os limites dessas secções pela *metadata do objecto* —
o cabeçalho NSO, que dá `api_info`, `dynstr` e `dynsym`.

**Essa metadata não está no espaço do guest.** O Eden também não a escreve
(`codeset.memory` é só os segmentos descomprimidos), o que significa que o
Eden resolve isto por outro caminho — ou também não resolve e o `main` arranca
por via diferente.

### O bloco de argumentos

O Eden reserva **0x9000 bytes** no fim dos dados do rtld
(`NSO_ARGUMENT_DATA_ALLOCATION_SIZE`). Para `program_args` isso é absurdo — um
conjunto de argumentos não ocupa 36 KB. O bloco é maior porque o layout real é:

```cpp
struct NSOArgumentHeader { u32 allocated_size; u32 actual_size; u8 pad[0x18]; };
// seguido de: contagem de módulos, cabeçalhos NsHeaderModule por módulo,
//              depois as strings de argv/environ.
```

O Eden escreve o cabeçalho e os argumentos, mas **não** a lista de módulos.

### Próximo passo exacto

Construir o bloco de argumentos completo:

1. `allocated_size` / `actual_size` (já feito)
2. contagem de módulos
3. por módulo: o cabeçalho NSO (magic, segmentos, `dynstr_offset/size`,
   `dynsym_offset/size`, `api_info_offset/size`, `build_id`)
4. depois: `argv`/`environ`

Os campos do cabeçalho NSO já estão na nossa `NsoHeader` — estão a ser lidos e
descartados. Falta escrevê-los no bloco.

Isto é trabalho de estrutura de dados, não de investigação: o formato está no
`nso.h` do Eden e as estruturas já existem no nosso loader.

---

## 33. Medido: as tabelas de símbolos estão todas carregadas

Antes de construir a metadata dos módulos, medi se os dados estavam lá.
Distingue dois diagnósticos opostos: "os dados não foram carregados" (bug de
carregamento) e "os dados estão lá mas ninguém sabe onde" (falta metadata).

```
rtld   imagem=15 782 bytes
    api_info  off=0x00000000 size=0x00000000
    dynstr    off=0x00000610 size=0x0000015a
    dynsym    off=0x00000430 size=0x000001e0   -> 20 entradas

main   imagem=40 572 144 bytes
    api_info  off=0x00b7f9cc size=0x000002ff
    dynstr    off=0x002aba88 size=0x00008d96
    dynsym    off=0x002a65b0 size=0x000054d8   -> 905 entradas

sdk    imagem=13 047 384 bytes
    api_info  off=0x006820bc size=0x0000008d
    dynstr    off=0x001ac318 size=0x002e410b
    dynsym    off=0x001192d0 size=0x00093048   -> 25 091 entradas
```

Todos os offsets dentro das respectivas imagens, e os tamanhos de `dynsym` são
múltiplos de 24 (o tamanho de uma entrada de símbolo ELF64). **Os dados estão
carregados e correctamente endereçados.**

O `dynsym` do `main` — 905 entradas — é exactamente a tabela de onde o rtld
tira os símbolos de que o `main` precisa. Está lá.

### A estrutura está certa (verificada contra a fonte)

O Eden declara os campos como `RODataRelativeExtent { u32 data_offset; u32
size; }` — o mesmo layout de dois `u32` que nós lemos. A única diferença é o
nome: `dynsyn_extent` no Eden, `dynsym_offset` connosco. É o mesmo campo (a
Nintendo chama-lhe "dynsyn" por um acidente histórico do SDK).

**A oitava hipótese refutada.** Eu suspeitei de um erro de layout. Não havia.

### O que o rtld procura

As strings do rtld dizem o mecanismo:

```
__nnDetailNintendoSdkRuntimeObjectFile
__got_start  __got_end
__rela_dyn_start  __rela_dyn_end
__rela_plt_start  __rela_plt_end
```

O rtld:
1. procura o bloco marcado por `__nnDetailNintendoSdkRuntimeObjectFile`
2. lê nele a lista de módulos (cada um com base, tamanho e offsets da GOT,
   `rela_dyn` e `dynsym`)
3. resolve os símbolos percorrendo essas tabelas

**O passo 1 falha.** Esse bloco não existe no espaço do guest.

O Eden também não o escreve — `grep NintendoSdkRuntimeObjectFile` em todo o
`~/eden/src/` não devolve nada. Duas leituras possíveis:

- o Eden resolve isto por outro caminho (e `nn::init::Start` é encontrado de
  outra forma), ou
- o Eden também não resolve, e o `main` arranca por via diferente.

Sem o jogo instalado no Eden (`~/user/sdmc/atmosphere/contents/01008DB008C2C000`
está vazio) não há como comparar. **Isto é uma limitação real do que posso
verificar aqui, e digo-lo em vez de fingir que sei.**

### O que falta, com precisão

Um bloco de headers NSO no espaço do guest, com o marcador
`__nnDetailNintendoSdkRuntimeObjectFile`, contendo por módulo:
`base`, `size`, `bss_size`, `module_id`, e os offsets de `api_info`,
`dynstr`, `dynsym`.

**Toda a informação necessária já está no nosso loader** — está nos
`NsoHeader` que já lemos. O que falta é escrevê-la no bloco de argumentos de
0x9000 bytes que o Eden reserva no fim dos dados do rtld.

O formato exacto do bloco (`NsModInfo`) não está no Eden — porque o Eden não o
precisa. Terá de vir da especificação do kernel do Switch, não do código de
referência. É a primeira peça deste projecto que **não tem uma referência
local para copiar**, e é por isso que não a implementei às cegas.

---

## 34. Como o rtld encontra os módulos: `MOD0`, não uma lista

O §33 deixou-me à beira de construir a coisa errada. A via findings foi:
buscar a estrutura do bloco de módulos. A resposta certa veio de **duas
fontes que se confirmam uma à outra**.

### O que o rtld faz (medido no binário)

Descompilei o `rtld` do jogo com `objdump -b binary -m aarch64` e segui a
variável de `x20` desde o início da função:

```
0x800004a4  mov  x20, xzr          ; o primeiro endereço a interrogar é 0
0x800004d8  bl   0x800016e4        ; svcQueryMemory(x0=buf, x1=&flags, x2=x20)
0x800004e0  ldr  w8, [sp, #32]     ; permission
0x800004e4  cmp  w8, #0x5          ; tem de ser 5 = Read|Execute
0x800004ec  ldr  w8, [sp, #24]     ; state
0x800004f0  cmp  w8, #0x3          ; tem de ser 3 = Code
0x800004fc  cmp  x21, x19          ; se for o próprio rtld, saltar
0x80000504  ldr  w8, [x21, #4]     ; HeaderOffset
0x80000508  add  x23, x21, x8      ; -> ModuleHeader
0x80000510  cmp  w9, w24          ; w24 = 0x30444f4d  = "MOD0"
0x80000514  b.ne 0x8000070c        ; sem MOD0: PRESO (b .)
0x80000594  add  x8, x9, x8        ; próximo endereço = base + size
0x800005a0  b.hi 0x800004cc        ; continuar enquanto base+size > actual
```

E o stub do serviço, byte a byte:

```
0x800016e4  str  x1, [sp, #-16]!
0x800016e8  svc  #0x6
0x800016f0  str  w1, [x2]          ; PageInfo.flags volta em x1
```

**Não há lista de módulos.** O rtld varre o espaço de endereçamento com
`svcQueryMemory` e aceita uma região se — e só se — `permission == 5` e
`state == 3`. Depois lê um cabeçalho `MOD0` no início dela.

### O formato (switchbrew, e confirmado pelo código acima)

`ModuleHeaderLocation` na base da região, `ModuleHeader` ao alcance de um
offset. Comparado com o dump das imagens descomprimidas, bate certo:

```c
struct ModuleHeaderLocation {   // base da região
    u32 version;        // 0
    u32 HeaderOffset;   // 0x8 no main e na sdk
    u32 VersionOffset;
};
struct ModuleHeader {           // base + HeaderOffset
    u32 signature;      // "MOD0"
    u32 DynamicOffset;
    u32 BssStartOffset;
    u32 BssEndOffset;
    u32 ExceptionInfoStartOffset, ExceptionInfoEndOffset;
    u32 ModuleOffset;   // "tipicamente igual à base do .bss" — e é
    ...
};
```

E o `MOD0` **já está em cada NSO**, posto lá pelo linker da Nintendo.
Medido nas imagens descomprimidas:

```
main  vaddr 0x00: 00 00 00 00  version = 0
              0x04: 08 00 00 00  HeaderOffset = 8
              0x08: 4d 4f 44 30  "MOD0"
              0x0c: DynamicOffset     0x0c: f0 47 49 02
              0x10: BssStartOffset    0x10: f8 9f 4b 02
              0x14: BssEndOffset      0x14: 40 db 7f 02
              0x20: ModuleOffset      0x20: f8 9f 4b 02  == .bss base
              0x30: ff 43 02 d1       <-- o código
```

`HeaderOffset == 8` e o `.text` começa em `0x30`: o `ModuleHeaderLocation`
está no início da imagem, no mesmo segmento R-X que o `.text`. **O módulo é
descoberto porque o seu `.text` é Code/R-X** — nada mais é preciso.

### Os três bugs que estavam por trás do "Unresolved symbol"

**Nono: `state` e `permission` errados.** O `svc_query_memory` devolvia
`state = 5` (Normal) e `permission = 7` (RWX) para a reserva inteira. O rtld
viajava uma região e não aceitava nenhuma. Devolvia uma reserva, não uma
região.

**Décimo: o `.bss` ficava fora do layout.** `plan_layout` avançava o cursor
pelo tamanho da *imagem*, mas o `.bss` (0x3444f0 bytes no `main`) ocupa
memória logo a seguir. O `.bss` do `main` escrevia por cima da `sdk` — que o
layout põe a seguir. Silent corruption, do tipo que só aparece muito depois.

**Décimo primeiro: buracos na tabela.** Mesmo com tudo certo, a varrura
parava no primeiro endereço não descrito: recebe "Free" com `base + size = 0`, e
`0 > actual` é falso. A tabela tem de ladrilhar o espaço **sem buracos**, e a
primeira região tem de começar em **0**.

### Uma fiction, dita como fiction

A reserva começa em `0x8000'0000` e nada abaixo disso é mapeado. Mas o rtld
interroga o endereço 0 primeiro, e sem nada lá a varrura não arranca. Descrevo
`[0, 0x8000'0000)` como uma região `Static`/`R--`.

Não é memória que tenhamos. É o mínimo que torna a resposta honesta, e não
produz candidatos a módulo porque não satisfaz os dois campos que o rtld
exige. Está escrito no código, não escondido.

## 35. O guest le o seu TLS de `TPIDR_EL0`, e esse registo é nosso

Com os módulos carregados, o `Unresolved symbol` **desapareceu**. O rtld
resolveu os símbolos, chamou o SDK, e este pediu entropia:

```
#13 0x29 GetInfo  x1=0xb    (11 = RandomEntropy)
#15 0x29 GetInfo  x1=0xc    (12 = AslrRegionAddress)
```

Depois, fault em `0x82d2a908`:

```asm
mrs  x8, tpidrro_el0      ; o TLS do guest
ldr  w9, [x0]             ; um spill
ldr  x8, [x8, #0x1f8]     ; <-- fault
```

É o mesmo problema de sempre, com outro nome: o guest corre **nativo**, e o
registo que o kernel do Switch programa por thread é, aqui, o `fs` da thread
do Linux.

### A solução limpa não existe neste kernel

Trocar o registo seria o correcto e o motor já o faz para o `x18`. Medi
primeiro, e o kernel recusa:

| via | resultado |
|---|---|
| `mrs x0, tpidrro_el0` | funciona — devolve **0** |
| `msr tpidrro_el0, x0` | **SIGILL** |
| `arch_prctl(ARCH_SET_FS)` | **-1** |

Num programa de dez linhas, no próprio Pi. Não há caminho de escrita.

### O que fica: emular a leitura

`MSR` não é possível, `MRS` é. E o `mrs` não levanta trap — lê 0 e segue —,
por isso o fault chega **duas instruções depois** (medido: o `ldr w9` de spill
entra entre o `mrs` e o load que falha).

O motor procura, até quatro instruções para trás, um `MRS <Xt>, TPIDRRO_EL0`
cujo `Xt` ainda valha 0, escreve nele a base do TLS do guest, e recomeça a
execução a partir dessa instrução. A condição `valor == 0` é o que impede o
ciclo e limita o alcance: se o registo já mudou, a causa não era o TLS e o
fault é reportado como fault.

Os encodings foram montados com o assembler e lidos do `.o`, não escritos a
olho:

```
mrs x0, tpidrro_el0   ->  d53bd060
mrs x8, tpidrro_el0   ->  d53bd068
mrs x5, tpidr_el0     ->  d53bd045      <- não colide
msr tpidrro_el0, x0   ->  d51bd060      <- não colide
```

A máscara tem de apagar **só** os bits [4:0]. A primeira versão usou
`0xfffffc1f`, que também apaga [5:6] — e `0xd53bd068 & 0xfffffc1f` dá
`0xd53bd040`, que não bateva com o alvo. A emulação nunca disparava e o sintoma
era o fault de sempre, sem nada novo. Foi uma máscara escrita a olho; apanhou-a
olhar para os bytes.

Continua a ser execução nativa: o CPU não sabe que isto aconteceu.

### Onde isto está agora

Com a emulação, o guest passa `0x82d2a908` e falha a seguir, em
`0x82d2a910: ldur w8, [x8, #4]`. Ou seja: **o mecanismo está certo e falta o
conteúdo.** O bloco de TLS do guest precisa de ser inicializado como o kernel
do Switch faria — não um bloco de zeros.

É o próximo trabalho, e é o primeiro em que a resposta não está nem no Eden
nem num binário que se possa descompilar: é a convenção de TLS da Nintendo.
